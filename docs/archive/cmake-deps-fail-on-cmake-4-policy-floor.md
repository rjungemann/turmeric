# A `:cmake-deps` dep with an old `cmake_minimum_required` fails on CMake 4.x

**RESOLVED 2026-08-28.** `pkg_cmake_build` passes
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` when the host CMake is >= 4 (fix
direction (1)), gated so CMake 3.x does not report an unused cache variable
on every configure. `TUR_CMAKE_NO_POLICY_MIN=1` opts out.

**Severity: medium** -- any dependency whose upstream `CMakeLists.txt` declares
`cmake_minimum_required(VERSION <3.5)` aborts the whole configure, so no dep in
that project builds. Found 2026-08-28 on CMake 4.3.3 while building
`spices/opengl`.

## Summary

CMake 4 removed compatibility with `cmake_minimum_required` floors below 3.5.
A FetchContent-built dependency that has not updated its floor -- hiredis, and
plenty of other stable C libraries -- now fails at configure:

```
CMake Error at build/_deps/hiredis-src/CMakeLists.txt:1 (CMAKE_MINIMUM_REQUIRED):
  Compatibility with CMake < 3.5 has been removed from CMake.

  Update the VERSION argument <min> value.  Or, use the <min>...<max> syntax
  ...
  Or, add -DCMAKE_POLICY_VERSION_MINIMUM=3.5 to try configuring anyway.
```

`tur` never passes `-DCMAKE_POLICY_VERSION_MINIMUM` when it invokes CMake
(`grep -rn CMAKE_POLICY_VERSION_MINIMUM src/` finds nothing), and there is no
manifest key that would let a spice ask for it. `:options` sets cache variables
on the generated project, which does not reach the policy floor the same way a
command-line `-D` does at the top-level configure.

## turmeric already knows about this

The project's own bootstrap instructions carry the exact workaround:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

So the compiler's own build survives CMake 4 and the dependency builds it
generates do not.

## Repro

On CMake >= 4.0, any spice with a `:cmake-deps` entry on a library with an old
floor:

```sh
cd turmeric-spices/spices/opengl
tur fetch    # CMake Error at .../hiredis-src/CMakeLists.txt:1
```

## Why CI has not caught it

The GitHub runners' pinned CMake is still 3.x, so this is currently a
local-developer failure on any machine that has moved to CMake 4. It will
arrive in CI on its own schedule when the runner image updates -- the same
shape as a toolchain-floor bump, and worth getting ahead of.

## Fix directions

1. **Pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` from `pkg_cmake_build`** when the
   host CMake is >= 4, matching what the bootstrap already does. Cheapest, and
   it fixes every existing spice with no manifest change. It is a
   compatibility shim, so it should be easy to turn off.
2. **A manifest key** (`:cmake-policy-min "3.5"`, or a `:build-opts` entry) for
   spices that want to be explicit. More honest, but it makes every affected
   spice edit its manifest for a problem none of them caused.
3. **Do nothing and let deps update their floors.** Correct in principle and
   unbounded in practice -- the spice author does not control the dependency.

(1) with an escape hatch is the pragmatic answer; a project that genuinely wants
the strict floor can opt out.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md -- the `:cmake-deps` section, if a
  manifest key is added.
