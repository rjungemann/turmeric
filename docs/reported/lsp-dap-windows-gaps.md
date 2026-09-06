# LSP/DAP on Windows: what still fails after the stdio fix

> **Mostly resolved.** The stdio transport, the spice walk-up and the file-URI
> spelling are all fixed; `tests/lsp/run-mcp-lsp.sh` is **70 passed / 0 failed**
> on Windows. What remains open is section 3, DAP time-travel replay.

**Severity: high for any editor on Windows.** `tur lsp` and `tur dap` produced
*zero output* on Windows until the stdio transport was put in binary mode (fixed
alongside this report). With that fixed, most of both servers works. What
remains is recorded here.

Found 2026-09-06 while bringing [Trowel](https://github.com/rjungemann/trowel)
up on Windows. Trowel drives `tur lsp`, `tur dap`, `tur format` and `tur repl`,
which makes it a good exercise of these surfaces -- none of which the Windows CI
job touches.

## How this went unnoticed

The coverage exists and does not run. `tests/lsp/run-mcp-lsp.sh`
(`tur_mcp_lsp_tests`) and `tests/run-dap.sh` (`tur_dap`) drive exactly these
servers, but:

- the Windows CI job runs `tests/run.sh` only -- it never invokes `ctest`, so
  neither target has ever run on Windows;
- `run-mcp-lsp.sh` hardcoded `TUR="./build/tur"`, with no `$TUR` override and no
  `.exe`, so it would have reported "not built" and skipped even if invoked.

Both are fixed with this report. The CI half -- actually running these on
Windows -- is a separate decision, because two of the checks below are still red
there.

## 1. Cross-module resolution returns nothing (LSP) -- FIXED

`tests/lsp/run-mcp-lsp.sh` on Windows went **65 passed / 5 failed -> 70 passed
/ 0 failed.** Two independent defects, and a wrong diagnosis of my own on the
way.

### The correction first

An earlier revision of this report said:

> Not spice discovery generally. `tur check src/user.tur` on the same tree
> resolves the sibling module and exits 0.

**That was wrong**, and it pointed away from the actual cause. `tur check` does
exit 0 -- but not because discovery worked. The module search path it printed
for a deliberately-bad import gives it away:

```
  searched:
    .../src/no-such-module.tur    (importing file's directory)
    .../stdlib/no-such-module.tur    (stdlib)
```

Two entries. The spice's own `src/` is absent, so
`auto_append_spice_includes` contributed **nothing**. `tur check` resolved the
sibling anyway because `mathy.tur` sits in the importing file's own directory,
which is search path #1 -- adjacency, not discovery. The LSP cannot lean on
that: it analyses a scratch copy in the temp directory, whose neighbours are
other scratch files.

The lesson is the one this codebase keeps teaching: exit 0 is not evidence that
the mechanism under test ran. Asking for a module that does not exist, and
reading the search path it printed, took a minute and settled it.

### Defect A: the spice walk-up could not step (src/main.c)

`find_spice_root` canonicalises with `realpath()` and then walks up with
`strrchr(dir, '/')`. On Windows `realpath` is `_fullpath`, which returns
`C:\dir\sub` **even when the caller passed forward slashes** -- so the step
found no separator, the loop broke at depth 0, and no spice root was ever
found. Every `(import sibling)` inside a spice went unresolved, so the symbol
index had no cross-module names and definition, completion and rename all
answered "nothing here".

Two more loops in the same file had the identical bug and are fixed with the
same helper: `find_project_root`, and the `tur docs` root probe. That makes
four instances of this class in total, after `find_stdlib_beside_exe` and
`rewrite_autolink_relative_paths` (#824). They all fail by returning "not
found" rather than by erroring, which is why they survive so long.

### Defect B: the URIs were not file URIs (src/lsp/lsp_util.c, lsp_docs.c)

With A fixed, definition resolved and returned:

```
file://C%3A%5CUsers%5Croger%5C...%5Csrc/mathy.tur
```

`lsp_path_to_uri` percent-encoded the drive colon and every backslash. That
round-trips through this codebase's own decoder, so every internal comparison
agreed and the server looked self-consistent -- but no editor spells a path
that way, so a definition answer naming another file was a URI the client could
not match to any document it had. Go-to-definition would land nowhere.

The reverse direction was broken too: `lsp_uri_to_path` stripped exactly
`file://` and stopped, turning the standard `file:///C:/dir/x.tur` that every
client sends into `/C:/dir/x.tur`, which no Windows API will open.

Both now speak `file:///C:/dir/x.tur`. POSIX is untouched: there is no drive
letter, so the same code produces the same bytes it always did, and the
backslash-to-slash rewrite is `#ifdef _WIN32` because a backslash is a legal
character in a POSIX filename.

`tests/lsp/mcp_lsp_test.py` built its URIs as `"file://" + path`, which is
correct on POSIX and not a file URI at all on Windows. It now goes through a
`to_uri()` helper that spells them the way a client does.

## 2. `spice_root_of` cannot walk up a backslash path (LSP) -- fixed

`src/lsp/lsp.c`'s own walk-up had the same defect as A, in a second copy.
Fixed with the same shape.

An earlier revision noted this fix "changes no observable behaviour, because
(1) masks it". With (1) fixed that is no longer true -- it is on the live path
for rename refusals now.

## 3. Time-travel replay (DAP) -- still open

`tests/run-dap.sh` on Windows gets a long way -- breakpoints bind, stepping
works, variables and `evaluate` return correct values -- and then:

```
LIVE replayInfo success=false message=there is no recording in this session -- relaunch with "replay": true
LIVE replaySeek success=false
FAIL: timeout waiting for event output
```

Whether the recorder itself is broken on Windows or the driver's relaunch step
is, was not determined. Everything before the replay section passes.

## What remains

Only section 3. The LSP harness is green on Windows, so it is now wired into
the Windows CI job -- the coverage that existed all along and had never run
there. `tur_dap` is deliberately NOT wired up yet: everything in it passes
except the replay section, and a job that is red for one known reason teaches
people to ignore it.

The `tur lsp` / `tur dap` stdio smoke test runs there too. It is two
subprocess calls and would have caught the original total outage, which 65
passing assertions did not -- because none of them ran on the platform.

For section 3, the first question is whether the recorder is broken on
Windows or the driver's relaunch step is. Everything before the replay
section passes, so it is well isolated.
