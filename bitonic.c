#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int *a;
    size_t n;
    int threads;
    int hold;
    pthread_barrier_t bar;
} Ctx;

typedef struct {
    Ctx *c;
    int id;
} Arg;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static inline void step(int *a, size_t i, size_t j, size_t k) {
    size_t l = i ^ j;
    if (l > i) {
        int asc = ((i & k) == 0);
        if (asc == (a[i] > a[l])) {
            int t = a[i]; a[i] = a[l]; a[l] = t;
        }
    }
}

static void bitonic_seq(int *a, size_t n) {
    for (size_t k = 2; k <= n; k <<= 1)
        for (size_t j = k >> 1; j > 0; j >>= 1)
            for (size_t i = 0; i < n; i++)
                step(a, i, j, k);
}

static void *worker(void *p) {
    Arg *arg = p;
    Ctx *c = arg->c;
    size_t n = c->n;
    size_t lo = n * (size_t)arg->id / c->threads;
    size_t hi = n * (size_t)(arg->id + 1) / c->threads;

    for (size_t k = 2; k <= n; k <<= 1) {
        for (size_t j = k >> 1; j > 0; j >>= 1) {
            for (size_t i = lo; i < hi; i++)
                step(c->a, i, j, k);
            pthread_barrier_wait(&c->bar);
        }
    }
    if (c->hold > 0) sleep(c->hold);
    return NULL;
}

static int bitonic_par(int *a, size_t n, int threads, int hold) {
    Ctx c;
    c.a = a; c.n = n; c.threads = threads; c.hold = hold;
    pthread_barrier_init(&c.bar, NULL, threads);
    pthread_t *th = malloc(sizeof(pthread_t) * threads);
    Arg *args = malloc(sizeof(Arg) * threads);
    if (!th || !args) return -1;

    int created = 0;
    for (int i = 0; i < threads; i++) {
        args[i].c = &c;
        args[i].id = i;
        if (pthread_create(&th[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed at %d\n", i);
            return -1;
        }
        created++;
    }
    for (int i = 0; i < created; i++) pthread_join(th[i], NULL);

    pthread_barrier_destroy(&c.bar);
    free(th);
    free(args);
    return 0;
}

static uint64_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static int is_sorted(const int *a, size_t n) {
    for (size_t i = 1; i < n; i++)
        if (a[i - 1] > a[i]) return 0;
    return 1;
}

static void usage(const char *p) {
    fprintf(stderr,
            "Использование: %s -n <размер> -t <потоки> [-s seed] [-w сек] [-q]\n"
            "  -n  размер массива (будет дополнен до степени двойки)\n"
            "  -t  максимальное число потоков\n"
            "  -s  seed генератора (по умолчанию 42)\n"
            "  -w  держать потоки N секунд после сортировки (для ps/htop)\n"
            "  -q  вывод одной CSV-строкой: n,threads,seq_ms,par_ms,speedup,eff\n",
            p);
}

int main(int argc, char **argv) {
    size_t n_user = 1 << 20;
    int threads = 4, hold = 0, quiet = 0;
    uint64_t seed = 42;

    int opt;
    while ((opt = getopt(argc, argv, "n:t:s:w:q")) != -1) {
        switch (opt) {
        case 'n': n_user = strtoull(optarg, NULL, 10); break;
        case 't': threads = atoi(optarg); break;
        case 's': seed = strtoull(optarg, NULL, 10); break;
        case 'w': hold = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default: usage(argv[0]); return 1;
        }
    }
    if (n_user < 2 || threads < 1) { usage(argv[0]); return 1; }

    size_t n = 1;
    while (n < n_user) n <<= 1;

    int *orig = malloc(n * sizeof(int));
    int *a = malloc(n * sizeof(int));
    if (!orig || !a) { fprintf(stderr, "out of memory\n"); return 1; }

    rng_state = seed ? seed : 1;
    for (size_t i = 0; i < n_user; i++) orig[i] = (int)rnd();
    for (size_t i = n_user; i < n; i++) orig[i] = INT_MAX;

    if (!quiet) {
        printf("PID=%d, n=%zu (padded %zu), threads=%d, logical cores=%ld\n",
               getpid(), n_user, n, threads, sysconf(_SC_NPROCESSORS_ONLN));
    }

    memcpy(a, orig, n * sizeof(int));
    double t0 = now_ms();
    bitonic_seq(a, n);
    double t_seq = now_ms() - t0;
    int ok_seq = is_sorted(a, n);

    memcpy(a, orig, n * sizeof(int));
    t0 = now_ms();
    if (bitonic_par(a, n, threads, hold) != 0) return 1;
    double t_par = now_ms() - t0 - (hold > 0 ? hold * 1000.0 : 0);
    int ok_par = is_sorted(a, n);

    double speedup = t_seq / t_par;
    double eff = speedup / threads;

    if (quiet) {
        printf("%zu,%d,%.3f,%.3f,%.3f,%.3f\n", n_user, threads, t_seq, t_par, speedup, eff);
    } else {
        printf("Последовательная: %.3f мс (%s)\n", t_seq, ok_seq ? "OK" : "FAIL");
        printf("Параллельная:     %.3f мс (%s)\n", t_par, ok_par ? "OK" : "FAIL");
        printf("Ускорение S = %.3f, эффективность E = %.3f\n", speedup, eff);
    }

    free(orig);
    free(a);
    return (ok_seq && ok_par) ? 0 : 2;
}