# `tur fetch` could not fetch anything on Windows

**Severity: high, Windows only.** Every URL spice dependency failed to fetch.
The package manager's shell commands interpolated POSIX `'...'` quoting into a
string cmd.exe runs, and cmd.exe does not treat `'` as a quote character at all,
so git received the quotes as part of the path.

Found 2026-09-06 while building an end-to-end test for the lockfile integrity
hash. Fixed in the same change.

## What it looks like

```
$ tur fetch
spice: fetching 'demo' from file://.../demo (ref: (default)) ...
fatal: could not create leading directories of ''./spices/demo'': Invalid argument
spice: git failed for 'file://.../demo' ref '(default)' in './spices/demo'
spice: failed to fetch 'demo'
spice: fetch completed with errors
```

The doubled quotes in `''./spices/demo''` are the whole story: git's own error
message quotes the path it was given, and the path it was given already had
quotes on it.

## Root cause

`pkg_git_fetch` built its command with `'%s'`:

```c
buf_printf(&cmd, "git clone --depth 1 -- '%s' '%s' 2>&1", url, dest_dir);
```

`system()` on Windows runs `cmd.exe /c <string>`. cmd.exe quotes with `"`, and
passes `'` through as an ordinary character.

Four sites in `pkg.c` had it -- `pkg_git_resolve`, `pkg_git_fetch` (clone and
fetch/checkout), and the cmake configure/build pair -- plus three
`2>/dev/null` redirects, which cmd.exe resolves as a path and fails on.

## The fix

`src/platform_proc.h` already carried `tur_shell_quote` and `TUR_DEVNULL`, with
the win64 CRT escaping rules worked out (including the trailing-backslash case
that eats a closing quote). The migration had simply not reached these call
sites -- `tur new`'s git scaffold used the helpers, `tur fetch` did not.

`pkg.c` gains a `cmd_arg(Buf *, const char *)` that appends one quoted
argument, and each command is now built from literal text plus `cmd_arg` calls
rather than a format string. A quote that does not fit makes the command fail
rather than run truncated.

Verified end to end on Windows against a local `file://` git repo: fetch
clones, `tur.lock` is written, `tur run` builds and runs the dependency.

## Still open: `tur install`

`src/compiler/install.c` has the same defect at three sites, not fixed here
because `tur install` is a different command from the fetch/run path this
change is about, and one of them needs more than quoting:

| line | command | needs |
| --- | --- | --- |
| 166 | `rm -rf -- '%s'` | a real port -- cmd.exe has no `rm`; either `rmdir /s /q` or an in-process recursive delete |
| 526 | `'%s' build '%s' -I '%s' -o '%s'` | `cmd_arg`, plus `tur_shell_command` -- the string STARTS with a quoted program, which is the case cmd.exe's "strip the first and last quote" rule tears in half |
| 1177 | `git ls-remote '%s' '%s' 2>/dev/null` | `cmd_arg` + `TUR_DEVNULL` |

Until then `tur install` does not work on Windows.
