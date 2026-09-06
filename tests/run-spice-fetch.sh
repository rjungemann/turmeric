#!/usr/bin/env bash
# tests/run-spice-fetch.sh -- `tur fetch` and the lockfile integrity check, end
# to end, against a local git repo.
#
# Everything this covers was broken on Windows and covered by nothing: the CI
# job there runs tests/run.sh directly and never ctest, so the whole package
# manager had no check on that platform at all.  In one sitting that hid four
# separate defects, every one of them silent:
#
#   - pkg_git_fetch interpolated POSIX '...' quoting into a string cmd.exe runs,
#     so git got `''./spices/demo''` and nothing could be fetched
#     (docs/reported/windows-spice-fetch-shell-quoting.md)
#   - pkg_lock_read sized with ftell and read in text mode, so it rejected every
#     tur.lock tur itself wrote
#     (docs/reported/windows-text-mode-read-rejects-own-files.md)
#   - the integrity hash shelled out to sha256sum, which MinGW does not ship
#     (docs/reported/pkg-hash-shells-out-to-sha256sum.md)
#   - rename() does not replace on Windows, so the lock was write-once
#     (docs/archive/windows-rename-does-not-replace.md)
#
# A `file://` remote keeps this hermetic: no network, no credentials, and the
# same code path a real URL dep takes.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }
# A self-skip is a green check that tested nothing, which is the same disease
# this harness was written to catch -- the first CI run of it skipped exactly
# here, because the MSYS2 environment ships no git, and reported success.  So
# where git is guaranteed (every CI leg), a missing one is fatal; elsewhere it
# still skips, since `tur fetch` genuinely cannot work without git.
if ! command -v git >/dev/null 2>&1; then
    if [ "${TUR_REQUIRE_GIT:-0}" = 1 ]; then
        echo "FAIL run-spice-fetch -- git not found, and TUR_REQUIRE_GIT=1" >&2
        exit 1
    fi
    echo "tests: git not found; skipping (set TUR_REQUIRE_GIT=1 to make this fatal)" >&2
    exit 0
fi

# An absolute path spelled the way the platform's own tools expect.  tur is a
# native binary on Windows and does not understand an MSYS /c/... path.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) native() { cygpath -m "$1"; } ;;
  *)                    native() { echo "$1"; } ;;
esac

PASS=0
FAIL=0
FAILED=()
note() { printf "  %s\n" "$*"; }
ok()   { PASS=$((PASS+1)); echo "PASS $1"; }
bad()  { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "FAIL $1"; [ -n "${2:-}" ] && note "$2"; }

WORK="$(mktemp -d -t tur-fetch-tests-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

case "$TUR" in
  /*|?:*) TUR_ABS="$(native "$TUR")" ;;
  *)      TUR_ABS="$(native "$(pwd)")/${TUR#./}" ;;
esac

# ------------------------------------------------------------------ #
# The spice being depended on, as a local git repo.
# ------------------------------------------------------------------ #
mkdir -p "$WORK/demo/src"
cat > "$WORK/demo/build.tur" <<'EOF'
(defpackage demo
  :name    "demo"
  :version "0.1.0"
  :exports ["demo"])
EOF
cat > "$WORK/demo/src/demo.tur" <<'EOF'
(defmodule demo
  (export answer)
  (defn answer [] : int 42))
EOF
git -C "$WORK/demo" init -q
# Pin line endings: the tree hash is over CONTENT, so a machine with
# core.autocrlf=true must not produce a different fixture.
git -C "$WORK/demo" config core.autocrlf false
git -C "$WORK/demo" add -A
git -C "$WORK/demo" -c user.email=t@t -c user.name=t commit -qm init

# ------------------------------------------------------------------ #
# The consumer.
# ------------------------------------------------------------------ #
mkdir -p "$WORK/app/src"
cat > "$WORK/app/build.tur" <<EOF
(defpackage app
  :name    "app"
  :version "0.1.0"
  :spices  #map{"demo" #map{:url "file://$(native "$WORK/demo")"}})
EOF
cat > "$WORK/app/src/main.tur" <<'EOF'
(defmodule main
  (import demo :refer [answer])
  (defn main [] : int
    (println (answer))
    0))
EOF

cd "$WORK/app" || exit 1

# 1. fetch clones the dep.
out="$("$TUR_ABS" fetch 2>&1)"
if [ -d spices/demo ] && [ -f spices/demo/build.tur ]; then
    ok "fetch clones a file:// spice"
else
    bad "fetch clones a file:// spice" "$out"
fi

# 2. the lock records a hash tagged with the algorithm that produced it, so a
#    lockfile an older tur wrote is never compared against a newer hash.
if grep -q ':sha256 "tree1:[0-9a-f]\{64\}"' tur.lock 2>/dev/null; then
    ok "tur.lock records a tagged tree hash"
else
    bad "tur.lock records a tagged tree hash" "$(cat tur.lock 2>&1)"
fi

verdict() {
    out="$("$TUR_ABS" run 2>&1)"
    if echo "$out" | grep -q "integrity check failed"; then echo tampered
    elif echo "$out" | grep -q "^42";                 then echo ran
    else echo "other: $out"; fi
}

# 3. a clean tree runs, and is not reported as tampered.
v="$(verdict)"
[ "$v" = ran ] && ok "clean tree runs" || bad "clean tree runs" "$v"

# 4. the hash covers CONTENT, not the git metadata beside it: running git in a
#    fetched spice used to change .git/index's mtimes and so its hash, and the
#    next `tur run` reported tampering on a tree nobody had touched.
git -C spices/demo status >/dev/null 2>&1
v="$(verdict)"
[ "$v" = ran ] && ok "a git command in the tree does not look like tampering" \
               || bad "a git command in the tree does not look like tampering" "$v"

# 5. a real edit IS tampering.
echo ';; tampered' >> spices/demo/src/demo.tur
v="$(verdict)"
[ "$v" = tampered ] && ok "an edited dependency is reported" \
                    || bad "an edited dependency is reported" "$v"

# 6. and restoring it clears.
git -C spices/demo checkout -- . 2>/dev/null
v="$(verdict)"
[ "$v" = ran ] && ok "restoring the dependency clears the report" \
               || bad "restoring the dependency clears the report" "$v"

# 7. a SECOND fetch must rewrite the lock.  On Windows rename() does not replace
#    an existing file, so this is where a write-once lockfile shows up.
out="$("$TUR_ABS" fetch --update 2>&1)"
if grep -q ':sha256 "tree1:[0-9a-f]\{64\}"' tur.lock 2>/dev/null \
   && ! echo "$out" | grep -qi "rename failed"; then
    ok "a second fetch rewrites the lock"
else
    bad "a second fetch rewrites the lock" "$out"
fi

echo
echo "spice-fetch summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
