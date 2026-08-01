# Windows CI leg installs a package that does not exist, so it never builds

**Severity: medium (the entire Windows CI signal is absent).** The
`Windows build (MSYS2/UCRT64)` job dies at its dependency-install step, before
Configure, Build, or the `tur.exe --version` smoke check ever run. The leg has
therefore produced **zero** Windows coverage since it was added, while
appearing in the checks list as a real job.

Found 2026-08-01 while merging `origin/main` into
`claude/nested-bind-result-segfaults-uplv42` (PR #753).

## Repro

Any push that triggers `.github/workflows/ci.yml`. The job fails identically on
`main` and on unrelated feature branches:

| Head | Run / job | Result |
| --- | --- | --- |
| `133ade47` (main tip) | 30686716976 / 91333878601 | fails at install |
| `d6d64500` (PR 753) | 30687714930 / 91336717905 | fails at install |
| `3cabc3de` (PR 753) | 30687744758 / 91336797160 | fails at install |

```
[command]... 'pacman' '--noconfirm' '-S' '--needed' '--overwrite' '*'
    'mingw-w64-ucrt-x86_64-gcc' 'mingw-w64-ucrt-x86_64-cmake'
    'mingw-w64-ucrt-x86_64-ninja' 'mingw-w64-ucrt-x86_64-libedit'
error: target not found: mingw-w64-ucrt-x86_64-libedit
##[error]Error: The process 'C:\Windows\system32\cmd.exe' failed with exit code 1
```

## Root cause

`.github/workflows/ci.yml:277` lists `mingw-w64-ucrt-x86_64-libedit` in the
`msys2/setup-msys2` `install:` set. pacman reports no such target in the
configured UCRT64 repos. A `pacman -S` transaction is all-or-nothing, so one
unresolvable name aborts the whole install step and the job stops there.

## Why this is worse than a missing nicety

The workflow's own comment (`ci.yml:270-272`) states the dependency is
**optional**:

> libedit is what gives the REPL line editing (HAVE_EDITLINE in
> src/CMakeLists.txt). Without it the build still succeeds, just silently
> degraded -- so install it and keep the leg honest.

The intent was to upgrade a silent degradation into an honest one. The effect
is the opposite: making an optional dependency a hard install requirement
converts "REPL line editing is off" into "there is no Windows build signal at
all." The three existing Windows reports
([posix-inline-c-gaps](windows-posix-inline-c-gaps.md),
[pipe-reactor-fixtures](windows-pipe-reactor-fixtures-do-not-build.md),
[subprocess-and-shared-lib-gaps](windows-subprocess-and-shared-lib-gaps.md))
describe real port defects found by hand on a local MSYS2 box; none of them is
being watched by CI, because CI never gets as far as compiling.

## Fix directions

In rough order of preference:

1. **Drop the package.** The comment already concedes the build succeeds
   without it. This restores the leg immediately and costs only REPL line
   editing on the Windows leg, which nothing in the job exercises -- the smoke
   check is `tur.exe --version`.
2. **Substitute the correct package name,** if UCRT64 ships an equivalent.
   `readline` is the usual line-editing package in the mingw-w64 repos, but
   **this is unverified** -- `packages.msys2.org` is not reachable from the
   container this was diagnosed in, so the substitute name needs one check on a
   machine with network before it is trusted. Note that swapping the package
   alone is not enough if `src/CMakeLists.txt` probes specifically for
   `libedit`/`editline` headers; the `HAVE_EDITLINE` detection would need to
   accept readline too.
3. **Make the install non-fatal** (separate `pacman -S` step with `|| true`, the
   way the macOS legs already do `brew install libedit ccache || true` at
   `ci.yml:50` and `:156`). This keeps the stated intent -- try for the better
   REPL -- without letting an unavailable optional package take the leg down.
   The macOS legs are the existing precedent for exactly this pattern.

Option 1 or 3; 3 matches what the other legs already do.

## Not caused by, and not fixable in, PR #753

Recorded from that branch only because the merge surfaced it. `ci.yml` there is
byte-identical to `main` (`git diff origin/main HEAD -- .github/workflows/ci.yml`
is empty), and the job fails the same way on `main`'s own tip. Left for a
Windows/CI-focused change rather than folded into an unrelated codegen fix.
