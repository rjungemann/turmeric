#!/usr/bin/env bash
# tests/lsp/run-mcp-lsp.sh — drive `tur mcp` and `tur lsp` end-to-end.
#
# Invokes mcp_lsp_test.py if python3 is available; otherwise skips with
# a non-failing message (in the spirit of tests/run-install.sh).
set -u
cd "$(dirname "$0")/../.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available -- MCP/LSP end-to-end tests skipped"
    echo "TUR_SKIP: python3 unavailable"
    exit 0
fi

# Honour $TUR like tests/run-dap.sh does, and fall back to the .exe so this
# is runnable on Windows -- where `tur lsp` and `tur dap` were silently
# broken precisely because nothing drove them there.
TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built" >&2
    # On stdout, unlike the human line above: the marker has to land in the
    # test's captured output for the timing ingest to see it.
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi

# Pin stdlib lookup so the running tur picks up the in-tree stdlib --
# matches what tests/run.sh does indirectly via the build layout.
exec env TUR="$TUR" python3 tests/lsp/mcp_lsp_test.py
