# `:cmake-deps` derives the `-l` name from the target name, with no override

**Severity: medium** (ergonomics/expressiveness hole, not a miscompile -- but it
forces a hand-written CMake shim per dependency, and four of
`turmeric-spices`'s real deps need one). Found 2026-08-28 getting
`turmeric-spices` CI green, against `tur v0.40.0` / turmeric `5c9d533`.

Filed as a finding rather than a defect: nothing here produces a wrong answer.
It is a gap in what the manifest can express, and the fix is small and
mechanical.

## Summary

Without `:targets`, `tur fetch` guesses a dependency's include dir
(`${SOURCE_DIR}/include`, else `${SOURCE_DIR}`) and its `-l` name (the target
basename, or the dep name). With `:targets`, the include dir and the link
*directory* are both derived properly from generator expressions -- but the link
*name* is still the target basename, and there is no way to say otherwise.

## The five dependencies that defeat it

| Dep | Why the guess fails |
| --- | --- |
| **raygui** | header-only, header in `src/`, no CMakeLists, no library at all |
| **glfw** | target `glfw`, `OUTPUT_NAME` `glfw3`, archive in `<BINARY_DIR>/src` |
| **glad** | a Python *generator*; nothing is built, so `<glad/gl.h>` never exists and `-lglad` never exists |
| **libpq** | `find_package` imports `PostgreSQL::PostgreSQL`, so the derived name is `-lPostgreSQL` while the file is `libpq.so` |
| **zlib** | static target is `zlibstatic` with `OUTPUT_NAME z`, so `-lzlibstatic` does not resolve. `BUILD_SHARED_LIBS "OFF"` does not stop zlib building a shared target either, and `-lz` then prefers it |

The zlib row is a **macOS-only hard failure** and the sharpest illustration of
the gap. It fails at runtime, not link time:

```
dyld: Library not loaded: @rpath/libz.1.dylib -- no LC_RPATH's found
```

Linux survives it only by accident: `-lz` falls back to the system
`libz.so.1`, which exists. There is no such fallback on macOS.

Note what `:targets` can and cannot do here. Declaring `:targets
["zlibstatic"]` fixes the link *directory* (via `$<TARGET_FILE_DIR:...>`) and
still leaves `-lzlibstatic`, which does not resolve. So the documented escape
hatch does not reach this case, and the only current workaround is a
`cmake-deps/` shim whose target name *is* the desired `-l` name -- i.e.
renaming a target to work around a string operation.

## Where the two halves are, in the source

Both are in `src/compiler/pkg.c`, and they are asymmetric -- which is the actual
finding.

**Link dir: already correct when `:targets` is present.** `emit_link_lines()`
(`pkg.c:2514`) uses a generator expression:

```c
fprintf(f, "%s\\\"$<TARGET_FILE_DIR:%s>\\\"", j ? ", " : "", t);   /* :2522 */
```

so glfw's `<BINARY_DIR>/src` is found correctly. The original framing of this
finding said the guess "gets both the name and the directory wrong" for glfw;
with `:targets` declared, only the name is wrong.

**Link name: still a string operation.** Eleven lines later:

```c
const char *bn = cmake_target_basename(d->targets[j]);            /* :2527 */
fprintf(f, "%s\\\"%s\\\"", j ? ", " : "", bn);                    /* :2528 */
```

`link_dirs` and `link_libs` are derived from the same target string, so a target
whose name differs from its library's base name cannot be expressed.

The include-dir half of this is already acknowledged in `pkg.c`'s own comment
(`:3185`):

> the heuristic misses libraries (e.g. yyjson) that publish their public header
> from a non-standard subdir like `${SOURCE_DIR}/src`

-- and was solved by sourcing include dirs from the first target's
`INTERFACE_INCLUDE_DIRECTORIES` when `:targets` is declared (`:3189`). The
link-name half has the identical shape and did not get the identical treatment.

## Three knobs, and one of them subsumes the other two

1. **`$<TARGET_FILE:...>` -- link by full artifact path.** For a dep declared
   with `:targets`, emit the generator expression for the built file itself
   instead of reconstructing `-L<dir> -l<name>` from the target's *name*. This
   sidesteps `OUTPUT_NAME`, namespace aliasing, **and** static-vs-shared
   preference in one move, because CMake hands back the exact artifact the
   target produces. It fixes glfw, libpq **and zlib** -- zlib specifically,
   which the other two knobs do not fully reach (see below).

   This is the structurally right answer: `tur` currently derives a link line
   from a target name by string manipulation, when CMake will simply tell it
   what the target is.

2. **`$<TARGET_FILE_BASE_NAME:...>` instead of the target basename at
   `pkg.c:2528`.** The smaller version of (1): CMake resolves `OUTPUT_NAME` and
   namespace aliasing, so glfw and libpq work with no user action -- exactly
   parallel to what `:3189` already does for include dirs.

   **It does not fix zlib.** It would yield the right *name* (`-lz`), but
   `link_dirs` still points at a directory containing both `libz.a` and
   `libz.1.dylib`, and `-lz` prefers the dylib -- which is the rpath failure.
   Only (1) picks the specific artifact.

3. **A per-dep `:link-libs ["pq"]` override.** The escape hatch for anything
   generator expressions cannot reach. `PkgManifest` already carries
   `link_libs`/`n_link_libs` (`pkg.h:119`) at the *manifest* level and
   `pkg.h:64` at the module level, and `main.c:1905` already emits `-l` from it
   -- so the plumbing exists and this is mostly a parse + thread-through in
   `PkgCmakeDep`.

None of the three helps raygui or glad, which are the genuinely degenerate
cases: one builds no library and one builds nothing at all. Those want a way to
say "this dep contributes include dirs only, link nothing" -- an empty
`:link-libs []` under (3) expresses that exactly, which is why (3) is worth
having even alongside (1).

## Suggested order

**(1) then (3).** (1) is barely larger than (2), fixes strictly more (zlib),
and removes a whole class of "the target name is not the library name" problems
rather than the two instances currently in front of us. (3) then covers the
degenerate deps that have no target to ask about.

Skip (2) unless (1) turns out to be blocked -- it is the same edit site for
less coverage, and landing it first would make zlib look fixed while leaving
the dylib preference in place.

Worth doing in the same pass as
[cmake-deps-cannot-express-framework](cmake-deps-cannot-express-framework.md):
that report needs `INTERFACE_LINK_LIBRARIES` read from the same targets, and
both are the same underlying correction -- ask CMake about the target instead
of manipulating its name.

## Guides to update when fixed

- docs/guides/consuming-spices-guide.md -- the `:cmake-deps` / `:targets`
  section; document the derived `-l` name and any new override.
- docs/guides/developing-spices-guide.md -- if `:link-libs` becomes manifest
  surface.
