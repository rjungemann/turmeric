# `libturi.a` drops `runtime/symbols.c` to a basename collision

**Severity: low today, latent.** No known breakage on the `cc` path. Found
while linking `libturi` whole into the J0 JIT harness.

## Summary

The tree has two files named `symbols.c`:

```
src/compiler/symbols.c
src/runtime/symbols.c
```

Both are in `TUR_CORE_SOURCES`, both compile to an object named `symbols.c.o`,
and `libturi.a` is a flat archive keyed by member basename. Only one survives:

```sh
$ ar t build-jit/src/libturi.a | grep -i symbol
symbols.c.o                      # just the one

$ nm build-jit/src/libturi.a | grep -c tur_sym_register
0                                # the RUNTIME symbols.c lost the race

$ nm build-jit/src/CMakeFiles/turt_runtime.dir/runtime/symbols.c.o | grep tur_sym_register
00000000000001b0 T tur_sym_register
```

So `libturi.a` does not contain `tur_sym_register`, `tur_sym_intern`, or
anything else defined in `src/runtime/symbols.c`, despite that file being listed
in the sources that build it.

CMake happens to compile the two into distinct object paths
(`tur_core.dir/compiler/symbols.c.o` and `.../runtime/symbols.c.o`), so the
build succeeds; the collision only bites when the objects are collected into an
archive and addressed by basename.

## Why nothing breaks today

`tur build` links `libturt_runtime.a` alongside `libturi`, and that archive
carries an uncolliding copy of `runtime/symbols.c`. The missing member is
therefore always supplied from elsewhere on the paths anyone currently uses.

It surfaced in the JIT harness because a JIT resolves runtime symbols **by name
at run time**, not by reference at link time, so it needs the whole runtime
resident and exported -- `--whole-archive libturi` -- rather than whichever
members a reference happens to pull in. Under that link `tur_sym_register` is
absent and any program using interned symbols fails with
`unresolved import: tur_sym_register`. Cost: 1 fixture.

## Repro

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
cmake --build build-jit -j --target libturi
nm build-jit/src/libturi.a | grep -c tur_sym_register    # 0, expected non-zero
```

## Fix directions

Cheapest and least invasive: rename one of the two files. `src/compiler/symbols.c`
is the newer/more specific of the pair by role, and something like
`compiler/sym_table.c` removes the ambiguity for readers as well as for `ar`.

Alternatively, give the archive members unique names. CMake does not do this by
default for flat archives; an `OBJECT` library with per-source output names, or
building `libturi` from explicitly-named objects, would.

Worth checking whether any other basename is duplicated across
`TUR_CORE_SOURCES` while fixing this -- the same silent-drop applies to any
pair, and nothing in the build warns.

## Workaround in place

`tools/jit-spike/CMakeLists.txt` compiles `src/runtime/symbols.c` directly into
the harness alongside the whole-archive `libturi` link, with a comment pointing
here. That should be reverted when the collision is fixed properly.

## Provenance

docs/upcoming/jit-engine-j0-findings.md section 11.5, during the S2
host-symbol-boundary work.
