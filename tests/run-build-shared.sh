#!/usr/bin/env bash
# tests/run-build-shared.sh -- RP0+RP1 smoke test for `tur build --shared`.
#
# Builds tests/fixtures/build-shared-smoke as a shared library, then
# compiles a hand-written C harness that dlopens it and calls the
# exported `smokelib__add42` symbol. Confirms the end-to-end pipe
# from `defmodule ... (export ...)` -> linker -> dlopen -> dlsym -> call.
#
# RP1 additions: assert that exports.manifest is produced with the right
# format and that --manifest <path> redirects the output.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="./build/tur"
FIXTURE="tests/fixtures/build-shared-smoke"
WORK="$(mktemp -d -t tur-rp0.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'just build' first" >&2
    exit 2
fi

# Pick the shared-library extension. macOS dlopen accepts .so too, but use
# the platform-conventional name so the artifact looks right under `file`.
case "$(uname -s)" in
    Darwin) LIB_EXT="dylib" ;;
    *)      LIB_EXT="so" ;;
esac
LIB="$WORK/libsmoke.$LIB_EXT"

# Build the shared library.
build_out=$("$TUR" build --shared "$FIXTURE" -o "$LIB" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "build-shared-smoke-link" "tur build --shared exit=$build_rc: $build_out"
    exit 1
fi
if [ ! -f "$LIB" ]; then
    fail "build-shared-smoke-link" "expected $LIB to exist"
    exit 1
fi
pass "build-shared-smoke-link"

# The .so must export smokelib__add42 (the mangled name for the (export
# add42) defn inside (defmodule smokelib ...)). nm is preferred; fall
# back to strings if nm is unavailable.
if command -v nm >/dev/null 2>&1; then
    if nm "$LIB" 2>/dev/null | grep -qE '(^| )_?smokelib__add42( |$)'; then
        pass "build-shared-smoke-symbol-present"
    else
        fail "build-shared-smoke-symbol-present" "smokelib__add42 not found in $LIB"
    fi
fi

# Compile and run the harness.
HARNESS="$WORK/harness"
CC_BIN="${CC:-cc}"
# -ldl: required on Linux for dlopen/dlsym; macOS rolls them into libSystem
# so the flag is harmless there.
case "$(uname -s)" in
    Darwin) DL_LIBS="" ;;
    *)      DL_LIBS="-ldl" ;;
esac
if ! $CC_BIN -O0 -std=c99 -Wall -o "$HARNESS" "$FIXTURE/harness.c" $DL_LIBS 2>"$WORK/cc.err"; then
    fail "build-shared-smoke-harness-compile" "cc failed: $(cat "$WORK/cc.err")"
    exit 1
fi
pass "build-shared-smoke-harness-compile"

if out=$("$HARNESS" "$LIB" 2>&1); then
    if echo "$out" | grep -q 'smokelib__add42(100) = 142'; then
        pass "build-shared-smoke-dlopen-call"
    else
        fail "build-shared-smoke-dlopen-call" "unexpected output: $out"
    fi
else
    fail "build-shared-smoke-dlopen-call" "harness exit=$?, output: $out"
fi

# RP1: exports.manifest is produced at the default location (<out>.manifest)
# and records the export with the expected mangled symbol and signature.
DEFAULT_MANIFEST="$LIB.manifest"
if [ -f "$DEFAULT_MANIFEST" ]; then
    pass "build-shared-smoke-manifest-default-exists"
else
    fail "build-shared-smoke-manifest-default-exists" "expected $DEFAULT_MANIFEST"
fi

EXPECTED='smokelib/add42 -> smokelib__add42 :: (:int) -> :int'
if grep -qxF "$EXPECTED" "$DEFAULT_MANIFEST" 2>/dev/null; then
    pass "build-shared-smoke-manifest-line"
else
    fail "build-shared-smoke-manifest-line" \
         "expected line not found. got: $(cat "$DEFAULT_MANIFEST" 2>/dev/null)"
fi

# --manifest <path> redirects manifest output and does NOT write the default.
LIB2="$WORK/libsmoke-alt.$LIB_EXT"
ALT_MANIFEST="$WORK/exports.manifest"
"$TUR" build --shared "$FIXTURE" -o "$LIB2" --manifest "$ALT_MANIFEST" \
       >"$WORK/build2.log" 2>&1
if [ ! -f "$ALT_MANIFEST" ]; then
    fail "build-shared-smoke-manifest-override" \
         "--manifest did not produce $ALT_MANIFEST: $(cat "$WORK/build2.log")"
elif [ -f "$LIB2.manifest" ]; then
    fail "build-shared-smoke-manifest-override" \
         "default $LIB2.manifest should not exist when --manifest is given"
elif ! grep -qxF "$EXPECTED" "$ALT_MANIFEST"; then
    fail "build-shared-smoke-manifest-override" \
         "expected line missing from $ALT_MANIFEST: $(cat "$ALT_MANIFEST")"
else
    pass "build-shared-smoke-manifest-override"
fi

# --manifest without --shared must be rejected.
if "$TUR" build "$FIXTURE" --manifest /tmp/should-not-exist.manifest \
        >"$WORK/build3.log" 2>&1; then
    fail "build-shared-smoke-manifest-requires-shared" \
         "expected non-zero exit; output: $(cat "$WORK/build3.log")"
elif ! grep -q -- '--manifest requires --shared' "$WORK/build3.log"; then
    fail "build-shared-smoke-manifest-requires-shared" \
         "missing expected diagnostic; got: $(cat "$WORK/build3.log")"
else
    pass "build-shared-smoke-manifest-requires-shared"
fi

echo
echo "build-shared: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
