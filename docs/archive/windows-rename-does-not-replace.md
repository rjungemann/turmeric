# `rename()` does not replace an existing file on Windows

**Severity: high, Windows only. FIXED 2026-09-06.** Every "write a temp file,
rename it over the real one" atomic update in the tree could CREATE a file and
never UPDATE one. Six call sites; the visible symptom was
`tur: rename failed: File exists`.

Found chasing the last `tests/run-install.sh` failures on Windows, where six of
eleven remaining failures turned out to share this one cause.

## What it is

POSIX `rename(2)` replaces an existing destination. The Windows CRT's
`rename()` does not -- it fails with `EEXIST`. So the atomic-update idiom

```c
FILE *f = fopen(tmp_path, "w");
...
if (rename(tmp_path, path) != 0) { ... }
```

works exactly once per file, and every subsequent write fails.

## What it broke

| site | consequence |
| --- | --- |
| `global.c` `tur_state_write` | a second `tur install` could not record itself -- the installed-spice registry was write-once |
| `pkg.c` `pkg_lock_write` | a second `tur fetch` could not update `tur.lock` |
| `pkg.c` (build path, +1 more) | cached build outputs never refreshed |
| `main.c` (two index writers) | index files never refreshed |
| `platform_fs.h` `tur_settle_exe_output` | unaffected -- it already `remove()`s the destination first |

The state-registry one is what made it visible: `tur install` worked, and the
*second* `tur install` failed. On a first run everything looks fine.

## The fix

`platform_fs.h` aliases `rename` to `tur_rename_replace`, which is
`MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)` -- the atomic replace, so the
pattern keeps the property it was written for rather than degrading to
remove-then-rename with a window where the file does not exist. `GetLastError`
is mapped onto `errno` so the callers' existing `strerror` diagnostics stay
truthful.

Aliasing over `rename()` rather than editing six call sites is the idiom this
header already uses for `mkdir`, `lstat` and `readlink`, and it covers any
future site without the author having to know. Nothing can be relying on the
failure it removes: POSIX `rename()` never fails because the target exists.

`<windows.h>` comes in with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`, both guarded
with `#ifndef` -- without them it drops `min`, `max` and `ERROR` macros into
every translation unit that includes this header, which is most of the compiler.

## What to watch for

The shape to grep for is not "calls rename" but **"writes a temp file and
renames it over a path that may already exist"**. On Windows that is a silent
no-op-with-an-error rather than an update, and a caller that only checks the
first write will not notice.
