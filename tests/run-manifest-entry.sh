#!/usr/bin/env bash
# tests/run-manifest-entry.sh -- `:entry "<path>"` in build.tur.
#
# Covers the first rung of `tur run`'s project-mode entry-point ladder:
#   1. :entry key in build.tur
#   2. src/main.tur
#   3. the single .tur file under src/
#
# Each project below carries TWO .tur files under src/, so rungs 2 and 3 alone
# cannot pick one -- a project that runs at all is proof :entry decided it.
# See docs/archive/build-tur-entry-key-unimplemented.md.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "manifest-entry: $TUR not built" >&2; exit 2; }
TUR_ABS=$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")

# The compiled arm shells out to cc against the sanitized libturi; anchor -L
# exactly as run.sh does.
_tur_build_dir=$(dirname "$TUR_ABS")
export TUR_CC_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -fno-strict-aliasing -L${_tur_build_dir}/src}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/tur-manifest-entry.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

mk_project() { # $1=dir  $2=entry-line (may be empty)
    mkdir -p "$1/src"
    {
        echo "(defpackage p"
        echo "  :name    \"p\""
        echo "  :version \"0.1.0\""
        [ -n "$2" ] && echo "  $2"
        echo ")"
    } > "$1/build.tur"
    cat > "$1/src/foo.tur" <<'TUR_EOF'
(defn main [] : int
  (println "ran foo")
  0)
TUR_EOF
    cat > "$1/src/bar.tur" <<'TUR_EOF'
(defn main [] : int
  (println "ran bar")
  0)
TUR_EOF
}

PASS=0; FAIL=0
chk() { # $1=name $2=expected-substring $3=actual
    case "$3" in
        *"$2"*) PASS=$((PASS+1)); echo "PASS $1" ;;
        *) FAIL=$((FAIL+1)); echo "FAIL $1 -- wanted '$2' in:"; printf '%s\n' "$3" | sed 's/^/    /' ;;
    esac
}

# 1. :entry picks its file out of an otherwise ambiguous src/.
mk_project "$tmp/e1" ':entry   "src/foo.tur"'
out=$(cd "$tmp/e1" && "$TUR_ABS" run 2>&1)
chk "entry-selects-foo" "ran foo" "$out"

# 2. ... and it is the KEY doing the selecting, not alphabetical luck.
mk_project "$tmp/e2" ':entry   "src/bar.tur"'
out=$(cd "$tmp/e2" && "$TUR_ABS" run 2>&1)
chk "entry-selects-bar" "ran bar" "$out"

# 3. :entry beats an existing src/main.tur (rung 1 outranks rung 2).
mk_project "$tmp/e3" ':entry   "src/foo.tur"'
cat > "$tmp/e3/src/main.tur" <<'TUR_EOF'
(defn main [] : int
  (println "ran main")
  0)
TUR_EOF
out=$(cd "$tmp/e3" && "$TUR_ABS" run 2>&1)
chk "entry-beats-main-tur" "ran foo" "$out"

# 4. An absolute :entry is honored as written rather than re-rooted.
mk_project "$tmp/e4" ":entry   \"$tmp/e4/src/bar.tur\""
out=$(cd "$tmp/e4" && "$TUR_ABS" run 2>&1)
chk "entry-absolute-path" "ran bar" "$out"

# 5. A dangling :entry is a hard error naming the key -- NOT a silent fall
#    through to src/main.tur, which would run a different program than the
#    manifest asked for.
mk_project "$tmp/e5" ':entry   "src/nope.tur"'
cat > "$tmp/e5/src/main.tur" <<'TUR_EOF'
(defn main [] : int
  (println "ran main")
  0)
TUR_EOF
out=$(cd "$tmp/e5" && "$TUR_ABS" run 2>&1; echo "rc=$?")
chk "dangling-entry-errors"      'does not name a file' "$out"
chk "dangling-entry-nonzero"     "rc=1"                 "$out"
case "$out" in
    *"ran main"*) FAIL=$((FAIL+1)); echo "FAIL dangling-entry-no-fallthrough -- ran src/main.tur anyway" ;;
    *) PASS=$((PASS+1)); echo "PASS dangling-entry-no-fallthrough" ;;
esac

# 6. No :entry + an ambiguous src/ still reports the old error, and now
#    mentions the key as a way out.
mk_project "$tmp/e6" ""
out=$(cd "$tmp/e6" && "$TUR_ABS" run 2>&1; echo "rc=$?")
chk "no-entry-ambiguous-errors"  "cannot determine entry point" "$out"
chk "no-entry-suggests-key"      ":entry"                       "$out"
chk "no-entry-nonzero"           "rc=1"                         "$out"

# 7. No :entry + src/main.tur present -> rung 2, unchanged.
mk_project "$tmp/e7" ""
cat > "$tmp/e7/src/main.tur" <<'TUR_EOF'
(defn main [] : int
  (println "ran main")
  0)
TUR_EOF
out=$(cd "$tmp/e7" && "$TUR_ABS" run 2>&1)
chk "no-entry-falls-back-to-main" "ran main" "$out"

echo "manifest-entry summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
