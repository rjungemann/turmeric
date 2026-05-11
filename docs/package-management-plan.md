# Package Management System Plan for Turmeric

> **Status:** Speculative — Future consideration  
> **Prerequisite:** Module system (Phase M0-M7 complete)  
> **Target:** v2 or later  
> **Related:** [module-system-plan.md](module-system-plan.md), [turmeric-plan.md](turmeric-plan.md)

---

## Executive Summary

Once the module system is in place, the natural next step is **package management**: enabling users to declare, version, publish, and consume reusable libraries. This document outlines two distinct approaches:

1. **Turmeric Package Manager (TurPM)** — A Zig-like, centralized package system with a registry
2. **CMake Integration** — Leverage CPM as the package manager, using CMake as the build/packaging layer

**Decision criterion:** TurPM is better for user experience and discoverability; CMake integration is better for interop and reduced tool complexity. We lean toward **Option 1 (TurPM) for v2+**, with Option 2 as a future interop layer.

---

## Option 1: Turmeric Package Manager (TurPM) — Zig-like Approach

### Design Philosophy

Inspired by Zig's package manager, TurPM prioritizes:
- **Simplicity**: Minimal, declarative configuration
- **Decentralization**: No mandatory central registry; self-hosted or GitHub-based by default
- **Reproducibility**: Lock files and content hashing for exact reproduction
- **Ergonomics**: `tur add`, `tur fetch`, `tur build` commands

### Package Metadata

Each package defines a `build.tur` file in the root:

```scheme
; build.tur — package metadata and build script
(defmodule build
  :name "geom"
  :version "0.2.1"
  :description "2D/3D geometry library for Turmeric"
  :license "MIT"
  :authors ["Alice Smith <alice@example.com>"]
  :repository "https://github.com/alice/tur-geom"
  :documentation "https://tur-geom.docs.example.com"
  :homepage "https://example.com/geom"

  ; Dependencies with semantic versioning
  :deps {
    "math" {:url "https://github.com/bob/tur-math" :ref "^1.5.0"}
    "test" {:path "../../tur-test" :optional true}  ; Local/optional deps
  }

  ; Build flags
  :build-opts {
    :c-flags ["-O3" "-DGEO_PRECISION=f64"]
    :link-libs ["m"]  ; Link against libm
    :no-stdlib false
  }

  ; Export spec (what this package provides to consumers)
  :exports {
    "geom/vector" ["vector-2d" "vector-3d" "cross-product"]
    "geom/matrix" ["matrix-2x2" "matrix-3x3" "multiply"]
  })
```

### Registry Model

**No mandatory central registry.** Three distribution mechanisms:

#### 1. Git-based (Default)
```scheme
:deps {
  "geom" {:url "https://github.com/alice/tur-geom" :ref "v0.2.1"}
  "geom" {:url "git@github.com:alice/tur-geom.git" :ref "main"}
}
```

- Pin by Git tag, branch, or commit hash
- Semantic versioning convention: tags named `v1.2.3`
- Works with GitHub, GitLab, Gitea, self-hosted repos

#### 2. Path-based (Local Development)
```scheme
:deps {
  "geom" {:path "../tur-geom"}
}
```

- For local workspaces with multiple packages
- Useful during development before publishing

#### 3. Central Registry (Future/Optional)
```scheme
:deps {
  "geom" {:registry "turmeric" :version "^0.2.0"}
}
```

- Planned registry at `pkg.turmeric-lang.org` (future)
- Simple HTTP API mimicking npm/Rust crates
- Community-maintained; decentralized mirrors supported

### Dependency Resolution

#### Semantic Versioning
- Standard semver: `MAJOR.MINOR.PATCH`
- Version constraints: `^0.2.0` (>=0.2.0, <0.3.0), `~1.5` (>=1.5.0, <2.0.0), `1.2.3` (exact)
- Pre-release versions: `0.2.0-alpha.1`

#### Lock File (`tur.lock`)
```yaml
# tur.lock — reproducible builds
format-version: 1

dependencies:
  geom:
    url: "https://github.com/alice/tur-geom"
    ref: "v0.2.1"
    hash: "sha256:abc123..."
    resolved-at: "2026-05-10T14:30:00Z"

  math:
    url: "https://github.com/bob/tur-math"
    ref: "v1.5.2"
    hash: "sha256:def456..."
    resolved-at: "2026-05-10T14:30:05Z"
    transitive-deps:
      - "test:0.1.0"

checksums:
  geom: "sha256:abc123..."
  math: "sha256:def456..."
```

- Lock file is **always** checked in to version control
- `tur fetch` updates lock file only after explicit upgrade
- Hash verification ensures no tampering

#### Conflict Resolution
- **Flat namespace per semantic version**: `geom@0.2.1` and `geom@0.1.0` treated as incompatible
- Build error if two transitive deps require incompatible versions
- User must pin to compatible versions or file issue with library maintainers
- No "vendoring" or multiple versions of same package in one build

### Directory Layout

```
my-project/
├── tur.toml (or build.tur)              # Workspace/package config
├── tur.lock                             # Reproducibility lock file
├── src/
│   ├── main.tur
│   └── util.tur
├── deps/
│   ├── geom-v0.2.1/
│   │   ├── build.tur
│   │   ├── src/
│   │   │   ├── vector.tur
│   │   │   └── matrix.tur
│   │   └── ...
│   └── math-v1.5.2/
│       ├── build.tur
│       └── ...
├── tests/
└── build/
    ├── main.c
    ├── main.h
    ├── geom_vector.c
    ├── geom_matrix.c
    └── ...
```

### CLI Commands

```bash
# Initialize new package
$ tur init --lib my-geom
$ tur init --bin my-app

# Add dependency (updates tur.lock)
$ tur add github.com/alice/tur-geom      # Latest version
$ tur add github.com/alice/tur-geom@^0.2  # Semver constraint
$ tur add ../local-geom --path           # Local dependency

# Fetch all dependencies
$ tur fetch                               # With tur.lock
$ tur fetch --update                      # Update to latest allowed versions

# Build package and dependencies
$ tur build
$ tur build --release                     # Optimized build

# Run executable
$ tur run [ARGS...]
$ tur run --release [ARGS...]

# Test package
$ tur test
$ tur test --package geom                 # Test specific dep

# Publish to registry (future)
$ tur publish
$ tur publish --registry turmeric

# Search registry (future)
$ tur search geom

# Generate C outputs
$ tur emit-c                              # Generate all .c/.h files
$ tur emit-h --package geom               # Generate headers for dep
```

### Integration with Module System

Package imports map to module imports:

```scheme
; In my-app/src/main.tur
(import geom/vector :refer [vector-2d cross-product])
(import geom/matrix :as mat)

; Compiler resolves:
; - geom/vector -> deps/geom-v0.2.1/src/vector.tur
; - geom/matrix -> deps/geom-v0.2.1/src/matrix.tur
```

Module system M0-M7 operates **within** the package; package manager handles **between** packages.

### Publishing Workflow

1. **Tag release**: `git tag v0.2.1`
2. **Verify**: `tur build && tur test`
3. **Publish**: `tur publish` (future: submits to pkg.turmeric-lang.org)
4. **Update registry**: Registry fetches from Git, caches, serves
5. **User discovers**: `tur search geom` or browsing pkg.turmeric-lang.org
6. **User adds**: `tur add geom` pins to latest compatible version in tur.lock

### Security & Trust

- **Hash verification**: All downloaded code checked against tur.lock
- **Signed Git tags** (future): GPG-signed releases for authenticity
- **Registry mirror support** (future): Users can mirror registry locally
- **Audit trail**: Build command shows all dependencies and versions
- **Source transparency**: Always buildable from published source (no precompiled binaries)

### Limitations of Option 1

- Must implement from scratch: CLI, registry, dependency resolution logic
- Ecosystem slower to grow initially (smaller user base)
- More maintenance burden on core team
- Requires bootstrapping with "killer libraries" to drive adoption

---

## Option 2: CMake Integration — CPM-like Approach

### Design Philosophy

Leverage **CMake as the package manager** using CPM-ish (C++ Package Manager) patterns:
- **Reduce tool count**: Users who know CMake can use existing knowledge
- **Instant interop**: Access to C libraries through CMakeLists.txt
- **Proven ecosystem**: CMake has 20+ years of battle-tested packaging
- **No registry needed initially**: Plain Git URLs sufficient

### Build System Architecture

#### User builds with CMake (not `tur` command)

```cmake
# CMakeLists.txt in project root
cmake_minimum_required(VERSION 3.20)
project(my_geom_app)

# Include CPM for dependency fetching
include(cmake/CPM.cmake)

# Declare Turmeric compiler
set(TUR_COMPILER /usr/local/bin/tur)

# Fetch dependencies
CPMAddPackage(
  NAME geom
  GITHUB_REPOSITORY alice/tur-geom
  GIT_TAG v0.2.1
)

CPMAddPackage(
  NAME mymath
  URL https://example.com/tur-math-1.5.2.tar.gz
  URL_HASH SHA256=def456...
)

# Add our Turmeric target
add_tur_library(mygeom
  SOURCES src/main.tur
  DEPENDS geom mymath
  TUR_VERSION 0.1.0
)

add_tur_executable(my_app
  SOURCES src/app.tur
  LIBRARIES mygeom
)
```

### Package Metadata

Each package publishes a `TurmericConfig.cmake` file (CMake package config):

```cmake
# TurmericConfig.cmake in tur-geom package root
set(geom_VERSION 0.2.1)
set(geom_FOUND TRUE)

# Export library targets
add_tur_library(geom::vector
  IMPORTED GLOBAL
  SOURCES "${geom_SOURCE_DIR}/src/vector.tur"
)

add_tur_library(geom::matrix
  IMPORTED GLOBAL
  SOURCES "${geom_SOURCE_DIR}/src/matrix.tur"
  DEPENDS geom::vector
)

# Expose includes for C interop
set(geom_INCLUDE_DIRS "${geom_SOURCE_DIR}/src")
set(geom_LIBRARIES geom::vector geom::matrix)
```

### Directory Layout

```
my-project/
├── CMakeLists.txt                       # Top-level build config
├── cmake/
│   ├── CPM.cmake                        # CPM helper (vendored)
│   ├── FindTurmeric.cmake               # Find tur compiler
│   └── AddTurTarget.cmake               # Custom CMake functions
├── src/
│   ├── main.tur
│   ├── CMakeLists.txt
│   └── ...
├── _deps/
│   ├── geom-src/                        # Fetched by CPM
│   ├── geom-build/                      # Built by CPM
│   ├── math-src/
│   └── math-build/
└── build/
    ├── CMakeCache.txt
    ├── Makefile (or Xcode, Visual Studio)
    └── ...
```

### Package as CMake Module

A Turmeric package is "just" a CMake project:

```
tur-geom/
├── CMakeLists.txt                       # Standard CMake
├── TurmericConfig.cmake                 # Export for find_package()
├── src/
│   ├── vector.tur
│   ├── matrix.tur
│   └── CMakeLists.txt
├── include/
│   └── tur/geom.h                      # Optional: manual C API
├── tests/
│   ├── test_vector.tur
│   └── CMakeLists.txt
├── docs/
└── README.md
```

### Dependency Declaration

Within CMakeLists.txt, declare transitive Turmeric dependencies:

```cmake
# In tur-geom/CMakeLists.txt
CPMAddPackage(
  NAME mymath
  GITHUB_REPOSITORY bob/tur-math
  GIT_TAG v1.5.2
)

add_tur_library(geom::all
  SOURCES src/vector.tur src/matrix.tur
  DEPENDS mymath::lib
  VERSION 0.2.1
)

# Export for downstream consumers
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  TurmericConfigVersion.cmake
  VERSION 0.2.1
  COMPATIBILITY SemVerCompatible
)

install(FILES TurmericConfig.cmake
  DESTINATION lib/cmake/geom)
```

### Build Integration

#### CMake custom commands invoke Turmeric compiler

```cmake
# In AddTurTarget.cmake
function(add_tur_executable name)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES LIBRARIES)
  cmake_parse_arguments(TUR "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Get full list of dependencies
  get_target_property(deps ${TUR_LIBRARIES} INTERFACE_LINK_LIBRARIES)
  
  # Invoke tur compiler
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.c"
            "${CMAKE_CURRENT_BINARY_DIR}/${name}.h"
    COMMAND ${TUR_COMPILER} compile
      --output "${CMAKE_CURRENT_BINARY_DIR}/${name}"
      --deps "${deps}"
      ${TUR_SOURCES}
    DEPENDS ${TUR_SOURCES} ${deps}
    COMMENT "Compiling Turmeric ${name}"
  )

  # Create C executable
  add_executable(${name}
    "${CMAKE_CURRENT_BINARY_DIR}/${name}.c"
  )
  
  target_link_libraries(${name} PRIVATE ${TUR_LIBRARIES})
endfunction()
```

### CLI Minimal — Just `tur compile`

With CMake as build system, the `tur` CLI is **much simpler**:

```bash
# Core command: compile Turmeric to C
$ tur compile --output main src/main.tur
$ tur compile --output geom/vector src/geom/vector.tur

# Optional convenience for development
$ tur fmt src/          # Format source files
$ tur check src/        # Type check only (no compile)
$ tur repl              # Interactive REPL

# That's mostly it! CMake handles fetching, linking, etc.
```

### Advantages of Option 2

1. **Minimal new tooling**: Reuse mature CMake ecosystem
2. **Instant C interop**: Link Turmeric code with C libraries trivially
3. **Familiar to C developers**: CMake is lingua franca in C community
4. **Scalable**: Leverages CPM's proven dependency resolution
5. **No "package manager" to maintain**: CMake community does it
6. **Works on all platforms**: CMake's portability is battle-tested

### Disadvantages of Option 2

1. **CMake is complex**: Steep learning curve for average user
2. **Cognitive overhead**: Users must learn CMake + Turmeric
3. **Verbose**: CMakeLists.txt requires more boilerplate than `tur.toml`
4. **Less discoverable**: Packages aren't listed in registry; must know GitHub URL
5. **Less language-integrated**: CMake doesn't understand Turmeric semantics

---

## Hybrid Approach: TurPM + CMake Interop (Recommended)

**Best of both worlds:**

### Phase 1: Ship TurPM (v2)
- Implement Turmeric-native package manager
- Simple CLI, Git-based distribution
- No mandatory registry initially
- Focus on getting packages published and consumed

### Phase 2: CMake Interop (v2.x or v3)
- Allow TurPM packages to be consumed by CMake projects
- Generate CMake package config files from Turmeric packages
- Allow CMake projects to link Turmeric libraries
- Bi-directional: TurPM projects can optionally be built via CMake

### Phase 3: Central Registry (v3)
- Launch optional pkg.turmeric-lang.org
- Index packages from GitHub + custom sources
- Mirror support for institutional deployments
- Signed releases with GPG keys

### Implementation Sketch

```
tur-geom/ (with TurPM)
├── build.tur                            # Package metadata
├── src/
│   └── vector.tur
├── CMakeLists.txt (generated)           # Auto-generated for CMake interop
└── TurmericConfig.cmake (generated)     # Auto-generated for CMake interop

Command: tur publish --generate-cmake
  → Generates CMakeLists.txt + TurmericConfig.cmake
  → User can now reference this package from CMake projects
```

---

## Comparison Matrix

| Feature | TurPM | CMake+CPM | Hybrid |
|---|---|---|---|
| User learning curve | Low | Medium-High | Low-Medium |
| Discoverability | High (registry) | Low (must know URL) | Medium (both) |
| CLI ergonomics | High | Medium (verbose) | High |
| C interop | Moderate | High (native) | High |
| Maintenance burden | High (on Tur team) | Low (use CMake) | Medium |
| Ecosystem maturity | Building | Mature | Building |
| v1 viability | No | Yes | Partial |
| Community adoption | Medium | High (C devs know CMake) | Medium-High |

---

## Recommendation for Turmeric

**Option 1 (TurPM) for v2+, with Phase 2 CMake interop in v2.x.**

Rationale:
1. **User-friendly**: Turmeric users shouldn't need to learn CMake
2. **Language-integrated**: Package semantics aligned with module system
3. **Future-proof**: Can add CMake support later without breaking changes
4. **Proven model**: Rust (cargo), Go (go get), Zig all use language-native package managers
5. **Discoverability**: Registry enables ecosystem growth

CMake integration can come **after** core TurPM ships, as an optional interop layer for enterprise/large projects.

---

## Open Questions

1. **Version scheme**: Semver or simpler (major.minor)?
   - Lean: Semver; aligns with existing ecosystem

2. **Private/enterprise packages**: How to support private registries?
   - Lean: Support Git SSH URLs + future: GitHub-style GitHub Packages integration

3. **Native dependencies**: How to declare C library dependencies (libssl, etc.)?
   - Lean: Via CMake integration (Phase 2); TurPM Phase 1 focused on Tur code only

4. **Workspace support**: Multiple packages in one repo?
   - Lean: Yes; like Rust workspaces with `tur.toml` at workspace root

5. **Precompilation**: Ship pre-built `.c` / `.h` files for faster CI?
   - Lean: No in v1; always compile from source; can add in v2

6. **License enforcement**: Check SPDX licenses for compatibility?
   - Lean: Informational only; no enforcement in v1

7. **Reproducible builds**: Exactly how to handle build script differences?
   - Lean: Minimal `build.tur` metadata; all actual build in CMakeLists.txt (Phase 2)

8. **Yank (retract) support**: Can authors remove published versions?
   - Lean: Yes, with notification to dependents; affects tur.lock resolution

---

## Migration Path

1. **Phase 1 (v2)**: TurPM core — basic Tur-to-Tur packaging
2. **Phase 2 (v2.x)**: CMake export — allow consuming TurPM packages from CMake
3. **Phase 3 (v3)**: Central registry — pkg.turmeric-lang.org launch
4. **Phase 4+ (v3+)**: Ecosystem — marketplace, documentation, success stories

---

## Implementation Checklist for Phase 1 (TurPM)

High-level tasks to implement TurPM:

### Core Infrastructure
- [ ] Define `build.tur` metadata format and parser
- [ ] Implement `tur.lock` file format and generator
- [ ] Implement semantic version parsing and constraint resolution
- [ ] Implement Git URL parser and clone logic
- [ ] Implement SHA256 hash verification for packages
- [ ] CLI: `tur init`, `tur add`, `tur fetch`, `tur build`, `tur test`, `tur publish`

### Integration
- [ ] Module system: resolve `import` → fetch from deps/
- [ ] Compiler: pass deps/ to header generation
- [ ] Emitter: generate cross-package headers with proper C naming
- [ ] Error messages: helpful guidance for dependency issues

### Documentation
- [ ] Package authoring guide
- [ ] Package publishing guide
- [ ] Registry setup (GitHub or self-hosted)
- [ ] Troubleshooting dependency conflicts

### Testing
- [ ] Unit tests for version constraint resolution
- [ ] Integration tests: multi-package builds
- [ ] End-to-end: publish + consume workflow

---

## References

- Zig package manager: https://ziglang.org/documentation/master/#Package-Management
- Rust Cargo: https://doc.rust-lang.org/cargo/
- Go modules: https://golang.org/ref/mod
- C++ Package Manager (CPM): https://github.com/cpm-cmake/CPM.cmake
- Node npm/yarn package formats
- Python pip/poetry
