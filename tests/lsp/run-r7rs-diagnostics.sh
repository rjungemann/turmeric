#!/usr/bin/env bash
# tests/lsp/run-r7rs-diagnostics.sh -- does the LSP analyse a `#lang r7rs`
# buffer as R7RS, and format it?
#
# Wrapper around r7rs-diagnostics.py, in the shape run-stdio-smoke.sh uses:
# skip cleanly (exit 0) when python3 or the binary is missing, so a machine
# without them does not fail the suite.
set -u
cd "$(dirname "$0")/../.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available -- LSP r7rs diagnostics test skipped"
    echo "TUR_SKIP: python3 unavailable"
    exit 0
fi

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built" >&2
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi

exec python3 tests/lsp/r7rs-diagnostics.py "$TUR"
