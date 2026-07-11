#!/usr/bin/env bash
# spike004: baseline (no PCH) vs PCH, on the pch-planner fixture.
# All .o via command-line clang++-19 so timing is directly comparable to the
# vane fork path (which also emits .o directly, not through the build system).
cd ~/pch-planner/test/fixtures/proj || exit 1

FLAGS="-std=c++20 -I."
mapfile -t TUS < <(ls tu_*.cpp | sort)
echo "TU count: ${#TUS[@]}"
mkdir -p /tmp/s004

run_serial() {  # $1 = extra flags (e.g. -include-pch)
  local extra="$1"
  local start end
  start=$(date +%s%N)
  for f in "${TUS[@]}"; do
    clang++-19 $FLAGS $extra -c "$f" -o "/tmp/s004/$(basename "$f").o" 2>/dev/null
  done
  end=$(date +%s%N)
  echo "$(( (end - start) / 1000000 ))"
}

run_parallel() {  # $1 = extra flags, $2 = jobs
  local extra="$1" jobs="$2" start end
  compile_one() { clang++-19 -std=c++20 -I. $EXTRA -c "$1" -o "/tmp/s004/$(basename "$1").o" 2>/dev/null; }
  export -f compile_one
  export EXTRA="$extra"
  start=$(date +%s%N)
  printf '%s\n' "${TUS[@]}" | xargs -P"$jobs" -I{} bash -c 'compile_one "$@"' _ {}
  end=$(date +%s%N)
  echo "$(( (end - start) / 1000000 ))"
}

echo "=== A. baseline, no PCH ==="
echo "baseline_serial_ms=$(run_serial '')"
echo "baseline_j32_ms=$(run_parallel '' 32)"

echo "=== B. PCH (common.h + heavy_a/b/c) ==="
# Build one PCH covering the union of the shared headers. A TU only including
# common.h still works with a PCH that also parsed heavy_*; extra decls are
# harmless. This mirrors a project-wide precompiled.h.
cat > /tmp/s004/prefix.h <<'EOF'
#include "common.h"
#include "heavy_a.h"
#include "heavy_b.h"
#include "heavy_c.h"
EOF
pch_start=$(date +%s%N)
clang++-19 $FLAGS -x c++-header /tmp/s004/prefix.h -o /tmp/s004/prefix.pch 2>/tmp/s004/pch.err
pch_end=$(date +%s%N)
if [ ! -f /tmp/s004/prefix.pch ]; then echo "PCH BUILD FAILED"; head /tmp/s004/pch.err; exit 1; fi
echo "pch_build_ms=$(( (pch_end - pch_start) / 1000000 ))"
PCH="-include-pch /tmp/s004/prefix.pch"
echo "pch_serial_ms=$(run_serial "$PCH")"
echo "pch_j32_ms=$(run_parallel "$PCH" 32)"
echo "  (pch_serial_total_incl_build_ms = pch_build + pch_serial)"
