# The MCP server exits mid-session on Windows, between two tool calls

**Severity: medium.** Intermittent CI failure on `Windows build + suite 1/3
(MSYS2/UCRT64)`. The `tur` MCP server answers one `tools/call` and then closes
its stdout before answering the next, ending the session. Not reproduced on
Linux or macOS.

**Status: open.** Observed 2026-09-15 on PR #875, which does not touch the LSP,
the MCP server, or the stdio transport.

## What actually happens

The harness reported it as:

```
File "tests/lsp/mcp_lsp_test.py", line 317, in test_mcp
    def_text = r["result"]["content"][0]["text"]
TypeError: 'NoneType' object is not subscriptable
```

which names the wrong thing, and is worth unpicking because it is what made
this look like "a tool returned null".

`srv.call` -> `await_id` -> `mcp_read_one`, and `mcp_read_one` returns `None`
in exactly one case:

```python
line = stream.readline()
if not line:
    return None          # EOF
```

So `r` is `None` only when the server's **stdout reached EOF**. The server did
not answer unhelpfully; it **exited**. The preceding `hover` call, against the
same file and the same position, had just been answered normally -- so the
process died between answering `hover` and answering `definition`.

The harness half of this is fixed in the same change: `tool_text()` now reports
"server closed stdout (exited mid-session)" with the exit status and the
server's stderr, instead of raising a `TypeError` several frames from anything
meaningful. That does not fix the exit; it makes the next occurrence
diagnosable, which this one was not.

## Why it is not the PR it was seen on

Worth recording, because "unrelated-looking" is not evidence:

- the immediately preceding run (`35014899506`, head `91c31b74`) contains every
  code change of that PR and this same job **passed**;
- the only delta to the failing head `1674153f` is **one table row added to
  `docs/reported/README.md`**.

Identical compiler code, one docs line apart, passing and then failing. That
makes it intermittent and environmental rather than caused by a diff.

## Where to look

`mcp_tool_definition` (`src/lsp/mcp.c`) is itself defensive -- every early exit
frees and returns a message, and a missing symbol is a normal
`"No definition found for 'x'"` response, not an exit. So the exit is most
likely below it, in `mcp_load_and_analyze` (which runs the elaborator) or
`mcp_find_symbol`. Note `hover` calls `mcp_load_and_analyze` on the same file
and survived, so a first analysis of that file works; whatever fails is either
the second analysis in one process or something `definition` reaches that
`hover` does not.

Adjacent, and probably the same neighbourhood: the test's own recent history is
Windows transport work (`fix(win): put the LSP/DAP stdio transport in binary
mode`, `fix(win): make LSP cross-module resolution work`), and
[windows-subprocess-and-shared-lib-gaps](windows-subprocess-and-shared-lib-gaps.md)
is open against Windows subprocess behaviour.

## Fix directions

1. **Get the evidence first.** With the harness fix above, the next occurrence
   reports the exit status and stderr. A non-zero status or an abort message
   distinguishes a crash from a clean-but-premature exit, and those have
   different causes.
2. If it is a crash, run the MCP server under a debugger or with
   `-fsanitize=address` on MSYS2/UCRT64 and drive the same two calls
   (`hover` then `definition`, same path and position).
3. If it is a clean exit, suspect the stdio read loop terminating early -- the
   binary-mode fix above is precedent for that class of bug on this platform.

## Repro

Intermittent; no reliable local repro. Seen once in
[run 35015960062](https://github.com/rjungemann/turmeric/actions/runs/35015960062),
job `Windows build + suite 1/3 (MSYS2/UCRT64)`, on `1674153f`. The same job
passed on `91c31b74` minutes earlier and passed again on re-run.
