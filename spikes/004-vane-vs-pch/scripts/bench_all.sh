#!/usr/bin/env bash
# spike004 final benchmark: baseline vs PCH vs vane, median of N runs.
# All configs produce .o for the same 42 fixture TUs at -O0, -std=c++20.
cd ~/pch-planner/test/fixtures/proj || exit 1
OUT=/tmp/s004
mkdir -p $OUT/bench
FLAGS="-std=c++20 -I. -O0"
RUNS="${1:-5}"
mapfile -t TUS < <(ls tu_*.cpp | sort)
echo "fixture TUs: ${#TUS[@]}, runs: $RUNS, cores: $(nproc)"

median() {  # reads numbers on stdin, prints median
  sort -n | awk '{a[NR]=$1} END{ if(NR%2) print a[(NR+1)/2]; else printf "%.0f\n",(a[NR/2]+a[NR/2+1])/2 }'
}

compile_one() { clang++-19 -std=c++20 -I. -O0 $EXTRA -c "$1" -o "$OUT/bench/$(basename "$1").o" 2>/dev/null; }
export -f compile_one; export OUT

par() {  # $1=extra flags $2=jobs -> echo ms
  export EXTRA="$1"; local start end
  start=$(date +%s%N)
  printf '%s\n' "${TUS[@]}" | xargs -P"$2" -I{} bash -c 'compile_one "$@"' _ {}
  end=$(date +%s%N); echo $(( (end-start)/1000000 ))
}

echo "=== A. baseline (no PCH), -j32 ==="
for i in $(seq $RUNS); do par "" 32; done | tee $OUT/bench/a.txt | tr '\n' ' '; echo
echo "  median_ms=$(median < $OUT/bench/a.txt)"

echo "=== B. PCH, -j32 (build once + compile) ==="
cat > $OUT/bench/prefix.h <<'EOF'
#include "common.h"
#include "heavy_a.h"
#include "heavy_b.h"
#include "heavy_c.h"
EOF
: > $OUT/bench/b.txt
for i in $(seq $RUNS); do
  ps=$(date +%s%N)
  clang++-19 $FLAGS -x c++-header $OUT/bench/prefix.h -o $OUT/bench/prefix.pch 2>/dev/null
  pe=$(date +%s%N)
  pchbuild=$(( (pe-ps)/1000000 ))
  comp=$(par "-include-pch $OUT/bench/prefix.pch" 32)
  echo $(( pchbuild + comp )) >> $OUT/bench/b.txt
done
cat $OUT/bench/b.txt | tr '\n' ' '; echo
echo "  median_ms=$(median < $OUT/bench/b.txt) (includes PCH build each run)"

echo "=== C. vane fork, pool=32 ==="
: > $OUT/bench/c.txt
for i in $(seq $RUNS); do
  /tmp/s004/vane_spike . $OUT/bench 32 "${TUS[@]}" 2>/dev/null | grep '^total_ms=' | cut -d= -f2 | cut -d. -f1 >> $OUT/bench/c.txt
done
cat $OUT/bench/c.txt | tr '\n' ' '; echo
echo "  median_ms=$(median < $OUT/bench/c.txt)"

echo ""
echo "=== SUMMARY (median ms, -j32 / pool=32) ==="
a=$(median < $OUT/bench/a.txt); b=$(median < $OUT/bench/b.txt); c=$(median < $OUT/bench/c.txt)
echo "  baseline: $a"
echo "  PCH:      $b   ($(awk "BEGIN{printf \"%+.1f%%\", ($b-$a)*100/$a}") vs baseline)"
echo "  vane:     $c   ($(awk "BEGIN{printf \"%+.1f%%\", ($c-$a)*100/$a}") vs baseline, $(awk "BEGIN{printf \"%+.1f%%\", ($c-$b)*100/$b}") vs PCH)"
