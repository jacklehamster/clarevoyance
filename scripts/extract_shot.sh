#!/usr/bin/env bash
# extract_shot.sh <logfile> <out.png>
# Extracts the base64 PNG from between CV_SHOT_BEGIN / CV_SHOT_END markers.
set -euo pipefail

LOG="$1"
OUT="$2"

if ! grep -q "CV_SHOT_BEGIN" "$LOG"; then
    echo "extract_shot: CV_SHOT_BEGIN not found in $LOG" >&2
    exit 1
fi

awk '/CV_SHOT_BEGIN/{found=1; next} /CV_SHOT_END/{if(found){exit}} found{print}' "$LOG" \
    | base64 -d > "$OUT"

echo "Extracted screenshot to $OUT ($(wc -c < "$OUT") bytes)"
