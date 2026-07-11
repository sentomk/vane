#!/usr/bin/env bash
# spike004 benchmark at a given -O level. Usage: bench_opt.sh <opt> <runs>
# baseline vs PCH vs vane, all at -O<opt>, median of <runs>.
OPT="${1:-2}"
RUNS="${2:-5}"
cd ~/pch-planner/test/fixtures/proj || exit 1
OUT=/tmp/s004/bench_o$OPT
mkdir -p "$OUT"
FLAGS="-std=c++20 -I. -O$OPT"
mapfile -t TUS < <(ls tu_*.cpp | sort)
echo "opt=-O$OPT TUs=${#TUS[@]} runs=$RUNS cores=$(nproc)"

median() { sort -n | awk '{a[NR]=$1} END{ if(NR%2) print a[(NR+1)/2]; else printf "%.0f\n",(a[NR/2]+a[NR/2+1])/2 }'; }

compile_one() { clang++-19 -std=c++20 -I. -O$VOPT $EXTRA -c "$1" -o "$OUT/$(basename "$1").o" 2>/dev/null; }
export -f compile_one; export OUT VOPT=$OPT
par() { export EXTRA="$1"; local s e; s=$(date +%s%N); printf '%s\n' "${TUS[@]}" | xargs -P"$2" -I{} bash -c 'compile_one "$@"' _ {}; e=$(date +%s%N); echo $(( (e-s)/1000000 )); }

: > "$OUT/a.txt"; for i in $(seq $RUNS); do par "" 32 >> "$OUT/a.txt"; done
cat > "$OUT/prefix.h" <<'EOF'
#include "common.h"
#include "heavy_a.h"
#include "heavy_b.h"
#include "heavy_c.h"
EOF
: > "$OUT/b.txt"
for i in $(seq $RUNS); do
  ps=$(date +%s%N); clang++-19 $FLAGS -x c++-header "$OUT/prefix.h" -o "$OUT/prefix.pch" 2>/dev/null; pe=$(date +%s%N)
  comp=$(par "-include-pch $OUT/prefix.pch" 32)
  echo $(( (pe-ps)/1000000 + comp )) >> "$OUT/b.txt"
done
: > "$OUT/c.txt"
for i in $(seq $RUNS); do
  VANE_OPT=$OPT /tmp/s004/vane_spike . "$OUT" 32 "${TUS[@]}" 2>/dev/null | grep '^total_ms=' | cut -d= -f2 | cut -d. -f1 >> "$OUT/c.txt"
done

a=$(median < "$OUT/a.txt"); b=$(median < "$OUT/b.txt"); c=$(median < "$OUT/c.txt")
echo "=== SUMMARY -O$OPT (median ms, -j32/pool=32) ==="
echo "  baseline: $a   [$(tr '\n' ' ' < "$OUT/a.txt")]"
echo "  PCH:      $b   [$(tr '\n' ' ' < "$OUT/b.txt")]  ($(awk "BEGIN{printf \"%+.1f%%\",($b-$a)*100/$a}") vs base)"
echo "  vane:     $c   [$(tr '\n' ' ' < "$OUT/c.txt")]  ($(awk "BEGIN{printf \"%+.1f%%\",($c-$a)*100/$a}") vs base, $(awk "BEGIN{printf \"%+.1f%%\",($c-$b)*100/$b}") vs PCH)"
