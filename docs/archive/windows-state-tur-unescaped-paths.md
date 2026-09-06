# `state.tur` was written with unescaped Windows paths

**Severity: high, Windows only. FIXED 2026-09-06.** The installed-spice registry
was written as Turmeric source with raw backslashes in it, so the reader that
loads it back choked on the first directory separator and the whole registry
failed to parse. `tur install` could write it and then never read it.

Found chasing `tests/run-install.sh` on Windows, where every test that touched a
second `tur` invocation failed.

## What it looks like

```
state.tur:5:27: error: unknown string escape '\U'
5 |     "sample" #{:path "C:\Users\roger\AppData\Local\Temp\...\sample-spice" ... }
  |                           ^
state.tur:5:39: error: unknown string escape '\A'
state.tur:5:47: error: unknown string escape '\L'
state.tur:5:53: error: unknown string escape '\T'
state.tur:5:83: error: unknown string escape '\s'
```

Five errors from one path. `tur_state_read` then returns an empty registry, and
every command that asks "what is installed?" -- `tur list`, `tur uninstall`,
`tur upgrade`, the conflict check, `:global` dep resolution -- answers "nothing".

## Root cause

`tur_state_write` interpolated each value straight into a string literal:

```c
if (e->path) fprintf(f, ":path \"%s\" ", e->path);
```

`state.tur` is Turmeric source, read back by the Turmeric reader with escape
processing ON. A POSIX path survives because it has no backslashes. A Windows
path does not.

This is the **same defect** `src/source_literal.h` was written for -- generated
source with a path pasted into a string literal -- at a site the earlier sweep
did not reach. That header's own comment already says the hazard is not
Windows-specific: a directory name containing `"` breaks it on POSIX too.

## The fix

`tur_state_write` routes every emitted string through
`tur_source_literal_escape` via two small helpers (`state_put_str`,
`state_put_kv`), so the name, all eight scalar fields and the `:bin` list are
escaped. No reader change: the escaped form is what the reader already expects.

## Why it hid

The first `tur install` on a machine works perfectly -- it writes the file and
exits. Nothing reads it until the *next* command. So the bug is invisible in any
test that installs once and checks the filesystem, which is exactly what the
first four assertions in the harness do.

Fixing it moved `run-install.sh` on Windows from 16 passed / 18 failed to
23 / 11 in one change.

## Related

- [windows-rename-does-not-replace](windows-rename-does-not-replace.md) -- the
  *other* reason the registry could not be updated, found immediately after this
  one and gating six more of the same tests.
- `docs/reported/windows-text-mode-read-rejects-own-files.md` -- the same
  "tur cannot read a file tur wrote" shape, on the other side (the read).
