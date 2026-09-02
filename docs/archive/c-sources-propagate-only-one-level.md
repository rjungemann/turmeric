# `:c-sources` propagates one level, so a library spice's implementation TU is lost two hops away

**Status: RESOLVED 2026-09-02** -- see the Resolution at the end.

**Severity: medium** -- a spice that ships a hand-written C translation unit
works for its direct consumers and silently fails to link for anyone further
down the graph. Found 2026-08-29 while looking at whether
`spices/raygui`'s `cmake-deps/` shim can be retired in favor of `:c-sources`.

**Read-verified against `src/main.c`, not reproduced.** The code is a flat loop
with no recursion, so the shape is unambiguous, but no fixture was built to
watch it fail.

## Summary

`collect_spice_aux_c` (`src/main.c`) gathers `:build-opts :c-sources` /
`:c-includes` from exactly two places:

1. the manifest of the project being built, and
2. the manifest of each of **its direct `:spices` entries**.

It never recurses into those deps' own `:spices`, and keeps no visited set:

```c
for (int i = 0; i < m.n_c_includes; i++)   /* the root's own */
    buf_printf(includes, " -I%s/%s", root, m.c_includes[i]);
...
for (int i = 0; i < m.n_spices; i++) {     /* one level, then stop */
    ...
    for (int j = 0; j < dm.n_c_sources; j++)
        buf_printf(sources, " %s/%s", dep_dir, dm.c_sources[j]);
}
```

So `app -> raygui` compiles raygui's TU, and `app -> ui-kit -> raygui` does
not. The failure is at link time, in the *app*, naming symbols
(`GuiButton`, ...) that belong to a dependency two hops away, with nothing
pointing at the manifest that should have contributed them.

## The asymmetry that makes it surprising

`:cmake-deps` **is** fully transitive -- `pkg_collect_transitive_cmake_deps`
walks the whole `:spices` closure with a worklist and a visited set. Native
dependencies declared three spices away are found; a vendored `.c` declared two
spices away is not. Both are "native code this spice needs", declared in the
same `build.tur`, and they behave differently.

## Why it matters now: the raygui shim

`spices/raygui` currently carries a hand-written `cmake-deps/raygui/`
CMake shim whose entire job is to compile one TU:

```c
/* raygui_impl.c */
#include <raylib.h>
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>
```

raygui is a single header with no CMakeLists, so the shim exists to give that
TU a home and a target. `:c-sources` is the natural replacement -- vendor
`raygui.h` next to `raygui_impl.c` (the same move that retired glad's
generator) and declare:

```turmeric
:build-opts #map{
  :c-sources  ["cmake-deps/raygui/raygui_impl.c"]
  :c-includes ["cmake-deps/raygui"]
}
```

The pieces are already in place for that: the aux `.c` is compiled in the same
`cc` invocation that receives the `:cmake-deps` `-I` flags, so
`#include <raylib.h>` resolves from the raylib dep's manifest entry without
extra plumbing.

What stops it being a clean swap is the depth limit. `raygui`'s direct
consumers would be fine; `sdf-raylib`, `ecs-raylib`, or any app that reaches
raygui through another spice would lose the implementation and fail to link.
The CMake shim has no such limit, because `:cmake-deps` is transitive -- which
is precisely the asymmetry above.

## Fix directions

1. **Walk the closure, like `:cmake-deps` already does.** Reuse the same
   worklist + visited-set shape as `pkg_collect_transitive_cmake_deps` rather
   than growing a second traversal with different semantics. The visited set is
   not optional here: the same spice reached by two paths must contribute its
   `.c` **once**, or the link fails on duplicate symbols instead of missing
   ones.
2. **Deduplicate by resolved path** regardless of (1). Two deps that vendor the
   same third-party `.c` (a second copy of `stb_image.c`, say) would today
   produce duplicate symbols; `sidecar_accum_unique` dedups whole sidecar
   *fields*, not individual sources.
3. **Diagnose the shortfall.** If a `:spices` dep more than one hop away
   declares `:c-sources` and they are being dropped, say so -- the current
   failure is an undefined symbol in the consumer with no link back to the
   manifest that owns it.

(1) with (2) is the real fix; they are the same change.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md -- the "Vendoring C Sources" section
  should state how far `:c-sources` propagates, which it currently does not.

## Resolution (2026-09-02)

Fix directions (1) and (2), as one change. `collect_spice_aux_c`
(`src/main.c`) is now a worklist walk over the whole `:spices` closure with
a realpath-keyed visited set -- the same shape as
`pkg_collect_transitive_cmake_deps`, and using the same resolver
(`pkg_resolve_spice_dep_dir`, now exported from `pkg.c`) so the two closures
agree on what a dependency is: workspace sibling, `:path`, fetched, or
`:global`. Every `:c-sources` entry is emitted once by resolved path, and
every `:c-includes` dir likewise, so a spice reached by two routes or two deps
vendoring the same file never produce duplicate symbols. Direction (3), a
dedicated "dropped source" diagnostic, is moot: nothing is dropped any more.

Pinned in `tests/spice-c-sources-tests.sh`: `consumer-two-hops` (the vendor
is reachable only through `consumer-of-aux`; the old binary fails its link on
`vendor_value`, the new one exits 7) and `consumer-diamond` (the vendor reached
both directly and through the intermediate links once). The developing-spices
guide's "Vendoring C Sources" rules now state the propagation depth. The
raygui CMake shim this report was found under can now move to `:c-sources`.
