#!/usr/bin/env bash
# Copy this tree to the Pi, build it there, and run the on-target checks.
# Password is not stored here. The netkit helper reads NETKIT_PI_PASS.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SSH="${NETKIT_PI_SSH:-${HOME}/workspace/netkit/tools/pi_ssh.sh}"
: "${NETKIT_PI_PASS:?set NETKIT_PI_PASS}"
export NETKIT_PI_HOST="${NETKIT_PI_HOST:-pi.local}"
export NETKIT_PI_USER="${NETKIT_PI_USER:-pi}"
REMOTE="${NETKIT_PI_DIR:-/home/pi/adxl345}"

if [[ ! -x "$SSH" ]]; then
  echo "missing $SSH" >&2
  exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
COPYFILE_DISABLE=1 tar -C "$ROOT" -czf "$stage/adxl345.tgz" \
  --exclude build --exclude cmake-build-debug --exclude .git .

"$SSH" ssh "sudo raspi-config nonint do_i2c 0; mkdir -p $REMOTE"
"$SSH" scp "$stage/adxl345.tgz" "$REMOTE/adxl345.tgz"
"$SSH" ssh "tar -xzf $REMOTE/adxl345.tgz -C $REMOTE && rm -f $REMOTE/adxl345.tgz && sudo -n rmmod adxl345_custom || true && make -C $REMOTE clean all test && $REMOTE/build/adxl345 scan && $REMOTE/build/adxl345_hw && bash $REMOTE/tools/run_kernel_compare.sh"
