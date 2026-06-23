#!/bin/sh
# install.sh -- bootstrap the Turmeric Version Manager (tvm).
#
# Installs tvm into $TVM_DIR (default ~/.tvm) and wires it into your shell
# rc. After installation, open a new shell (or `source` your rc) and run
# `tvm help`.
#
# Usage:
#   sh install.sh                # install from this checkout
#   TVM_DIR=~/.turmeric-vm sh install.sh
#
# Modeled after the nvm / rustup installer scripts.

set -eu

TVM_DIR="${TVM_DIR:-$HOME/.tvm}"

# Locate the directory holding this script (so we can copy tvm.sh + libexec).
SRC_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

echo "tvm: installing into $TVM_DIR"
mkdir -p "$TVM_DIR" "$TVM_DIR/versions" "$TVM_DIR/aliases" "$TVM_DIR/cache/downloads"

cp "$SRC_DIR/tvm.sh" "$TVM_DIR/tvm.sh"
if [ -d "$SRC_DIR/libexec" ]; then
  cp -R "$SRC_DIR/libexec" "$TVM_DIR/"
fi

# Decide which rc file to wire up.
detect_rc() {
  if [ -n "${ZSH_VERSION-}" ] || [ "${SHELL##*/}" = "zsh" ]; then
    echo "$HOME/.zshrc"
  elif [ -n "${BASH_VERSION-}" ] || [ "${SHELL##*/}" = "bash" ]; then
    if [ -f "$HOME/.bashrc" ]; then echo "$HOME/.bashrc"; else echo "$HOME/.bash_profile"; fi
  else
    echo "$HOME/.profile"
  fi
}

RC="$(detect_rc)"
SNIPPET_MARKER="# >>> tvm initialize >>>"

if [ -f "$RC" ] && grep -qF "$SNIPPET_MARKER" "$RC"; then
  echo "tvm: $RC already configured; leaving it untouched"
else
  {
    echo ""
    echo "$SNIPPET_MARKER"
    echo "export TVM_DIR=\"$TVM_DIR\""
    echo "[ -s \"\$TVM_DIR/tvm.sh\" ] && . \"\$TVM_DIR/tvm.sh\""
    echo "# <<< tvm initialize <<<"
  } >> "$RC"
  echo "tvm: added init snippet to $RC"
fi

cat <<EOF

tvm installed.

  Open a new shell, or run:
      export TVM_DIR="$TVM_DIR"
      . "$TVM_DIR/tvm.sh"

  Then:
      tvm install 0.23.1
      tvm use 0.23.1
      tur --version

EOF
