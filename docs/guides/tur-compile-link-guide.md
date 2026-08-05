# `tur compile` and `tur link` -- the compile/link split

`tur build` has always been a single `cc` call that compiles **and** links in
one step. That call is uncacheable (ccache marks a multi-input compile+link
invocation "Uncacheable") and re-compiles the autolinked runtime sources from
scratch on every build. The `tur compile` + `tur link` pair splits that into a
cacheable object compile followed by a cheap link -- the same mental model you
already have from `cc -c` + `cc`.

```
tur compile foo.tur -o foo.o   # frontend lowering + `cc -c` (cacheable)  + foo.link
tur link    foo.o   -o foo     # `cc` link, reading foo.link for link flags
```

`tur build foo.tur -o foo` is exactly those two composed.

## `tur compile <file.tur> -o <out.o>`

Lowers one Turmeric source to an object file. This is the `emit-c` lowering
fused with `cc -c`, so you never have to drop to a raw `cc` for the middle step.
It writes three artifacts next to `<out.o>`:

| File | What it is |
| --- | --- |
| `<out>.o` | the compiled object (the **cacheable** unit) |
| `<out>.c` | the generated C the object was compiled from |
| `<out>.link` | a sidecar recording the resolved link flags |

Flags: `-I <dir>` (module resolution, repeatable), `-o <out.o>`,
`-B` / `--build-dir <dir>` (routes the object under `<dir>/obj/`). Enclosing
`build.tur` spice includes and `:reader-macros` are auto-discovered exactly as
`tur build` does.

`emit-c` is untouched -- it stays the "show me the C" tool and still backs the
snapshot fixtures. `tur compile` is the additive fused step, not a rename.

### The `.link` sidecar

```
# tur link sidecar v1
asan: 0
autolink:/abs/turmeric/src/runtime/hamt.c -I/abs/turmeric/src/runtime
cmake:
auxsrc:
```

The sidecar carries the **fully resolved** link flags (`-lturi` SDK anchoring,
ASan autodetect, tree-relative path anchoring, and the `-lturi`-supersedes-bare-
`.c` filter have all already run). It is the single source of truth for the link
step, so the compile and link halves cannot disagree about link flags.

## `tur link <obj/src>... -o <out>`

Links precompiled objects (and/or `.c` sources) into an executable. For each
input object it reads the sibling `.link` sidecar and unions the flags (with
whole-value dedup, so several objects of one program that each carry the same
resolved flags do not re-link a runtime source twice).

Flags: `-o <out>`, `--shared` (produce a shared library), `--link-flags "..."`
(extra linker flags, e.g. `"-L<dir> -lfoo"`).

```
tur compile app.tur -o build/obj/app.o
tur link    build/obj/app.o -o build/bin/app
```

## `tur build --split-build`

`tur build` grows two rollout flags:

- `--split-build` -- build a single file as `compile` + `link` (the object
  compile is a cacheable `cc -c`). Native builds only.
- `--no-split-build` -- force the monolithic single-`cc` build. **This is the
  default** while the split is proven out, so `tur build` output stays
  byte-identical to before.

Under `--split-build`, a single-file build runs the real `cmd_compile` then
`cmd_link` code paths (with the object + sidecar landing under the build temp
dir and cleaned up afterward), so `tur build`, `tur compile`, and `tur link`
share one implementation and cannot drift.

## `tur build --runtime=lib` -- link the prebuilt runtime

By default a program that uses runtime facilities (maps, arc, reactor, ...)
*autolinks the bare `src/runtime/*.c` sources* -- which means `cc` **recompiles**
`hamt.c` (etc.) on every build. `--runtime=lib` links the prebuilt `libturi.a`
archive instead, turning "recompile the runtime per build" into "link a static
archive built once." Static linking dead-strips to only the referenced TUs, so
the binary is unchanged.

```
tur build --runtime=lib foo.tur -o foo     # link libturi.a
tur build --runtime=source foo.tur -o foo  # autolink+recompile sources (default)
```

It also works with `tur compile` (the `.link` sidecar then records the runtime
link) and composes with `--split-build`. `TUR_RUNTIME=lib` in the environment
seeds the same default for every build (a CLI `--runtime=` flag still wins), so
CI can flip the whole suite over with one env var.

### Which archive gets linked

Two archives can back `--runtime=lib`, probed per directory in this order:

1. **`libturt_runtime.a`** (preferred) -- a lean, **non-sanitized** archive of
   exactly the autolinkable runtime TUs (`hamt.c`, `symbols.c`, `tur_string.c`).
   Built by the `turt_runtime` CMake target. Because it is non-ASan, linking it
   is behaviorally identical to the bare-source recompile -- no sanitizer is
   imposed on your program.
2. **`libturi.a`** (fallback) -- the full runtime library. In a Debug build this
   is AddressSanitizer-instrumented, so the ASan autodetect pulls
   `-fsanitize=address,undefined` into the link and your program then runs under
   ASan/LeakSanitizer. That is by design when only the full lib is available;
   use a Release/non-sanitized `libturi.a` (or `ASAN_OPTIONS=detect_leaks=0`) if
   you want a plain run.

Archive directories are searched in order: `$TUR_RUNTIME_LIB` (an archive file
or its directory) -> `<tur_exe_dir>/src` -> `<turmeric_root>/build/src`. A
prefix-installed SDK's `libturi.a` is found automatically once `-lturi` is on
the line. Set `TUR_RUNTIME_LIB` if your archive lives elsewhere.

## Why this exists

See [docs/upcoming/v2/tur-link-and-build-split-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/v2/tur-link-and-build-split-plan.md).
The short version: splitting the compile from the link makes the object
compiles cacheable (ccache hits on unchanged runtime sources across every
fixture and every run) and stops re-compiling the runtime per build -- the
dominant cost in the test-suite wall-clock.
