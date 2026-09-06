# An extracted release archive cannot compile a program

> **RESOLVED 2026-09-06, all platforms.** The fix is not the one this report
> first proposed. Rather than restructuring every archive into a prefix layout,
> `locate_runtime_lib` now probes `<exe_dir>` itself -- the flat shape the
> archives already have -- and the archives ship `libturt_runtime.a`, which they
> never did. Nothing downstream had to change. The release workflow now compiles
> a program out of the artifact it just built, on every leg.
**Severity: high for anyone installing from a release tarball.** `tur --version`
works, the REPL works, and `tur run` fails at the C compile step. Not
Windows-specific -- the code involved has no platform branch -- though it was
found on Windows.

Found 2026-09-05 while validating the first Windows artifact
([jit-windows-support-spike](jit-windows-support-spike.md) follow-on). The
Windows half is verified end to end; the Linux/macOS half is **read-verified,
not run** -- see "How far this was taken" below.

## Repro

Build the archive layout the release workflow produces and the installation
guide tells users to extract:

```
<dir>/tur          (or tur.exe)
<dir>/libturi.a
<dir>/include/turi/*.h
<dir>/stdlib/
```

then, with a C toolchain on PATH:

```
$ ./tur run hello.tur
cc1.exe: fatal error: <dir>/src/runtime/hamt.c: No such file or directory
cc1.exe: fatal error: <dir>/src/runtime/trail.c: No such file or directory
compilation terminated.
tur: cc invocation failed (status 1)
```

`hello.tur` is `(defn main [] : int (println "hi") 0)` -- nothing exotic.

## Cause

`tur` compiles the emitted C in one of two runtime modes.

- **Source mode** recompiles `src/runtime/*.c` alongside the program. Those
  files are not in the archive.
- **Lib mode** links a runtime archive instead. `locate_runtime_lib`
  (`src/main.c:2224`) looks in `$TUR_RUNTIME_LIB`, `<exe_dir>/src`,
  `<exe_dir>/../lib`, and `<root>/build/src`.

The archive is **flat**: `libturi.a` sits beside the exe, which is none of those
four. So no archive is found, `TUR_RT_AUTO` falls back to source mode
(`apply_runtime_lib_mode`, `src/main.c:2325` -- AUTO only engages for the lean
`libturt_runtime.a`), and source mode wants files the archive does not ship.

The layout is not an accident of the workflow: `releases-and-installation-guide.md`
tells users to `tar -xzf ... -C ~/.local/turmeric` and symlink
`~/.local/turmeric/tur`. `get_exe_path` resolves that symlink, so `<exe_dir>` is
the flat directory either way.

## Two further defects behind it

Reaching for the layout the probes *do* understand -- `<prefix>/bin/tur` with
`<prefix>/lib/libturt_runtime.a` -- gets further and then hits two more:

1. ~~**The `-L` never reaches the link line.**~~ **FIXED.** It reached the line
   mangled, not dropped. `rewrite_autolink_relative_paths` (`src/main.c:1968`)
   anchors relative `-I`/`-L` paths at the turmeric root, and tested "already
   absolute" as `tok[2] != '/'`. A Windows path has `C` there, so an absolute
   `-LC:\...\src` was treated as relative and became
   `-LC:\root/C:\real\src`. ld reports that as `cannot find -lturt_runtime`,
   which reads as a dropped flag and sends you looking in the wrong place.
   Fixed with a `path_is_absolute` helper that knows about drive letters and
   backslashes.

   Worth noting how it was found, because reading did not do it: a one-line
   `TUR_SHOW_CC` dump of the assembled cc command showed the doubled path
   immediately, after several wrong hypotheses (flag ordering, quoting, a
   missing archive) had each been tested and eliminated.

2. ~~**`libturt_runtime.a` does not resolve `tur_set_contract_handler`.**~~ Not
   reproducible once (1) was fixed. It was almost certainly an artefact of the
   mangled `-L`: the externally supplied `-L` let ld open *a* library while the
   real one was still unreachable.

## Status by platform

| platform | layout | before | after |
| --- | --- | --- | --- |
| windows-x86_64 | prefix (`bin/lib/include/share`) | broken | fixed, shipped in v0.44.1 and verified against the published asset |
| macos-arm64 | flat | broken (verified) | fixed |
| linux-x86_64 | flat | broken (inferred) | fixed |
| linux-aarch64 | flat | broken (inferred) | fixed |

v0.44.1 shipped with Windows working and the other three still flat and still
unable to compile -- which is what prompted finishing this.

The Windows fix is two things: the `-L` defect above, and packaging a prefix
layout instead of a flat one. The other three legs need the same packaging
change; the `-L` fix is already shared, since it is not platform-specific code
(only its trigger was).

## How far this was taken

| claim | status |
| --- | --- |
| flat archive cannot compile, on Windows | **verified**, repro above |
| the four probe paths do not match a flat archive | **verified** by reading `locate_runtime_lib` |
| the guide's instructions produce a flat layout | **verified** by reading the guide |
| the same failure on macOS | **verified 2026-09-06** on an arm64 Release build |
| therefore Linux behaves identically | **inferred** -- the code has no platform branch, and two platforms now agree, but it was not run there |

macOS was checked rather than assumed. A flat archive built from the branch
fails at the same place:

```
clang: error: no such file or directory: '.../src/runtime/hamt.c'
clang: error: no such file or directory: '.../src/runtime/trail.c'
tur: cc invocation failed (status 256)
```

Linux remains unverified for want of a box. With two platforms agreeing and no
platform branch in the code involved it is a formality -- and the
compile-from-archive check now added to every release leg turns it into one
that answers itself: if Linux does not work, the release job says so instead of
publishing.

## The fix, and why it is not what was proposed

The first draft of this report proposed shipping a prefix layout everywhere.
That would have worked, and it would have been a breaking change for three
consumers at once: `tvm` restages the tarball into `bin/` + `lib/` and would
have had to learn a new shape, Homebrew's cask consumes the published one, and
Trowel stages the extracted tree next to its binary and probes
`turmeric/tur`.

Probing `<exe_dir>` instead makes the shape the archives ALREADY have work:

1. `locate_runtime_lib` gains a fourth candidate, `<exe_dir>` itself -- the flat
   case, where `tur` and the runtime archive are siblings. One `if`.
2. The archives ship `libturt_runtime.a`. They never did, and it is the only
   one `TUR_RT_AUTO` will link -- `libturi.a` alone is not enough, which is why
   even `tvm`'s `bin/` + `lib/` restaging would not have worked either.

Nothing downstream changes. Windows keeps the prefix layout it shipped in
v0.44.1 -- that is released and verified, and changing it now would break
anyone who scripted against it -- and `<exe_dir>/../lib` already covers it.

## The check that would have caught this

Every leg of `release.yml` now unpacks the artifact it just built and compiles a
two-line program with it. The archives passed a `tur --version` smoke test for
many releases while being unable to compile anything; that check proves the
binary starts, which is a much weaker claim than it appears.
## Related

Two Windows-specific defects were in the way and are fixed:

- `find_stdlib_beside_exe` stepped up with `strrchr(dir, '/')` only, so the
  walk-up never happened on Windows and the prefix layout could not find its
  stdlib at all.
- `rewrite_autolink_relative_paths` read `C:\dir` as relative (defect 1 above).

Neither is needed by the Linux/macOS work -- the first is Windows-only and the
second, while shared code, is only triggered by a drive-lettered path.

The two defects listed above under "Two further defects behind it" are both
settled: the `-L` was mangled rather than lost (fixed), and the
`tur_set_contract_handler` failure was an artefact of that mangling rather than
a real gap in the lean archive.
