#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
# `int` is a unique prefix of `interpret` (no other canonical subcommand
# starts with "int"), so the dispatcher should resolve and run it.
"$TUR" int "$FIXTURE_DIR/tiny.tur"
