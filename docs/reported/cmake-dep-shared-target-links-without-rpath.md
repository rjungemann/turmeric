# A `:cmake-deps` target that is a shared library links with no `-rpath`

**Severity: medium** -- the binary links successfully and then fails at load.
Found 2026-08-28 while testing the `$<TARGET_FILE:...>` change that resolved
[cmake-deps-link-name-not-overridable](../archive/cmake-deps-link-name-not-overridable.md).

## Summary

`emit_link_lines` emits `$<TARGET_FILE:tgt>` for each declared `:targets`
entry. When that target is a **shared** library, the emitted token is an
absolute path to a `.dylib`/`.so` whose install name is `@rpath/...`, and
nothing on the link line supplies a matching `-Wl,-rpath`. The link succeeds
and the program dies at startup:

```
dyld[58208]: Library not loaded: @rpath/libz.1.dylib
  Referenced from: <...>/build/bin/zmulti
```

## Repro

```turmeric
(defpackage zmulti
  :name "zmulti" :version "0.1.0"
  :cmake-deps #map{
    "zlib" #map{:url "https://github.com/madler/zlib" :ref "v1.3.1"
                :targets ["zlibstatic" "zlib"]}
  })
```

`tur build .` exits 0; running the binary produces the dyld error above.
zlib's build dir holds `libz.a`, `libz.dylib`, `libz.1.dylib`, and
`libz.1.3.1.dylib`; the `zlib` target is the shared one, so its
`$<TARGET_FILE:...>` is `libz.1.3.1.dylib`.

## This is a fail-later change, but not a new gap

Under the previous name-derived line the same declaration produced a
**link-time** error instead:

```
ld: library 'zlibstatic' not found          # old: -L<dir> -lzlibstatic -lzlib
```

so for this particular declaration the failure moved from link time to run
time, which is worse to diagnose. But the underlying defect -- no `-rpath` for
a shared dependency -- is not new and is not caused by `$<TARGET_FILE:...>`.
The old line hit it whenever the derived `-l` name *did* resolve to a dylib;
that is precisely the original zlib finding
(`dyld: @rpath/libz.1.dylib -- no LC_RPATH's found`) that motivated linking by
artifact path in the first place. Linking by path fixed *preference* (naming
the static target now reliably selects `libz.a`, verified) and does not address
*shared-library runtime search*, which was always missing.

Declaring both `zlibstatic` and `zlib` is user error; the sharper form of this
report is that any dep whose `:targets` names a shared library has no way to
produce a runnable binary today.

## Fix directions

1. **Emit `-Wl,-rpath,$<TARGET_FILE_DIR:tgt>`** alongside the artifact path when
   the target's `TYPE` is `SHARED_LIBRARY`. The `TYPE` genexpr guard is already
   in `emit_link_lines` for the INTERFACE case, so this is the same shape. Note
   a build-tree rpath is not relocatable -- fine for `tur run`/dev, wrong for a
   distributable artifact, so this likely wants to be conditional.
2. **Prefer the static target when a dep exposes both.** Cheap and matches what
   most spices want, but it silently overrides an explicit `:targets`.
3. **Diagnose it.** If a resolved `$<TARGET_FILE:...>` is a shared library and
   no rpath is being emitted, warn at build time naming the dep -- the current
   failure names only `@rpath/libz.1.dylib`, which points nowhere near
   `build.tur`.

(1) is the real fix; (3) is worth having regardless, since the runtime error is
opaque.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md -- the "How the link line is derived"
  section, which currently documents artifact-path linking without mentioning
  the shared-library caveat.
