#!/usr/bin/env bash
set -e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
"$TUR" --interpret "$FIXTURE_DIR/tiny.tur"
