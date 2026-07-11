#!/usr/bin/env bash
cd ~/pch-planner/test/fixtures/proj || exit 1
mkdir -p /tmp/s004/vane_out
mapfile -t TUS < <(ls tu_*.cpp | sort)
echo "running vane spike on ${#TUS[@]} TUs, pool=$1"
/tmp/s004/vane_spike . /tmp/s004/vane_out "${1:-32}" "${TUS[@]}"
