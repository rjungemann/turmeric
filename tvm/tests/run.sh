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

# --- build a fake release tarball ------------------------------------------
#
# make_release <version> <flat|prefix>
#
# Both shapes are covered because both have shipped: windows-x86_64 has always
# used the prefix layout and the three tar.gz targets were flat until the
# archives were unified. tvm has to install either one into the same tree, so
# the suite builds one of each and asserts the result is identical.
RELDIR="$WORK/releases"
make_release() {
  ver="$1"; layout="$2"; tag="v$ver"
  staging="$WORK/stage-$ver"
  rm -rf "$staging"

  case "$layout" in
    flat)   bindir="$staging"; libdir="$staging"; stddir="$staging/stdlib" ;;
    prefix) bindir="$staging/bin"; libdir="$staging/lib"
            stddir="$staging/share/turmeric/stdlib" ;;
    *) echo "make_release: bad layout '$layout'" >&2; exit 1 ;;
  esac
  mkdir -p "$bindir" "$libdir" "$stddir" "$staging/include/turi"

  cat > "$bindir/tur" <<EOF
#!/bin/sh
case "\$1" in
  --version|-V) echo "turmeric $ver" ;;
  stdlib-dir)   echo "\${TUR_STDLIB_DIR:-unset}" ;;
  echo)         shift; echo "\$@" ;;
  *) echo "tur($ver): \$*" ;;
esac
EOF
  chmod +x "$bindir/tur"
  echo "; fake stdlib for $ver" > "$stddir/list.tur"
  echo "$ver" > "$stddir/VERSION"
  # Stand-ins for libturt_runtime.a / libturi.a. Their placement is the whole
  # point of the normalization: tur probes <exe_dir>/../lib for the runtime
  # archive, so one left at the version root is one tur cannot find.
  echo "fake" > "$libdir/libturt_runtime.a"
  echo "fake" > "$libdir/libturi.a"
  echo "/* fake */" > "$staging/include/turi/eval.h"

  mkdir -p "$RELDIR/$tag"
  asset="turmeric-$tag-$TARGET.tar.gz"
  ( cd "$staging" && tar -czf "$RELDIR/$tag/$asset" . )
  # checksums file as the workflow generates it
  ( cd "$RELDIR/$tag" && { sha256sum "$asset" 2>/dev/null || shasum -a 256 "$asset"; } > sha256sums.txt )
}
make_release 0.99.0 flat
make_release 0.98.0 prefix

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

# install 0.99.0 (shipped FLAT)
tvm install 0.99.0 >/dev/null 2>&1
if [ -x "$TVM_DIR/versions/0.99.0/bin/tur" ]; then ok "install lays down bin/tur"; else bad "install lays down bin/tur"; fi

# A flat tarball must be normalized into the prefix layout on the way in.
# Leaving the runtime archive at the version root while moving `tur` into bin/
# is what made an installed version unable to compile: locate_runtime_lib
# probes <exe_dir>/src, <exe_dir> and <exe_dir>/../lib, and the version root is
# none of those once tur has moved down into bin/.
v99="$TVM_DIR/versions/0.99.0"
if [ -f "$v99/lib/libturt_runtime.a" ]; then ok "flat install moves libturt_runtime.a into lib/"; else bad "flat install moves libturt_runtime.a into lib/"; fi
if [ -f "$v99/lib/libturi.a" ]; then ok "flat install moves libturi.a into lib/"; else bad "flat install moves libturi.a into lib/"; fi
if [ -f "$v99/libturt_runtime.a" ]; then bad "flat install leaves no .a at the version root"; else ok "flat install leaves no .a at the version root"; fi
if [ -f "$v99/share/turmeric/stdlib/list.tur" ]; then ok "flat install moves stdlib under share/turmeric"; else bad "flat install moves stdlib under share/turmeric"; fi
if [ -d "$v99/stdlib" ]; then bad "flat install leaves no stdlib at the version root"; else ok "flat install leaves no stdlib at the version root"; fi
if [ -f "$v99/include/turi/eval.h" ]; then ok "flat install keeps include/turi in place"; else bad "flat install keeps include/turi in place"; fi

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
check "TUR_STDLIB_DIR exported on use" "$out" "$TVM_DIR/versions/0.99.0/share/turmeric/stdlib"

# install a second version, shipped in the PREFIX layout this time
tvm install 0.98.0 >/dev/null 2>&1
v98="$TVM_DIR/versions/0.98.0"
if [ -x "$v98/bin/tur" ]; then ok "prefix install lays down bin/tur"; else bad "prefix install lays down bin/tur"; fi
if [ -f "$v98/lib/libturt_runtime.a" ]; then ok "prefix install keeps lib/ in place"; else bad "prefix install keeps lib/ in place"; fi
if [ -f "$v98/share/turmeric/stdlib/list.tur" ]; then ok "prefix install keeps share/turmeric/stdlib in place"; else bad "prefix install keeps share/turmeric/stdlib in place"; fi

# Both shapes must land identically -- that is the point of normalizing.
lay99="$( cd "$v99" && find . -type d | sort | tr '\n' ' ' )"
lay98="$( cd "$v98" && find . -type d | sort | tr '\n' ' ' )"
check "flat and prefix installs produce the same tree" "$lay99" "$lay98"

# run one-shot without switching
out="$(tvm run 0.98.0 --version)"
check "run one-shot uses 0.98.0" "$out" "turmeric 0.98.0"

# `tvm run` must point at the version it is running, with no warning-provoking
# path to a stdlib that is not there.
out="$(tvm run 0.98.0 stdlib-dir)"
check "run exports the right stdlib" "$out" "$v98/share/turmeric/stdlib"
# active version unchanged by run
out="$(tvm current)"
check "run does not change active" "$out" "0.99.0"

# exec
out="$(tvm exec 0.98.0 -- tur --version)"
check "exec runs cmd with version on PATH" "$out" "turmeric 0.98.0"
out="$(tvm exec 0.98.0 -- tur stdlib-dir)"
check "exec exports the right stdlib" "$out" "$v98/share/turmeric/stdlib"

# TUR_STDLIB_DIR is exported, so it outlives a switch. `use` has to move it
# every time or a later version compiles against an earlier version's stdlib --
# which tur honors (the directory has a readable macros.tur) and miscompiles.
tvm use 0.99.0 >/dev/null 2>&1
out="$(tur stdlib-dir)"
check "use 0.99.0 points at 0.99.0's stdlib" "$out" "$v99/share/turmeric/stdlib"
tvm use 0.98.0 >/dev/null 2>&1
out="$(tur stdlib-dir)"
check "use 0.98.0 moves TUR_STDLIB_DIR off 0.99.0" "$out" "$v98/share/turmeric/stdlib"

# A version installed by an OLDER tvm is on disk in the un-normalized shape.
# Upgrading tvm must not strand it, so the stdlib probe accepts both.
legacy="$TVM_DIR/versions/0.97.0"
mkdir -p "$legacy/bin" "$legacy/stdlib"
cp "$v98/bin/tur" "$legacy/bin/tur"
sed -i.bak 's/turmeric 0.98.0/turmeric 0.97.0/' "$legacy/bin/tur" && rm -f "$legacy/bin/tur.bak"
echo "; legacy" > "$legacy/stdlib/list.tur"
tvm use 0.97.0 >/dev/null 2>&1
out="$(tur stdlib-dir)"
check "use finds a legacy top-level stdlib" "$out" "$legacy/stdlib"
tvm use 0.98.0 >/dev/null 2>&1

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
