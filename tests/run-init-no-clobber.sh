#!/usr/bin/env bash
# tests/run-init-no-clobber.sh -- `tur init` must never destroy a working tree.
#
# tur-init-help-scaffolds-and-overwrites: `tur init --help` did not print help.
# The flag starts with '-', so it fell past the project-name slot, and with no
# name given the CURRENT DIRECTORY's basename was used -- so `tur init --help`
# in a checkout called `turmeric` scaffolded a project called `turmeric` over
# the working tree, replacing a 209-line `.gitignore` with an 8-line template
# and clobbering `README.md`.  Data loss, from a command spelled `--help`.
#
# Two independent defects composed into it, and both are asserted here:
#   1. `--help` (and an unknown flag) was not handled.
#   2. Scaffolding overwrote existing files.  The refusal that DID exist keyed
#      on a MANIFEST being present, so a checkout with no build.tur at its root
#      sailed straight past it.
#
# Why nothing caught it: `tests/run-sweet-manifest.sh` and `tests/run-tur-new.sh`
# both scaffold inside `mktemp -d`, so nothing in the suite ever ran `tur init`
# in a directory whose contents it cared about, and nothing checked that
# `--help` prints help.
#
# Usage: bash tests/run-init-no-clobber.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."
TUR="$(cd "$(dirname "${TUR:-./build/tur}")" && pwd)/$(basename "${TUR:-./build/tur}")"
if [ ! -x "$TUR" ]; then
    echo "run-init-no-clobber: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

# A directory that looks like a real project checkout: files a scaffold would
# target, and NO build.tur -- the shape the manifest-keyed refusal missed.
seed() {
    rm -rf "$TMP/proj" && mkdir -p "$TMP/proj/.github/workflows"
    printf 'REAL GITIGNORE\n'  > "$TMP/proj/.gitignore"
    printf '# My Real README\n' > "$TMP/proj/README.md"
    printf 'REAL JUSTFILE\n'    > "$TMP/proj/Justfile"
    printf 'REAL LOCK\n'        > "$TMP/proj/tur.lock"
    printf 'real ci\n'          > "$TMP/proj/.github/workflows/ci.yml"
}

# Every seeded file must still hold its original first line.
intact() {
    local why="$1" ok=0
    [ "$(head -1 "$TMP/proj/.gitignore")" = "REAL GITIGNORE" ]   || ok=1
    [ "$(head -1 "$TMP/proj/README.md")" = "# My Real README" ]  || ok=1
    [ "$(head -1 "$TMP/proj/Justfile")" = "REAL JUSTFILE" ]      || ok=1
    [ "$(head -1 "$TMP/proj/tur.lock")" = "REAL LOCK" ]          || ok=1
    [ "$(head -1 "$TMP/proj/.github/workflows/ci.yml")" = "real ci" ] || ok=1
    if [ $ok -ne 0 ]; then
        echo "FAIL init-no-clobber: $why -- a seeded file was overwritten"
        FAILED=1
    else
        echo "PASS init-no-clobber: $why"
    fi
}

# 1. `--help` prints usage and writes nothing.  This is the reported command.
seed
out="$(cd "$TMP/proj" && "$TUR" init --help 2>&1)"
case "$out" in
    *"usage: tur init"*) : ;;
    *) echo "FAIL init-no-clobber: --help did not print usage"; FAILED=1 ;;
esac
intact "--help leaves the tree alone"

# 2. An UNKNOWN flag is refused, not silently treated as "scaffold here".
#    A flag that does not exist is the same accident with a different
#    spelling.  (This case was first written with `--saffron`, back when that
#    flag did not exist; saffron-lang-plan S8 made `tur init --saffron` real,
#    so the unknown flag is now one nothing will ever claim.)
seed
out="$(cd "$TMP/proj" && "$TUR" init --no-such-flag 2>&1)"
case "$out" in
    *"unknown option"*) : ;;
    *) echo "FAIL init-no-clobber: unknown flag was not refused"; FAILED=1 ;;
esac
intact "an unknown flag leaves the tree alone"

# 3. A GENUINE init refuses rather than clobbering, and names every target.
#    Each of these five goes through a different writer -- scaffold_write for
#    .gitignore/README.md, pkg_lock_write for tur.lock, justrun_write_template
#    for the Justfile -- so this asserts the refusal covers all of them, not
#    just the two the report happened to name.
seed
out="$(cd "$TMP/proj" && "$TUR" init myproj 2>&1)"
for f in .gitignore README.md Justfile tur.lock ci.yml; do
    case "$out" in
        *"$f"*) : ;;
        *) echo "FAIL init-no-clobber: refusal did not name $f"; FAILED=1 ;;
    esac
done
intact "a real init refuses instead of overwriting"

# 4. --force is the documented escape hatch and still overwrites.
seed
(cd "$TMP/proj" && "$TUR" init myproj --force --no-git >/dev/null 2>&1)
if [ "$(head -1 "$TMP/proj/.gitignore")" = "REAL GITIGNORE" ]; then
    echo "FAIL init-no-clobber: --force did not overwrite"
    FAILED=1
else
    echo "PASS init-no-clobber: --force still overwrites"
fi

# 5. The ordinary path is unharmed: a clean directory scaffolds.
rm -rf "$TMP/clean" && mkdir -p "$TMP/clean"
(cd "$TMP/clean" && "$TUR" init myproj --no-git >/dev/null 2>&1)
if [ -f "$TMP/clean/build.tur" ] && [ -f "$TMP/clean/src/main.tur" ]; then
    echo "PASS init-no-clobber: a clean directory still scaffolds"
else
    echo "FAIL init-no-clobber: clean scaffold did not produce build.tur + src/main.tur"
    FAILED=1
fi

if [ $FAILED -ne 0 ]; then
    echo "run-init-no-clobber: FAILED"
    exit 1
fi
echo "run-init-no-clobber: PASS"
