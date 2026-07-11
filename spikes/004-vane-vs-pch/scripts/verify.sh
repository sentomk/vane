#!/usr/bin/env bash
# spike004 correctness: prove vane's .o are functionally equivalent to
# independent compiles, via (1) defined-symbol set diff and (2) end-to-end
# link + run producing identical program output.
cd ~/pch-planner/test/fixtures/proj || exit 1
OUT=/tmp/s004
VANE=$OUT/vane_out
mkdir -p $OUT/indep

FLAGS="-std=c++20 -I. -O0"
mapfile -t TUS < <(ls tu_*.cpp | sort)

# 1. independent compiles
for f in "${TUS[@]}"; do
  clang++-19 $FLAGS -c "$f" -o "$OUT/indep/$(basename "$f").o" 2>/dev/null
done

# 2. symbol-set diff, one representative per group
echo "=== defined-symbol diff (vane vs independent) ==="
mismatch=0
for f in tu_a_10.cpp tu_b_1.cpp tu_c_1.cpp; do
  [ -f "$VANE/$f.o" ] || continue
  llvm-nm-19 -C "$VANE/$f.o"      | grep -E ' [TWDBVRtwdbvr] ' | awk '{print $2,$3,$4,$5,$6,$7,$8,$9}' | sort > "$OUT/v.txt"
  llvm-nm-19 -C "$OUT/indep/$f.o" | grep -E ' [TWDBVRtwdbvr] ' | awk '{print $2,$3,$4,$5,$6,$7,$8,$9}' | sort > "$OUT/i.txt"
  if diff -q "$OUT/v.txt" "$OUT/i.txt" >/dev/null; then
    echo "  $f: symbol sets IDENTICAL ($(wc -l < "$OUT/v.txt") defined)"
  else
    echo "  $f: SYMBOL MISMATCH"
    diff "$OUT/v.txt" "$OUT/i.txt" | head -10
    mismatch=1
  fi
done

# 3. end-to-end: link main.cpp + all TU .o from each source, run, compare output
echo "=== end-to-end link + run ==="
clang++-19 $FLAGS -c main.cpp -o $OUT/main.o 2>/dev/null

VANE_OBJS=(); INDEP_OBJS=()
for f in "${TUS[@]}"; do
  VANE_OBJS+=("$VANE/$(basename "$f").o")
  INDEP_OBJS+=("$OUT/indep/$(basename "$f").o")
done

if clang++-19 $OUT/main.o "${VANE_OBJS[@]}"  -o $OUT/prog_vane  2>$OUT/link_vane.err; then
  echo "  vane link: OK"
else
  echo "  vane link: FAILED"; head -5 $OUT/link_vane.err
fi
if clang++-19 $OUT/main.o "${INDEP_OBJS[@]}" -o $OUT/prog_indep 2>$OUT/link_indep.err; then
  echo "  indep link: OK"
else
  echo "  indep link: FAILED"; head -5 $OUT/link_indep.err
fi

if [ -x $OUT/prog_vane ] && [ -x $OUT/prog_indep ]; then
  $OUT/prog_vane  > $OUT/out_vane.txt  2>&1; rc_v=$?
  $OUT/prog_indep > $OUT/out_indep.txt 2>&1; rc_i=$?
  echo "  vane run rc=$rc_v, indep run rc=$rc_i"
  if diff -q $OUT/out_vane.txt $OUT/out_indep.txt >/dev/null; then
    echo "  PROGRAM OUTPUT IDENTICAL"
    echo "  output: $(cat $OUT/out_vane.txt)"
  else
    echo "  OUTPUT DIFFERS:"
    echo "   vane:  $(cat $OUT/out_vane.txt)"
    echo "   indep: $(cat $OUT/out_indep.txt)"
    mismatch=1
  fi
fi

echo "=== verdict: $([ $mismatch -eq 0 ] && echo CORRECT || echo MISMATCH) ==="
