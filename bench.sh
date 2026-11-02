#!/usr/bin/env bash
N=${1:-1048576}
REPS=${2:-3}
CORES=$(nproc)

THREADS="1 2 3 $((CORES>4 ? CORES-1 : 2)) $CORES 16 128 1024"

echo "n,threads,seq_ms,par_ms,speedup,eff"
for t in $THREADS; do
    best=""
    for _ in $(seq "$REPS"); do
        line=$(./bitonic -n "$N" -t "$t" -q) || exit 1
        if [ -z "$best" ] || awk -F, -v a="$line" -v b="$best" 'BEGIN{split(a,x,",");split(b,y,",");exit !(x[4]<y[4])}'; then
            best=$line
        fi
    done
    echo "$best"
done