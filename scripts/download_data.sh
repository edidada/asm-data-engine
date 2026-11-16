#!/usr/bin/env bash
# download_data.sh - Fetch a real NYC TLC yellow taxi trip Parquet file.
# Usage: ./scripts/download_data.sh [YYYY-MM]
set -euo pipefail

MONTH="${1:-2024-01}"
URL="https://d37ci6vzurychx.cloudfront.net/trip-data/yellow_tripdata_${MONTH}.parquet"
OUT="yellow_tripdata_${MONTH}.parquet"

echo "Downloading ${URL}"
if command -v wget >/dev/null 2>&1; then
    wget -O "${OUT}" "${URL}"
else
    curl -L -o "${OUT}" "${URL}"
fi

echo "Converting to raw doubles..."
python3 scripts/convert.py "${OUT%.parquet}.bin" --parquet "${OUT}"
echo "Done. Run: ./build/asm-stat --input ${OUT%.parquet}.bin"
