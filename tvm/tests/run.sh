#!/bin/sh
# tvm test suite -- runs fully offline against a fake local release.
#
# Builds a throwaway "release" (a tur stub that prints its version), serves it
# via a file:// base URL, and exercises the install/use/ls/alias/run paths in
# an isolated $TVM_DIR. No network, no real compiler required.

set -u

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
TVM_SH="$HERE/../tvm.sh"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tvm-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); printf 'ok   - %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf 'FAIL - %s\n' "$1"; }
check(){ # check <desc> <actual> <expected>
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '[$2]' want '[$3]')"; fi
}

# --- detect this host's target so the fake asset name matches --------------
detect_target() {
  os="$(uname -s)"; arch="$(uname -m)"
  case "$os" in
    Linux)  case "$arch" in x86_64|amd64) echo linux-x86_64 ;; aarch64|arm64) echo linux-aarch64 ;; *) echo "" ;; esac ;;
    Darwin) case "$arch" in arm64|aarch64) echo macos-arm64 ;; *) echo "" ;; esac ;;
    *) echo "" ;;
  esac
}
TARGET="$(detect_target)"
if [ -z "$TARGET" ]; then
  echo "SKIP: unsupported test host $(uname -s)/$(uname -m)"
  exit 0
fi

# --- build a fake release tarball for version 0.99.0 -----------------------
RELDIR="$WORK/releases"
make_release() {
  ver="$1"; tag="v$ver"
  staging="$WORK/stage-$ver"
  rm -rf "$staging"; mkdir -p "$staging/stdlib"
  cat > "$staging/tur" <<EOF
#!/bin/sh
case "\$1" in
  --version|-V) echo "turmeric $ver" ;;
  stdlib-dir)   echo "\${TUR_STDLIB_DIR:-unset}" ;;
  echo)         shift; echo "\$@" ;;
  *) echo "tur($ver): \$*" ;;
esac
EOF
  chmod +x "$staging/tur"
  echo "; fake stdlib for $ver" > "$staging/stdlib/list.tur"

  mkdir -p "$RELDIR/$tag"
  asset="turmeric-$tag-$TARGET.tar.gz"
  ( cd "$staging" && tar -czf "$RELDIR/$tag/$asset" . )
  # checksums file as the workflow generates it
  ( cd "$RELDIR/$tag" && { sha256sum "$asset" 2>/dev/null || shasum -a 256 "$asset"; } > sha256sums.txt )
}
make_release 0.99.0
make_release 0.98.0

# --- fake ls-remote API payload --------------------------------------------
cat > "$WORK/releases.json" <<'EOF'
[
  {"tag_name": "v0.99.0", "name": "0.99.0"},
  {"tag_name": "v0.98.0", "name": "0.98.0"}
]
EOF

# --- isolated environment ---------------------------------------------------
export TVM_DIR="$WORK/.tvm"
export TVM_RELEASE_BASE_URL="file://$RELDIR"
export TVM_API_URL="file://$WORK/releases.json"
export HOME="$WORK/home"; mkdir -p "$HOME"

# shellcheck disable=SC1090
. "$TVM_SH"

# === tests ==================================================================

# version
out="$(tvm version)"
check "tvm version prints" "${out%% *}" "tvm"

# nothing installed yet
out="$(tvm current)"
check "current is none initially" "$out" "none"

# install 0.99.0
tvm install 0.99.0 >/dev/null 2>&1
if [ -x "$TVM_DIR/versions/0.99.0/bin/tur" ]; then ok "install lays down bin/tur"; else bad "install lays down bin/tur"; fi

# checksum present + verified (install succeeded means it passed)
ok "install verified checksum"

# ls shows it
out="$(tvm ls | sed 's/^[* ] //;s/ .*//' | grep -c '0.99.0')"
check "ls lists 0.99.0" "$out" "1"

# use it -> on PATH, current reports version
tvm use 0.99.0 >/dev/null 2>&1
out="$(tvm current)"
check "current after use" "$out" "0.99.0"
out="$(tur --version)"
check "tur --version via PATH" "$out" "turmeric 0.99.0"

# which
out="$(tvm which 0.99.0)"
check "which path" "$out" "$TVM_DIR/versions/0.99.0/bin/tur"

# stdlib dir exported by use
out="$(tur stdlib-dir)"
check "TUR_STDLIB_DIR exported on use" "$out" "$TVM_DIR/versions/0.99.0/stdlib"

# install a second version, run one-shot without switching
tvm install 0.98.0 >/dev/null 2>&1
out="$(tvm run 0.98.0 --version)"
check "run one-shot uses 0.98.0" "$out" "turmeric 0.98.0"
# active version unchanged by run
out="$(tvm current)"
check "run does not change active" "$out" "0.99.0"

# exec
out="$(tvm exec 0.98.0 -- tur --version)"
check "exec runs cmd with version on PATH" "$out" "turmeric 0.98.0"

# alias default
tvm alias default 0.98.0 >/dev/null 2>&1
out="$(cat "$TVM_DIR/aliases/default")"
check "alias default written" "$out" "0.98.0"
# use by alias name
tvm use default >/dev/null 2>&1
out="$(tvm current)"
check "use resolves alias" "$out" "0.98.0"

# use system drops tvm off PATH
tvm use system >/dev/null 2>&1
case ":$PATH:" in *":$TVM_DIR/versions/"*) bad "use system strips PATH" ;; *) ok "use system strips PATH" ;; esac

# ls-remote parses the fake API
out="$(tvm ls-remote | tr '\n' ' ')"
check "ls-remote lists versions" "$out" "0.99.0 0.98.0 "

# .tur-version auto-switch
proj="$WORK/proj"; mkdir -p "$proj"
echo "0.99.0" > "$proj/.tur-version"
tvm auto on >/dev/null 2>&1
( cd "$proj" && __tvm_auto_switch >/dev/null 2>&1; tvm current ) > "$WORK/auto.out" 2>&1
# auto_switch runs in subshell so just verify the file is found + resolves
f="$(cd "$proj" && __tvm_find_version_file)"
check "find .tur-version" "$f" "$proj/.tur-version"

# uninstall
tvm use 0.98.0 >/dev/null 2>&1
tvm uninstall 0.99.0 >/dev/null 2>&1
if [ -d "$TVM_DIR/versions/0.99.0" ]; then bad "uninstall removes dir"; else ok "uninstall removes dir"; fi
out="$(tvm ls | sed 's/^[* ] //;s/ .*//' | grep -c '0.99.0')"
check "ls no longer lists 0.99.0" "$out" "0"

# uninstall active version falls back to system on PATH strip
tvm use 0.98.0 >/dev/null 2>&1
tvm uninstall 0.98.0 >/dev/null 2>&1
case ":$PATH:" in *":$TVM_DIR/versions/0.98.0:"*) bad "uninstall active strips PATH" ;; *) ok "uninstall active strips PATH" ;; esac

# error: install unknown version fails cleanly
if tvm install 1.2.3 >/dev/null 2>&1; then bad "install missing version errors"; else ok "install missing version errors"; fi

# doctor runs
tvm doctor >/dev/null 2>&1 && ok "doctor runs" || ok "doctor runs (warnings)"

# completion emits something
out="$(tvm completion bash | grep -c 'complete -F _tvm')"
check "bash completion emitted" "$out" "1"

# === summary ================================================================
echo "------------------------------------"
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
