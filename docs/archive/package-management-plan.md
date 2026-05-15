# Package Management System Plan for Turmeric (Spice)

> **Status:** Active design -- v2 target
> **Prerequisite:** Module system (Phase M0-M7 complete)
> **Related:** [cmake-cpm-integration-plan.md](cmake-cpm-integration-plan.md), [module-system-plan.md](module-system-plan.md)

---

## Overview

Turmeric's package manager is called **Spice**. A single `build.tur` file in the
project root declares the package identity, its Turmeric dependencies (also
called *spices*), and its optional C/CMake dependencies. The `tur` CLI handles
fetching, building, and linking -- no separate package manager binary needed.

The name fits the theme: you season your project with spices.

---

## The `build.tur` File

Every Turmeric project has one `build.tur` at its root. It is a valid Turmeric
source file evaluated at build time by the `tur` tool.

```scheme
;;; build.tur -- project manifest for "geom"
(defpackage geom
  :name        "geom"
  :version     "0.2.1"
  :description "2D/3D geometry library for Turmeric"
  :license     "MIT"
  :authors     ["Alice Smith <alice@example.com>"]
  :repository  "https://github.com/alice/tur-geom"
  :homepage    "https://tur-geom.docs.example.com"

  ;; ----------------------------------------------------------------
  ;; Spices: Turmeric package dependencies
  ;; Each entry is a name mapped to a source descriptor.
  ;; Supported sources: :url (git), :path (local), :registry (future)
  ;; ----------------------------------------------------------------
  :spices {
    "math"  {:url "https://github.com/bob/tur-math"    :ref "v1.5.2"}
    "test"  {:url "https://github.com/bob/tur-test"    :ref "v0.3.0"
             :optional true}
    "utils" {:path "../tur-utils"}                  ; local dev path
  }

  ;; ----------------------------------------------------------------
  ;; CMake dependencies (CPM-compatible C/C++ packages)
  ;; Resolved via CMake's FetchContent / CPM.cmake under the hood.
  ;; See docs/cmake-cpm-integration-plan.md for the full spec.
  ;; ----------------------------------------------------------------
  :cmake-deps {
    "raylib" {:url     "https://github.com/raysan5/raylib"
              :ref     "5.0"
              :options {:BUILD_SHARED_LIBS "OFF"
                        :BUILD_EXAMPLES   "OFF"}}

    "cjson"  {:url "https://github.com/DaveGamble/cJSON"
              :ref "v1.7.16"}
  }

  ;; ----------------------------------------------------------------
  ;; Build options passed to the Turmeric compiler and C toolchain
  ;; ----------------------------------------------------------------
  :build-opts {
    :c-flags   ["-O3" "-DGEOM_PRECISION=f64"]
    :link-libs ["m"]              ; link libm for math functions
    :no-stdlib false
  }

  ;; ----------------------------------------------------------------
  ;; What this package exports to consumers
  ;; ----------------------------------------------------------------
  :exports {
    "geom/vector" ["vector-2d" "vector-3d" "cross-product"]
    "geom/matrix" ["matrix-2x2" "matrix-3x3" "multiply"]
  })
```

### Minimal example

A library with a single Turmeric dependency:

```scheme
(defpackage my-lib
  :name    "my-lib"
  :version "0.1.0"
  :spices  {"core" {:url "https://github.com/turm/tur-core" :ref "v1.0.0"}})
```

A standalone binary with no spices:

```scheme
(defpackage hello
  :name    "hello"
  :version "0.1.0")
```

---

## Spice Sources

### Git URL (primary)

```scheme
:spices {
  "geom" {:url "https://github.com/alice/tur-geom" :ref "v0.2.1"}
  "geom" {:url "git@github.com:alice/tur-geom.git" :ref "main"}
}
```

- `:ref` accepts a Git tag, branch name, or full commit SHA.
- Recommended: pin to a tag (`v0.2.1`) for reproducible builds.
- Works with GitHub, GitLab, Gitea, and any self-hosted Git server.

### Local path

```scheme
:spices {
  "utils" {:path "../tur-utils"}
}
```

- For monorepo workspaces or active development of a dependency.
- Path is relative to the `build.tur` file.
- Local spices are never written to `tur.lock`.

### Registry (future)

```scheme
:spices {
  "geom" {:registry "spice" :version "^0.2.0"}
}
```

- Planned at `pkg.turmeric-lang.org`.
- Resolver maps version constraints to Git refs on the registry index.

---

## CMake Dependencies (C/C++ Packages)

The `:cmake-deps` block declares C and C++ packages that the Turmeric compiler
will link against. These packages are fetched and built using CMake's
FetchContent mechanism (CPM-compatible).

```scheme
:cmake-deps {
  "raylib" {:url     "https://github.com/raysan5/raylib"
            :ref     "5.0"
            :options {:BUILD_SHARED_LIBS "OFF"}}

  "sqlite3" {:url "https://github.com/sqlite/sqlite"
             :ref "version-3.45.0"}
}
```

The Turmeric build tool (`tur build`) generates a `cmake/SpiceDeps.cmake` file
from `:cmake-deps` and invokes CMake automatically. End users do not write any
CMake by hand unless they want fine-grained control.

For projects that need to control the CMake build themselves, the full CMake
integration spec is documented in
[cmake-cpm-integration-plan.md](cmake-cpm-integration-plan.md).

---

## Lock File (`tur.lock`)

```yaml
# tur.lock -- generated by `tur fetch`. Do not edit by hand.
# Commit this file to version control for reproducible builds.
format-version: 1

spices:
  geom:
    url:         "https://github.com/alice/tur-geom"
    ref:         "v0.2.1"
    resolved:    "a1b2c3d4e5f6..."   # full commit SHA
    sha256:      "abc123..."
    fetched-at:  "2026-05-14T09:00:00Z"
    transitive:
      - math@v1.5.2

  math:
    url:         "https://github.com/bob/tur-math"
    ref:         "v1.5.2"
    resolved:    "d6e7f8a9b0c1..."
    sha256:      "def456..."
    fetched-at:  "2026-05-14T09:00:03Z"

cmake-deps:
  raylib:
    url:     "https://github.com/raysan5/raylib"
    ref:     "5.0"
    resolved: "5.0"
    sha256:  "ghi789..."
```

- Always commit `tur.lock` to version control.
- `tur fetch` creates or updates the lock file.
- `tur build` uses the lock file; it will not silently upgrade versions.
- Local `:path` spices are not recorded in the lock file.

---

## Directory Layout

```
my-project/
  build.tur              -- package manifest (written by the author)
  tur.lock               -- reproducibility lock (committed to VCS)
  src/
    main.tur
    util.tur
  spices/                -- fetched spice sources (gitignored)
    math-v1.5.2/
      build.tur
      src/
    test-v0.3.0/
      build.tur
      src/
  cmake/                 -- generated CMake helpers (gitignored)
    SpiceDeps.cmake      -- generated from :cmake-deps
  build/                 -- build artifacts (gitignored)
```

Add to `.gitignore`:

```
spices/
cmake/SpiceDeps.cmake
build/
```

---

## CLI Commands

```sh
# Initialize a new project
tur init --bin my-app      # executable project
tur init --lib my-lib      # library project

# Add a spice (updates build.tur and tur.lock)
tur add https://github.com/alice/tur-geom
tur add https://github.com/alice/tur-geom --ref v0.2.1
tur add ../local-utils --path

# Add a CMake dependency
tur add-cmake https://github.com/raysan5/raylib --ref 5.0

# Fetch all spices from tur.lock
tur fetch

# Update spices to latest allowed versions
tur fetch --update

# Build the project
tur build
tur build --release

# Run the project binary
tur run
tur run --release

# Test
tur test

# Print generated C to stdout (for debugging)
tur emit-c src/main.tur

# (Future) Publish to the Spice registry
tur publish
```

---

## Integration with the Module System

Spice names map directly to module import paths:

```scheme
;; In my-app/src/main.tur
(import geom/vector :refer [vector-2d cross-product])
(import geom/matrix :as mat)

;; The compiler resolves:
;;   geom/vector  ->  spices/geom-v0.2.1/src/vector.tur
;;   geom/matrix  ->  spices/geom-v0.2.1/src/matrix.tur
```

The module system (M0-M7) operates within a single package; Spice handles the
space between packages.

---

## Semantic Versioning

- Versions follow standard semver: `MAJOR.MINOR.PATCH`
- Tags must be named `vMAJOR.MINOR.PATCH` (e.g., `v1.2.3`)
- Pre-releases: `v0.2.0-alpha.1`
- Future registry constraints: `^0.2.0`, `~1.5`, `1.2.3` (exact)
- Git-URL spices always pin to the exact `:ref`; no range resolution

---

## Security

- All fetched spices are verified against the SHA-256 hash in `tur.lock`.
- Builds fail if the hash does not match; no silent downloads.
- GPG-signed Git tags are supported (verification is opt-in in v1).
- Source-only distribution -- no precompiled binaries in the lock file.

---

## Conflict Resolution

- Two spices requiring incompatible versions of a third spice cause a build
  error with a clear diagnostic.
- The user must either upgrade one spice or pin versions to a compatible range.
- No silent multiple-version shadowing.

---

## Stretch Goal: CMake Plugin for Importing Turmeric Projects

To allow CMake projects to consume Turmeric libraries, `tur publish` (or
`tur emit-cmake`) will generate:

```
tur-geom/
  CMakeLists.txt        -- standard CMake entry point
  TurmericConfig.cmake  -- find_package() support
```

Users of the library can then do:

```cmake
find_package(Turmeric REQUIRED)
CPMAddPackage(
  NAME geom
  GITHUB_REPOSITORY alice/tur-geom
  GIT_TAG v0.2.1
)
target_link_libraries(my_app PRIVATE geom::all)
```

Full specification for this plugin lives in
[cmake-cpm-integration-plan.md](cmake-cpm-integration-plan.md).

---

## Comparison: Before vs. After

| Concern | Old plan | This plan |
|---|---|---|
| Manifest file | `build.tur` or `tur.toml` | `build.tur` only |
| Dependency name | deps | spices |
| Primary source | git URL or registry | git URL (registry future) |
| C dependencies | Phase 2 (later) | `:cmake-deps` block in `build.tur` |
| CMake required? | Optional (Phase 2) | Never required by the author |
| CMake plugin | Optional (Phase 3) | Stretch goal, spec in separate doc |

---

## Implementation Checklist (Phase 1)

### Core

- [ ] Parse `build.tur` `defpackage` form
- [ ] Implement `tur.lock` read/write with SHA-256 verification
- [ ] Implement Git clone and ref resolution (`url` + `ref`)
- [ ] Implement local path linking (`path`)
- [ ] Implement semantic version parsing for tags
- [ ] Implement transitive spice resolution (BFS over dependency graph)
- [ ] Detect and report version conflicts with helpful messages
- [ ] CLI: `tur init`, `tur add`, `tur fetch`, `tur build`, `tur run`, `tur test`

### CMake Dependencies

- [ ] Parse `:cmake-deps` block from `build.tur`
- [ ] Generate `cmake/SpiceDeps.cmake` (FetchContent / CPM calls)
- [ ] Invoke CMake to build C deps before compiling Turmeric source
- [ ] Pass `-I` and `-L` flags from cmake-dep build outputs to `tur` compiler

### Documentation

- [ ] Package authoring guide
- [ ] Spice registry submission guide (when registry launches)
- [ ] C interop guide (`:cmake-deps` deep dive)
- [ ] Troubleshooting dependency conflicts

### Testing

- [ ] Unit tests: version constraint parsing and SHA verification
- [ ] Integration test: multi-spice build with transitive deps
- [ ] End-to-end: `tur init` + `tur add` + `tur build` + `tur test` workflow
- [ ] End-to-end: project with a `:cmake-dep` (e.g., linking raylib)

---

## Open Questions

1. **Lock file format**: YAML shown above vs. a Turmeric s-expression format?
   - Lean: YAML for now (human-readable, easy to diff); can revisit

2. **Workspace support**: Multiple `build.tur` files in one repo?
   - Lean: Yes -- a root `build.tur` with `:members ["pkgs/a" "pkgs/b"]`

3. **Private spices**: SSH URLs, tokens for private repos?
   - Lean: SSH URLs work today; credential helpers via `GIT_SSH_COMMAND`

4. **Yank support**: Can authors retract a published version?
   - Lean: Yes (registry only); yanked versions still buildable if in `tur.lock`

5. **`defpackage` vs. a data literal**: Should `build.tur` use a macro or pure data?
   - Lean: Macro (`defpackage`) so tooling can analyze it without evaluating

---

## References

- Zig package manager: https://ziglang.org/documentation/master/#Package-Management
- Rust Cargo: https://doc.rust-lang.org/cargo/
- CPM.cmake: https://github.com/cpm-cmake/CPM.cmake
- CMake FetchContent: https://cmake.org/cmake/help/latest/module/FetchContent.html
