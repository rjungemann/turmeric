# A `:cmake-deps` `:path` resolves against a different base in each code path

**Severity: medium** -- a `:path` that is correct for the spice that declares
it resolves one directory too high when the same dep is reached transitively,
so `add_subdirectory` points at a directory that does not exist. Found
2026-08-28 immediately behind
[transitive-path-cmake-dep-absolutized-then-reprefixed](transitive-path-cmake-dep-absolutized-then-reprefixed.md);
that report's doubling is fixed, this is what was underneath it.

## Summary

The generated `cmake/CMakeLists.txt` lives in `<spice>/cmake/`, so a relative
`add_subdirectory` argument resolves from **`<spice>/cmake/`**. Every spice in
`turmeric-spices` writes its `:path` accordingly and says so:

```turmeric
;; :path is spliced into cmake/CMakeLists.txt as add_subdirectory("./<path>")
;; and CMake resolves that relative to cmake/, so the leading ../ is what
;; reaches the spice root.
"raygui" #map{:path "../cmake-deps/raygui" :targets ["raygui"]}
```

`<spice>/cmake/` + `../cmake-deps/raygui` = `<spice>/cmake-deps/raygui`. Correct.

But `append_cmake_dep_with_conflict_check` absolutizes a transitively-collected
`:path` against **`origin_dir`, the manifest directory** (`<spice>/`), not
`<spice>/cmake/`:

```
<spice>/ + ../cmake-deps/raygui  =  spices/cmake-deps/raygui     <-- one level too high
```

so the emitted line is

```cmake
add_subdirectory("/.../spices/raygui/../cmake-deps/raygui" ...)
# resolves to /.../spices/cmake-deps/raygui, which does not exist
```

```
CMake Error at CMakeLists.txt:58 (add_subdirectory):
  add_subdirectory given source
  "/.../spices/raygui/../cmake-deps/raygui"
  which is not an existing directory.
```

The same `:path` string therefore means two different directories depending on
whether the dep is reached directly or transitively.

## `tur fetch` and `tur build <dir>` also disagree

Under `tur fetch` the project's **own** `:path` dep is emitted relative
(`add_subdirectory("././../cmake-deps/opengl")`, `project_dir` is `"."`) and is
correct. Under `tur build <dir>` `project_dir` is the absolute spice directory,
so the same dep becomes `<spice>/../cmake-deps/opengl` -- one level too high, the
same defect as the transitive case.

So there are three combinations and they do not agree:

| | base used | correct? |
| --- | --- | --- |
| own dep, `tur fetch` | `<spice>/cmake/` (via a relative `./`) | yes |
| own dep, `tur build <dir>` | `<spice>/` | no |
| transitive dep, either | `<spice>/` (absolutized at collection) | no |

## Repro

```sh
cd turmeric-spices/spices/opengl
tur fetch     # CMake Error: .../spices/raygui/../cmake-deps/raygui not an existing directory
```

Needs a compiler carrying the doubling fix, otherwise the earlier defect masks
this one.

## Fix direction

Pick one base and make every path agree on it. `<spice>/cmake/` is the one the
existing manifests are written against, so changing the *meaning* would break
every spice in the wild; the code should move to match the docs, not the
reverse:

- absolutize transitively-collected `:path` against `<origin_dir>/cmake`, and
- have the `tur build <dir>` emitter resolve an own `:path` against
  `<project_dir>/cmake` rather than `<project_dir>`.

Worth adding a fixture with a workspace sibling that declares a `:path`
cmake-dep -- the existing `transitive-cmake-deps-*` fixtures use a fake
unfetchable `:url` dep with no `:targets` and no `:path`, so they exercise the
resolver and never this join. That gap is why both this and the doubling
survived.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md -- state which directory `:path` is
  relative to; it is currently only documented in spice-side comments.
