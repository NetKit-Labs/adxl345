#!/usr/bin/env bash
# Build the out-of-tree modules and compare them on this Pi.
# Run on the Pi, from anywhere. Requires passwordless sudo.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

make -C kernel clean
make -C kernel/stock clean
make -C kernel -j4
make -C kernel/stock -j4
make build/adxl345_compare

exec sudo -n "$ROOT/build/adxl345_compare" "$ROOT"
