# Transitive `:cmake-deps` are merged into one CMake project, so two siblings providing the same target collide

**RESOLVED 2026-08-28 via fix direction (1).** The `:cmake-deps` walk no longer
seeds every workspace member; all three callers (`tur fetch`, `tur run`,
`tur build`) now use the manifest's declared `:spices` closure.
`TUR_CMAKE_DEPS_WORKSPACE_WIDE=1` restores the old behavior.

The collision was a symptom. The measured cost of seeding was that building
`spices/opengl` -- which needs glfw and glad -- configured **15** unrelated
native dependencies (mbedtls, sqlite3, libpq, rtaudio, hiredis, ...). It is now
1. `spices/opengl` fetches, configures, builds `libglfw.a` + `libglad.a`, and
`tur build .` succeeds, on macOS, for the first time.

This reverts the "seed all members" decision from
[workspace-transitive-native-deps-plan](history/workspace-transitive-native-deps-plan.md),
which rated itself "Low / quality-of-life. Not a correctness bug" and chose all
members over only-declared on consistency grounds with the include-path
resolver. The include path is cheap to over-share; native builds are not. Three
independent findings had already converged on the same conclusion:
`tur build .` opted out in
[tur-build-cmake-deps-workspace-overreach](history/tur-build-cmake-deps-workspace-overreach.md)
(2026-06-23), the spices CI hides the workspace root during `tur fetch` to work
around it, and that CI comment records the evidence that the declared closure
suffices -- "every spice that genuinely needs a sibling's native lib declares
that sibling in its own :spices ... verified: ecs-raylib still gets raylib,
plot still gets plutovg, httpd still gets yyjson+mbedtls."

Fix direction (3) -- re-reporting a collision with the two spices named -- was
not implemented and is no longer reachable by this route.

**Severity: medium** -- `add_library cannot create target "glfw" because
another target with the same name already exists`. The configure aborts and no
dependency builds. Found 2026-08-28 building `spices/opengl` after the `:path`
base-dir and CMake-4 policy fixes cleared the earlier failures.

## Summary

`tur` collects the `:cmake-deps` of the project **and every workspace sibling
reachable through `:spices`** into a single generated
`cmake/CMakeLists.txt`. Every dep therefore shares one CMake target namespace.
Two siblings that each provide the same target name -- directly or by
vendoring -- cannot coexist.

Concretely, in `turmeric-spices`:

- `spices/opengl`'s shim `FetchContent`s **glfw** and creates a `glfw` target.
- `spices/raygui` pulls in **raylib**, which *vendors its own copy of glfw* at
  `raylib-src/src/external/glfw` and adds a `glfw` target of its own.

Both land in the same project:

```
CMake Error at build/_deps/raylib-src/src/external/glfw/src/CMakeLists.txt:2 (add_library):
  add_library cannot create target "glfw" because another target with the
  same name already exists.  The existing target is a static library created
  in source directory ...

CMake Error at build/_deps/raylib-src/src/external/glfw/src/CMakeLists.txt:23 (add_custom_target):
  add_custom_target cannot create target "update_mappings" because another
  target with the same name already exists.
```

Note neither spice is at fault and neither declares the other's dependency.
The collision is created by merging them.

## Repro

```sh
cd turmeric-spices/spices/opengl
tur fetch
```

Needs a compiler carrying the `:path` base-dir fix and the CMake-4 policy fix,
or an earlier failure masks this one. The three defects are stacked: each fix
reveals the next.

## Why it is structural, not a naming accident

The conflict detector in `append_cmake_dep_with_conflict_check` keys on the
**dep name** in the manifest (`"glfw"` vs `"raygui"`), which is a different
namespace from the **CMake target names** those deps go on to create. Two deps
with distinct manifest names can each create a target called `glfw`, and
nothing looks at that until CMake does. Vendoring makes it worse: raylib's
glfw is not declared anywhere in any `build.tur`, so no manifest-level check
could have seen it.

## Fix directions

1. **Do not merge unrelated siblings' deps.** The reason they are merged is
   that a spice's modules can import a sibling's without declaring it. Scoping
   the CMake project to the deps actually reachable from the spice being built
   would fix this and shrink configure time; it is the largest change.
2. **Give each dep its own CMake project / build tree**, configured
   separately, and merge only the resulting manifests. Removes the shared
   namespace entirely. Costs one configure per dep.
3. **Detect and report it.** Before configuring, ask whether two deps will
   create the same target -- not generally answerable ahead of CMake, but the
   *error* can be caught and re-reported as "spice A and spice B both provide
   target `glfw`", which is far more actionable than the raw CMake text
   pointing into a vendored source tree the user never mentioned.
4. **Let a dep opt out of a vendored sub-build** via `:options` (raylib has
   `USE_EXTERNAL_GLFW`), which fixes this instance and not the class.

(3) is worth doing regardless of which of (1)/(2) is chosen -- the current
failure names a directory under `_deps/raylib-src/` and nothing connects it to
`spices/opengl`'s manifest.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md -- the transitive `:cmake-deps`
  section should state that sibling deps share one CMake target namespace.
