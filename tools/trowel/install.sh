#!/usr/bin/env bash
# install.sh -- drop the Turmeric plugin + color themes into a user's
# existing Trowel configuration.
#
# Usage:
#   bash tools/trowel/install.sh [--copy] [--config-dir <path>]
#
# Defaults:
#   --config-dir = ${TROWEL_CONFIG:-$HOME/.config/trowel}
#   symlinks (so future updates of this repo propagate); pass --copy to
#   install standalone copies instead.

set -euo pipefail

CFG_DIR="${TROWEL_CONFIG:-$HOME/.config/trowel}"
MODE="symlink"

while [ $# -gt 0 ]; do
  case "$1" in
    --copy)         MODE="copy"; shift ;;
    --config-dir)   CFG_DIR="$2"; shift 2 ;;
    --help|-h)
      sed -n '2,15p' "$0"
      exit 0
      ;;
    *) echo "install.sh: unknown arg: $1" >&2; exit 1 ;;
  esac
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/tools/trowel"

mkdir -p "$CFG_DIR/plugins" "$CFG_DIR/colors"

place() {
  local from="$1" to="$2"
  if [ "$MODE" = "symlink" ]; then
    ln -sf "$from" "$to"
  else
    cp -f "$from" "$to"
  fi
  echo "installed: $to"
}

place "$SRC/turmeric.lua"               "$CFG_DIR/plugins/turmeric.lua"
place "$SRC/colors/turmeric-dark.lua"   "$CFG_DIR/colors/turmeric-dark.lua"
place "$SRC/colors/turmeric-light.lua"  "$CFG_DIR/colors/turmeric-light.lua"

if [ -f "$SRC/dist/plugins/welcome.lua" ]; then
  place "$SRC/dist/plugins/welcome.lua" "$CFG_DIR/plugins/welcome.lua"
fi

cat <<EOF

Done. Open any .tur or .tur.sweet file in Trowel and press cmd+r (macOS)
or ctrl+r to run it through the Turmeric interpreter.

If \`tur\` is not on the PATH that Trowel inherits (common on macOS when
launched from Finder), add this to $CFG_DIR/init.lua:

  local config = require "core.config"
  config.plugins.turmeric = config.plugins.turmeric or {}
  config.plugins.turmeric.tur = "/abs/path/to/tur"

EOF
