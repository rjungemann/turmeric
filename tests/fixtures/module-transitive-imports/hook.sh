#!/usr/bin/env bash
# A multi-file project: src/main.tur imports lib/cli, which itself imports
# lib/a and lib/b.  The point is that the TRANSITIVE imports resolve from one
# -I pointing at src/.
#
# Driven by a hook because the tree has no top-level input.tur, and a directory
# whose only entry is a source dir is neither a fixture nor a group to the
# top-level scan -- it used to fall through to a silent PASS and was run by no
# harness at all.  See
# docs/archive/fixture-dirs-with-loose-tur-files-pass-without-running.md.
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
"$TUR" run "$FIXTURE_DIR/src/main.tur" -I "$FIXTURE_DIR/src"
