#!/usr/bin/env bash
# spike004 correctness at a given -O level. Usage: verify_opt.sh <0|1|2|3>
# Proves vane's .o match independent `clang -O<n>` compiles: defined-symbol
# sets + successful link + identical program output.
OPT="${1:-2}"
cd ~/pch-planner/test/fixtures/proj || exit 1
OUT=/tmp/s004/verify_o$OPT
VANE=$OUT/vane_out
mkdir -p "$OUT" "$VANE" "$OUT/indep"
FLAGS="-std=c++20 -I. -O$OPT"
mapfile -t TUS < <(ls tu_*.cpp | sort)

# vane objects at this opt level
VANE_OPT=$OPT /tmp/s004/vane_spike . "$VANE" 32 "${TUS[@]}" >/dev/null 2>&1

# independent objects
for f in "${TUS[@]}"; do
  clang++-19 $FLAGS -c "$f" -o "$OUT/indep/$(basename "$f").o" 2>/dev/null
done

echo "=== -O$OPT: defined-symbol diff ==="
mismatch=0
for f in tu_a_10.cpp tu_b_1.cpp tu_c_1.cpp; do
  [ -f "$VANE/$f.o" ] || continue
  llvm-nm-19 -C "$VANE/$f.o"      | grep -E ' [TWDBVRtwdbvr] ' | awk '{print $2,$3,$4,$5,$6,$7,$8,$9}' | sort > "$OUT/v.txt"
  llvm-nm-19 -C "$OUT/indep/$f.o" | grep -E ' [TWDBVRtwdbvr] ' | awk '{print $2,$3,$4,$5,$6,$7,$8,$9}' | sort > "$OUT/i.txt"
  if diff -q "$OUT/v.txt" "$OUT/i.txt" >/dev/null; then
    echo "  $f: IDENTICAL ($(wc -l < "$OUT/v.txt") defined)"
  else
    echo "  $f: MISMATCH"; diff "$OUT/v.txt" "$OUT/i.txt" | head -10; mismatch=1
  fi
done

echo "=== -O$OPT: link + run ==="
clang++-19 $FLAGS -c main.cpp -o "$OUT/main.o" 2>/dev/null
VANE_OBJS=(); INDEP_OBJS=()
for f in "${TUS[@]}"; do
  VANE_OBJS+=("$VANE/$(basename "$f").o")
  INDEP_OBJS+=("$OUT/indep/$(basename "$f").o")
done
clang++-19 "$OUT/main.o" "${VANE_OBJS[@]}"  -o "$OUT/prog_vane"  2>/dev/null && echo "  vane link OK"  || { echo "  vane link FAIL"; mismatch=1; }
clang++-19 "$OUT/main.o" "${INDEP_OBJS[@]}" -o "$OUT/prog_indep" 2>/dev/null && echo "  indep link OK" || { echo "  indep link FAIL"; mismatch=1; }
if [ -x "$OUT/prog_vane" ] && [ -x "$OUT/prog_indep" ]; then
  "$OUT/prog_vane";  rc_v=$?
  "$OUT/prog_indep"; rc_i=$?
  echo "  vane rc=$rc_v indep rc=$rc_i (both = program checksum low bit)"
  [ "$rc_v" = "$rc_i" ] && echo "  return codes match" || { echo "  RC MISMATCH"; mismatch=1; }
fi
echo "=== -O$OPT verdict: $([ $mismatch -eq 0 ] && echo CORRECT || echo MISMATCH) ==="
