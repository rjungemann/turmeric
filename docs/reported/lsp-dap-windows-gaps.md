# LSP/DAP on Windows: what still fails after the stdio fix

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

## 1. Cross-module resolution returns nothing (LSP)

`tests/lsp/run-mcp-lsp.sh` on Windows: **65 passed, 5 failed.** All five are the
"LSP inside a spice (A6)" group:

```
lsp spice: definition crosses to the sibling module (got None)
lsp spice: completion offers an imported name (got set())
lsp spice: rename edits exactly the one real use (got 0)
lsp spice: renaming from a use in another file refuses with a reason
lsp spice: a local is renameable in place
```

Reduced to a standalone probe: a spice with `build.tur` and
`src/{mathy,user}.tur`, `didOpen` on `user.tur`, then
`textDocument/definition` on the imported `double-it`. The reply is `null`.

Two things this is **not**:

- Not spice discovery generally. `tur check src/user.tur` on the same tree
  resolves the sibling module and exits 0.
- Not the path separator. The probe was run with the path spelled both
  `C:/dir/src/user.tur` and `C:\dir\src\user.tur`; both return `null`.

Not root-caused further. The next step is to find where the LSP's own module
index diverges from the one `tur check` builds, since only the former fails.

## 2. `spice_root_of` cannot walk up a backslash path (LSP) -- fixed, unverifiable

`src/lsp/lsp.c`'s walk-up looked for `strrchr(cur, '/')` and returned false when
there was none. A path spelled `C:\dir\src\user.tur` contains no forward slash,
so the function answered "not in a spice" for the entire platform -- silently,
since the caller reads that as "no spice here" rather than an error.

Fixed here (both separators), and worth stating plainly: **the fix changes no
observable behaviour today**, because (1) above breaks cross-module resolution
regardless. It is kept because the defect is not a hypothesis -- it is what the
code does with a backslash -- and because it will matter the moment (1) is
fixed. The five tests above still fail with it in place.

This is the third instance of the same bug class, after `find_stdlib_beside_exe`
and `rewrite_autolink_relative_paths` (both fixed in #824): a POSIX-only path
assumption that is invisible until a drive-lettered path arrives, and that fails
by returning "nothing found" rather than by erroring.

## 3. Time-travel replay (DAP)

`tests/run-dap.sh` on Windows gets a long way -- breakpoints bind, stepping
works, variables and `evaluate` return correct values -- and then:

```
LIVE replayInfo success=false message=there is no recording in this session -- relaunch with "replay": true
LIVE replaySeek success=false
FAIL: timeout waiting for event output
```

Whether the recorder itself is broken on Windows or the driver's relaunch step
is, was not determined. Everything before the replay section passes.

## Fix directions

1. Root-cause (1). It is the one that costs a Windows user real editor
   functionality: go-to-definition and rename across modules.
2. Then run `tur_mcp_lsp_tests` and `tur_dap` in the Windows CI job. They cannot
   go green until (1) and (3) are settled, which is why the harness fixes land
   now and the CI wiring does not.
3. A cheaper interim guard: a smoke check that `tur lsp` and `tur dap` answer an
   `initialize` at all. That is the regression that just happened, it is two
   subprocess calls, and it would have caught a total outage that 65 passing
   assertions never saw because none of them ran.
