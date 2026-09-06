# An extracted release archive cannot compile a program

> **Windows: FIXED 2026-09-05.** The lost `-L` turned out to be a real and
> separate defect (`rewrite_autolink_relative_paths` read `C:\dir` as a
> RELATIVE path and anchored it at the turmeric root), and fixing it made a
> prefix-layout archive work end to end. The Windows release now ships
> `bin/lib/include/share` and CI compiles a program out of the extracted
> archive. **Linux and macOS still ship the flat layout and are still
> affected** -- see "Status by platform".

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

| platform | layout | `tur run` from an extracted archive |
| --- | --- | --- |
| windows-x86_64 | prefix (`bin/lib/include/share`) | **works**, and CI compiles a program out of the archive every release |
| linux-x86_64 | flat | **broken** (inferred, see below) |
| linux-aarch64 | flat | **broken** (inferred) |
| macos-arm64 | flat | **broken** (inferred) |

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
| therefore Linux and macOS behave identically | **inferred** -- the code has no platform branch, but it was not run there |

The last row is the one to check first. It should take one extracted tarball and
one `tur run`.

## What remains: the same change on the other three legs

The route is settled and proven on Windows; the other legs need the packaging
half of it.

1. **Ship a prefix layout** -- `bin/`, `lib/`, `include/`,
   `share/turmeric/stdlib/` -- which `<exe_dir>/../lib` and
   `resolve_stdlib_root` step 3 already understand. Include
   `libturt_runtime.a`, not just `libturi.a`: `TUR_RT_AUTO` only engages for
   the lean archive. The guide's extract-and-symlink instructions change with
   it (`~/.local/turmeric/bin/tur`).
2. **Add the compile-from-archive check** to those legs, as `build-windows`
   now has. Unpack the artifact into a clean directory and `tur run` a two-line
   program. This is the check that would have caught the whole thing, and it is
   about fifteen lines. A `--version` smoke test proves the binary starts,
   which is a much weaker claim than it looks -- the flat archive passed it
   while being unable to compile anything.

An alternative to (1) is shipping `src/runtime/*.c` so source mode works as-is.
Smaller diff, but every user then recompiles the runtime on every build, which
is the cost the archive exists to avoid. Not recommended.

## Related

Two Windows-specific defects were in the way and are fixed:

- `find_stdlib_beside_exe` stepped up with `strrchr(dir, '/')` only, so the
  walk-up never happened on Windows and the prefix layout could not find its
  stdlib at all.
- `rewrite_autolink_relative_paths` read `C:\dir` as relative (defect 1 above).

Neither is needed by the Linux/macOS work -- the first is Windows-only and the
second, while shared code, is only triggered by a drive-lettered path.
