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
make build/adxl345_compare build/adxl345_irq build/adxl345_modes

set +e
sudo -n "$ROOT/build/adxl345_compare" "$ROOT"
compare_status=$?
sudo -n "$ROOT/build/adxl345_irq"
irq_status=$?
sudo -n "$ROOT/build/adxl345_modes" "$ROOT"
modes_status=$?
set -e
echo "compare_exit=${compare_status} irq_exit=${irq_status} modes_exit=${modes_status}"
if [[ "$compare_status" -ne 0 || "$irq_status" -ne 0 || "$modes_status" -ne 0 ]]; then
  exit 1
fi
