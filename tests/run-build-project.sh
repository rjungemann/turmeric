#!/usr/bin/env bash
# tests/run-build-project.sh -- manifest-driven `tur build <dir>` smoke test.
#
# Verifies that `tur build <project-dir>` (where the directory carries a
# build.tur manifest) descends into src/ -- including nested src/<pkg>/ --
# compiles every module, resolves cross-module imports, and links a runnable
# binary.  Also asserts the bug it fixes: the manifest itself must not be
# compiled as source.
#
# The fixture is copied into a scratch dir before building so the generated
# .h/.c, _main.c, and .tur-abi-cache/ never land in the repo.

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/build-project-smoke"
WORK="$(mktemp -d -t tur-bp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; we are
# exercising the build-driver path, not chasing compiler-internal leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'just build' first" >&2
    exit 2
fi

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
BIN="$WORK/app"

# Build the project from a scratch CWD so generated files land in $WORK.
build_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    fail "build-project-compiles" "tur build exit=$build_rc: $build_out"
else
    pass "build-project-compiles"
fi

# The manifest must never be compiled as source.
if echo "$build_out" | grep -q "unbound symbol 'tur-build-project-smoke'"; then
    fail "build-project-skips-manifest" "build.tur was compiled as source"
else
    pass "build-project-skips-manifest"
fi

# The linked binary must exist and run, returning double-it(21) = 42.
if [ -x "$BIN" ]; then
    pass "build-project-binary-exists"
    "$BIN"
    run_rc=$?
    if [ "$run_rc" -eq 42 ]; then
        pass "build-project-cross-module-import"
    else
        fail "build-project-cross-module-import" "exit=$run_rc (expected 42)"
    fi
else
    fail "build-project-binary-exists" "expected $BIN to exist"
fi

# A module declared in :exports with no backing source file must fail loudly
# (Phase 2: parsed :exports map keys are validated against on-disk sources).
GHOST="$WORK/ghost"
mkdir -p "$GHOST/src/app"
cat > "$GHOST/build.tur" <<'EOF'
(defpackage tur-ghost
  :name    "tur-ghost"
  :version "0.1.0"
  :exports #{ "app/util" ["double-it"] "app/missing" ["nope"] })
EOF
cat > "$GHOST/src/app/util.tur" <<'EOF'
(defmodule app/util (export double-it) (defn double-it [x :int] :int (* x 2)))
EOF
ghost_out=$(cd "$WORK" && "$TUR" build "$GHOST" -o "$WORK/ghostbin" 2>&1)
ghost_rc=$?
if [ $ghost_rc -ne 0 ] && echo "$ghost_out" | grep -q "declares export 'app/missing'"; then
    pass "build-project-missing-export-fails"
else
    fail "build-project-missing-export-fails" "rc=$ghost_rc out=$ghost_out"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
