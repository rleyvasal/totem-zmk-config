#!/usr/bin/env bash
# Regenerate totem-keymap.svg from config/totem.keymap using keymap-drawer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v keymap >/dev/null 2>&1; then
  echo "keymap-drawer is not installed. Install with:"
  echo "  pipx install keymap-drawer"
  echo "or: pip install keymap-drawer"
  exit 1
fi

ARGS=()
if [[ -f keymap-drawer-config.yaml ]]; then
  ARGS+=(-c keymap-drawer-config.yaml)
fi

keymap -c keymap-drawer-config.yaml parse -z config/totem.keymap > /tmp/totem-keymap.yaml
# Prefer the shield physical layout when this keymap-drawer version supports it.
if keymap draw --help 2>&1 | grep -q dts; then
  keymap "${ARGS[@]}" draw -d boards/shields/totem/totem.dtsi /tmp/totem-keymap.yaml > totem-keymap.svg
else
  keymap "${ARGS[@]}" draw /tmp/totem-keymap.yaml > totem-keymap.svg
fi

echo "Wrote $ROOT/totem-keymap.svg"
