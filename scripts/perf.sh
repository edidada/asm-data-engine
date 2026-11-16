#!/usr/bin/env bash
#
# perf.sh - V7: hardware-counter profile for asm-stat.
#
# The TSC path in asm-stat measures wall-clock cycles per element. This
# script adds whole-process counters from perf: IPC, cache misses, branch
# mispredictions and stalled cycles, which point at *why* a kernel is slow
# (bandwidth bound, branch bound, ...).
#
# Usage:
#   sudo ./scripts/perf.sh ./build/asm-stat big.bin --threads 16 --io mmap
#   PERF_RECORD=1 sudo ./scripts/perf.sh ./build/asm-stat big.bin --threads 16
#
# Environment:
#   PERF          perf binary (default: perf)
#   PERF_EVENTS   comma-separated event list
#   PERF_RECORD   set to 1 to run `perf record -g` instead of `perf stat`
#
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 ASM_STAT FILE [asm-stat options...]" >&2
    echo "example: sudo $0 ./build/asm-stat big.bin --threads 16 --io mmap" >&2
    exit 2
fi

bin=$1
file=$2
shift 2

perf_bin=${PERF:-perf}
events=${PERF_EVENTS:-cycles,instructions,branches,branch-misses,cache-references,cache-misses}
record=${PERF_RECORD:-0}

if ! command -v "$perf_bin" >/dev/null 2>&1; then
    echo "error: '$perf_bin' not found." >&2
    echo "  Ubuntu: sudo apt-get install -y linux-tools-common linux-tools-\$(uname -r)" >&2
    exit 1
fi

# Hardware counters are often restricted for unprivileged users.
if ! "$perf_bin" stat -e cycles -- true >/dev/null 2>&1; then
    echo "error: perf cannot read hardware counters." >&2
    echo "  try: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid" >&2
    echo "  or rerun this script with sudo." >&2
    exit 1
fi

if [ "$record" = "1" ]; then
    exec "$perf_bin" record -g -o perf.data -- \
        "$bin" --input "$file" "$@"
fi

exec "$perf_bin" stat -e "$events" -- "$bin" --input "$file" "$@"
