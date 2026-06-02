#!/usr/bin/env bash
# tests/lsp/run-mcp-lsp.sh — drive `tur mcp` and `tur lsp` end-to-end.
#
# Invokes mcp_lsp_test.py if python3 is available; otherwise skips with
# a non-failing message (in the spirit of tests/run-install.sh).
set -u
cd "$(dirname "$0")/../.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available -- MCP/LSP end-to-end tests skipped"
    exit 0
fi

TUR="./build/tur"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built" >&2
    exit 0
fi

# Pin stdlib lookup so the running tur picks up the in-tree stdlib --
# matches what tests/run.sh does indirectly via the build layout.
exec python3 tests/lsp/mcp_lsp_test.py
