# The pipe-based reactor fixtures do not BUILD on Windows (plan says runtime)

**Severity: low (Windows-only, 9 fixtures, arguably works-as-intended).** The
value of this report is mostly the correction to the plan doc: these fixtures
are documented as a runtime limitation, but they never get that far.

Found 2026-07-31 sweeping `TUR=./build-win/tur.exe bash tests/run.sh` on main
at `f630230e5` (MSYS2/UCRT64, gcc 16.1.0).

## Affected

```
reactor-fd-modify        reactor-fibers-cancel-on-free   reactor-wake-cross-thread
reactor-fd-readable      reactor-fibers-park-fd          scheduler-io-park
reactor-fd-remove        reactor-stop-from-callback
reactor-fd-writable
```

## Cause

Their inline-C calls `pipe()`, which MinGW does not declare -- it ships `_pipe`,
which takes a different signature (extra buffer-size and mode arguments):

```
tests_fixtures_reactor-fd-readable_input_tur.c:7676:7: error: implicit
    declaration of function 'pipe'; did you mean '_pipe'?
 7676 |   if (pipe(fds) < 0) { free(fds); return NULL; }
```

`-Wimplicit-function-declaration` is a hard error on gcc >= 14, and the
`-Wno-error=` downgrade that used to cover it was deliberately removed
(`src/main.c:5243-5251`).

## Correction to the plan

`docs/upcoming/v1/windows-remaining-plan.md`, under "WIN3 tail -- Pipe-fd
polling: a hard `select()` limit (~8 fixtures)", lists this exact fixture set
and says they fail because "Windows `select()` is socket-only -- it cannot poll
pipe or file fds at all, so the select-based backend (`src/async/io_iocp.c`)
cannot service them."

That is a true statement about the runtime, but it is not what is happening
today: these fixtures fail at `cc`, before any reactor code runs. The plan
should say "do not build" rather than "fail at runtime". The two causes compound
rather than compete -- making them compile would just move the failure to the
`select()` limit the plan describes.

## Fix directions

The plan's recommendation (option 1: accept as a platform limit and skip) still
looks right, and now for two reasons rather than one. Concretely: add a
`requires.*` marker so `tests/run.sh` PASS-skips them on Windows, consistent
with the existing mechanism documented in `CLAUDE.md` (`requires.tsan`,
`requires.spices`, `requires.dedicated-runner`). That means a new marker kind
(e.g. `requires.posix-pipe`) plus the matching check in `tests/run.sh`.

Do **not** "fix" this by switching the inline-C to `_pipe`. That trades a clear
build error for fds the select-based backend cannot poll, i.e. a confusing hang
instead of an honest failure.
