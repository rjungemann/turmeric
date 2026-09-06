# `tur install` cannot place a binary on Windows

**Severity: high, Windows only.** `tur install` builds the binary and then fails
to install it. Four defects, in two files, all independent of the shell-quoting
ones already fixed.

Found 2026-09-06 immediately after the quoting fix let the build step run for
the first time. `tests/run-install.sh` on Windows is **12 passed, 22 failed**;
every one of the 22 is gated on the first of these.

## 1. `symlink()` is not implemented on MinGW

```
tur install: symlink('.../tur-home/bin/tur-sample.tmp.28188'
                  -> '.../sample-spice/build/tur-sample') failed: Function not implemented
```

`inst_replace_symlink` (install.c:265) writes a temp symlink and renames it over
the target. MinGW's CRT has no `symlink`, and the Win32 call it would wrap
(`CreateSymbolicLinkW`) needs either administrator rights or Developer Mode.

**Direction:** try a real symlink, fall back to a copy.
`CreateSymbolicLinkA(tmp, src, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)`
succeeds under Developer Mode -- which most developer machines have on -- and
gives semantics identical to POSIX. Otherwise `CopyFileA`. The rename-over-target
step is unchanged, so the placement stays atomic either way.

The cost of the copy path is that rebuilding the spice no longer updates the
installed binary. That is what `tur upgrade` is for, and it should say so when
it takes the copy path.

## 2. Ownership is decided by `readlink`

`inst_check_bin_target` (install.c:228) and `cmd_uninstall` (install.c:814) both
decide "did an earlier `tur install` put this here?" by `lstat` + `S_ISLNK` +
`readlink`, comparing the target against `spices_dir`. A copy answers none of
those.

**Direction:** the registry already knows. `cmd_uninstall` reaches that loop
only after `tur_state_find(name)` returned an entry -- its own comment calls the
`S_ISLNK` check "defensive" -- so on Windows it can accept a regular file at a
path the registry claims. For the install-time conflict check, the registry is
also the right source: "is this bin name claimed by another installed spice" is
a `state.tur` question, not a filesystem one.

Keep the POSIX path exactly as it is. The symlink check is strictly better where
symlinks exist.

## 3. No `.exe`

`tur build -o <path>` writes `<path>` with no extension, and that is what gets
installed as `bin/tur-sample`. Windows resolves a bare name through `PATHEXT`,
so a file with no extension is not findable as a command -- `tur-sample` on the
PATH does not run.

**Direction:** name the installed entry `tur-sample.exe` on Windows. It has to be
recorded that way in `state.tur` so uninstall builds the same path, and
`tests/run-install.sh`'s `bin/tur-sample exists` assertion has to accept the
platform suffix.

## 4. `try_external_subcommand` is POSIX-only in three ways

`main.c:10507`, the `tur foo` -> `tur-foo` fallthrough:

```c
const char *colon = strchr(p, ':');       /* PATH separator */
snprintf(candidate, ..., "%.*s/%s", ...);
if (access(candidate, X_OK) == 0) { ... execv(candidate, nv); }
```

- **The PATH separator is `;` on Windows, not `:`.** Worse than not finding
  anything: the walk splits `C:\Users\...` at the drive colon, so every
  candidate is malformed.
- **No `.exe` probe**, so it cannot find the installed binary even with the
  separator fixed.
- **`execv` on Windows does not replace the process** the way POSIX does -- the
  parent returns immediately and a calling shell sees the command finish before
  the child has. `_spawnv(_P_WAIT, ...)` and exiting with the child's status is
  the idiom that behaves.

## Not this

The shell-quoting defects in the same file (`rm -rf`, `git ls-remote`, the build
invocation, `cd` without `/d`) are **fixed** -- see
[windows-spice-fetch-shell-quoting](windows-spice-fetch-shell-quoting.md). Before
that, `tur install` failed at the build step with cmd.exe's `The filename,
directory name, or volume label syntax is incorrect`; it now builds and fails
here instead. The harness count is unchanged at 12/22 because defect 1 above
gates the same downstream tests -- what moved is which stage fails.
