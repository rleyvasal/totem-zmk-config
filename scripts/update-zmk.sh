#!/usr/bin/env sh
# Re-apply patches/totem-ble.patch onto a newer official ZMK commit and push it
# to the `totem` branch of your ZMK fork (which config/west.yml points at).
#
# One-time setup (see patches/README.md):
#   git clone https://github.com/rleyvasal/zmk.git ~/zmk
#   cd ~/zmk && git remote add upstream https://github.com/zmkfirmware/zmk.git
#
# Usage:
#   scripts/update-zmk.sh <new-upstream-sha>
#
# Override the fork checkout location with:  ZMK_DIR=/path/to/zmk scripts/update-zmk.sh <sha>
set -e

NEW_SHA="$1"
ZMK_DIR="${ZMK_DIR:-$HOME/zmk}"
PATCH="$(cd "$(dirname "$0")/.." && pwd)/patches/totem-ble.patch"

if [ -z "$NEW_SHA" ]; then
  echo "usage: $0 <new-upstream-sha>"
  exit 1
fi

git -C "$ZMK_DIR" fetch upstream
git -C "$ZMK_DIR" checkout -B totem "$NEW_SHA"
# Re-apply our change. If this fails, upstream moved the code: fix the rejected
# hunks by hand, then regenerate the patch with:
#   git -C "$ZMK_DIR" diff app/src/ble.c > patches/totem-ble.patch
git -C "$ZMK_DIR" apply "$PATCH"
git -C "$ZMK_DIR" commit -am "Totem BLE: throttle + idle-disconnect"
git -C "$ZMK_DIR" push -f origin totem

echo
echo "Pushed rleyvasal/zmk 'totem' = ${NEW_SHA} + totem-ble.patch"
echo "config/west.yml tracks the 'totem' branch, so no west.yml edit is needed."
echo "(To pin for reproducibility, set revision: $(git -C "$ZMK_DIR" rev-parse totem) in west.yml.)"
