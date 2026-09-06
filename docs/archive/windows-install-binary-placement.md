# `tur install` could not place a binary on Windows

**Severity: high, Windows only. FIXED 2026-09-06.** `tur install` built the
binary and then failed to install it. Four defects in two files, all independent
of the shell-quoting ones fixed alongside them.

`tests/run-install.sh` on Windows went from **12 passed / 22 failed** to
**34 / 0** across this change and the two general defects it uncovered
([rename](windows-rename-does-not-replace.md),
[state.tur escaping](windows-state-tur-unescaped-paths.md)).

## 1. `symlink()` is not implemented on MinGW

```
tur install: symlink('.../bin/tur-sample.tmp.28188'
                  -> '.../build/tur-sample') failed: Function not implemented
```

**Fixed:** `inst_place_bin` tries
`CreateSymbolicLinkA(tmp, src, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)`
and falls back to `CopyFileA`. Under Developer Mode -- which most developer
machines have on -- the semantics are identical to POSIX. Otherwise the copy
costs the live-update property (rebuilding the spice no longer changes the
installed command until `tur upgrade` runs), which the installer now says out
loud once per run rather than leaving the user to discover.

The fallback is what answers `platform_fs.h`'s standing warning against making
`symlink()` succeed via `CreateSymbolicLinkA` -- a shim that works only for
elevated users is worse than one that never works, and the fallback is the only
way to answer that. `platform_fs.h`'s shims are unchanged and unused by this
path; its comment now says so.

## 2. Ownership was decided by `readlink`

`inst_check_bin_target` and `cmd_uninstall` both decided "did an earlier
`tur install` put this here?" with `lstat` + `S_ISLNK` + `readlink`. A copy
answers none of those.

**Fixed:** the registry answers instead, which is what actually records it.
`inst_check_bin_target` takes the `TurState` and, when the entry is not a
symlink, asks which installed spice claims that bin name -- distinguishing "our
own previous install" (allow) from "another spice's" (refuse, naming it) from
"nobody's" (refuse as foreign). `cmd_uninstall` is already inside
`if (e)`, so the registry has vouched for the entry before the loop runs; its
own comment called the `S_ISLNK` test defensive, and it now also removes a plain
file at a path the registry claims.

The POSIX path is unchanged. A symlink check is strictly better where symlinks
exist.

## 3. No `.exe`

`tur build -o <path>` writes exactly `<path>`, so the installed entry had no
extension -- and Windows resolves a bare command name through `PATHEXT`, so it
was not findable as a command.

**Fixed:** `inst_bin_name` adds the suffix. The suffix is a property of the
PATH, not of the name: `state.tur` keeps the logical `tur-sample` on every
platform, so a registry written on Windows reads the same as one written on
Linux and `tur list --json` does not vary. Every filesystem path is built
through the helper.

## 4. `try_external_subcommand` was POSIX-only in three ways

The `tur foo` -> `tur-foo` fallthrough:

- **Split `PATH` on `:`.** Worse than finding nothing: it tears every entry at
  its drive colon, so `C:\Users\x` becomes `C` and `\Users\x` and every
  candidate is malformed. Now `TUR_PATH_LIST_SEP`.
- **No `.exe` probe.** Now probes `.exe` first, then the bare name.
- **`execv` does not replace the process on Windows** -- the parent exits
  immediately and a calling shell sees the command finish before the child has.
  Now `_spawnv(_P_WAIT, ...)`, returning the child's status.

`tur_global_bin_on_path` had the separator bug too, plus an exact `strcmp`
against a directory that Windows spells `C:/Users/.../bin` in one place and
`C:\Users\...\bin` in another. `tur_path_seg_eq` normalizes separators and case
on Windows and is a plain comparison on POSIX, where a path is case-sensitive
and a backslash is an ordinary character.

## The harness was POSIX-shaped in two places

Both fixed with helpers that are identities off Windows, so no POSIX assertion
changed:

- The conflict test planted a foreign file at `bin/tur-conflict`. The bin path
  is `.exe`-suffixed on Windows, so nothing was being conflicted with -- and the
  install correctly succeeded. `binfile()` plants it where the test means.
- The `--print-path-snippet` tests grepped tur's output for an MSYS path
  (`/tmp/...`) while tur prints the native spelling (`C:/Users/...`).
  `native()` (via `cygpath -m`) compares one spelling.

## Not this

The shell-quoting defects in the same file are separate and were fixed first:
`docs/reported/windows-spice-fetch-shell-quoting.md`. Before those,
`tur install` failed at the *build* step with cmd.exe's `The filename, directory
name, or volume label syntax is incorrect`; only then did it reach the symlink.
