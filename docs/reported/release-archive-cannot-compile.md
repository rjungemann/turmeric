# An extracted release archive cannot compile a program

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

1. **The `-L` never reaches the link line.** `apply_runtime_lib_mode` emits
   `-l<name> -L<dir>` (`src/main.c:2333`), and the link fails with
   `cannot find -lturt_runtime`. Supplying the same directory through
   `TUR_CC_FLAGS=-L<dir>` gets past it, so the flag is being lost rather than
   malformed. Not an `-l`-before-`-L` ordering problem: both orders were tested
   directly against this gcc and both work.
2. **`libturt_runtime.a` does not resolve `tur_set_contract_handler`.** With the
   `-L` supplied externally, that is the next failure.

Neither was chased further; they are recorded here so the next person does not
have to rediscover them.

## How far this was taken

| claim | status |
| --- | --- |
| flat archive cannot compile, on Windows | **verified**, repro above |
| the four probe paths do not match a flat archive | **verified** by reading `locate_runtime_lib` |
| the guide's instructions produce a flat layout | **verified** by reading the guide |
| therefore Linux and macOS behave identically | **inferred** -- the code has no platform branch, but it was not run there |

The last row is the one to check first. It should take one extracted tarball and
one `tur run`.

## Fix directions

1. **Ship a prefix layout** -- `bin/`, `lib/`, `include/`, `share/turmeric/stdlib/`
   -- which `<exe_dir>/../lib` and `resolve_stdlib_root` step 3 already
   understand. Changes the archive shape and the guide's instructions for every
   platform. Requires (2) and (3) below to actually work.
2. Fix the lost `-L`.
3. Make the lean archive complete, or fall back to `libturi.a` when a symbol is
   missing.
4. **Or** ship `src/runtime/*.c` in the archive so source mode works as-is.
   Smallest change, but it means every user recompiles the runtime on every
   build, which is what the archive exists to avoid.
5. **Whichever is chosen, the release workflow should compile a hello-world from
   the extracted archive.** That is the check that would have caught this, and
   it is a handful of lines: unpack the artifact into a clean directory and run
   `tur run` on a two-line program. A smoke test of `--version` proves the binary
   starts, which is a much weaker claim than it appears.

## Related

The Windows-specific half of this -- `find_stdlib_beside_exe` could not step up
a directory, so the prefix layout could not find its stdlib at all -- is fixed
separately (`src/main.c`, backslash handling in the walk-up). That fix is
necessary for direction 1 and not sufficient on its own.
