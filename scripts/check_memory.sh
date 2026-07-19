#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 5 ]]; then
  echo "usage: $0 EXECUTABLE GRAPH_DIR OUTPUT [THREADS] [ITERATIONS]" >&2
  exit 2
fi

executable=$1
graph_dir=$2
output=$3
threads=${4:-$(nproc)}
iterations=${5:-2}

systemd-run --user --wait --collect --pipe \
  --property=MemoryMax=128M \
  --property=MemorySwapMax=0 \
  --working-directory="$PWD" \
  "$executable" pagerank \
    --graph-dir "$graph_dir" \
    --output "$output" \
    --memory-mb 128 \
    --threads "$threads" \
    --iterations "$iterations"

