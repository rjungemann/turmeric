# CMake Migration Plan

**Goal:** Replace the Makefile-based build system with CMake for all building in the project. Decommission and remove the Makefile. Add a Justfile with common development tasks.

---

## Current State

| Component | Current Build System | Status |
|-----------|----------------------|--------|
| Main compiler (tur) | Makefile | Uses `src/*.c` + arch-specific assembly |
| Games (snake) | CMake | Depends on `build/tur` from Makefile |
| Tests | Makefile (`make test`) | Shell scripts in `tests/` |
| Custom targets | Makefile | `debug`, `release`, `tsan`, `clean` |

The root `CMakeLists.txt` currently delegates to the Makefile via `add_custom_target(tur_compiler COMMAND ${CMAKE_MAKE_PROGRAM} debug ...)`. This is a hybrid approach that needs to be unified.

---

## Migration Phases

### Phase 1: Prepare CMake Infrastructure (Prerequisite)

**Objective:** Set up CMake to build the main compiler natively.

**Tasks:**
- [ ] Create `src/CMakeLists.txt` for the compiler sources
  - List all `.c` files in `src/`
  - Handle architecture-specific assembly files (`fiber_ctx_arm64.S`, `fiber_ctx_x64.S`)
  - Set compile flags: `-Wall -Wextra -Werror -Wno-unused-parameter -std=c99 -pedantic`
  - Add build type configurations:
    - `Debug`: `-Og -g -fsanitize=address,undefined -DTUR_DEBUG=1`
    - `Release`: `-O2 -DNDEBUG`
    - `TSan`: `-O1 -g -fsanitize=thread`
- [ ] Update root `CMakeLists.txt`
  - Remove `add_custom_target(tur_compiler ...)` delegation to Makefile
  - Add `add_subdirectory(src)` for the compiler
  - Create proper CMake target `tur` (executable)
  - Export the `tur` target for games to depend on
- [ ] Update `examples/snake/CMakeLists.txt`
  - Change dependency from custom target to the CMake `tur` executable target
  - Use `add_dependencies()` or target-based dependencies
- [ ] Update `cmake/CPM.cmake` if needed for consistency

**Files to create/modify:**
- `src/CMakeLists.txt` (new)
- `CMakeLists.txt` (modify)
- `examples/snake/CMakeLists.txt` (modify)

**Verification:**
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
./tur --version  # or similar
```

---

### Phase 2: Parity Testing

**Objective:** Ensure CMake build produces equivalent output to Makefile.

**Tasks:**
- [ ] Build with both systems:
  ```bash
  make clean && make debug       # Makefile build
  cd build && cmake .. && make   # CMake build
  ```
- [ ] Compare compiler binaries:
  ```bash
  diff <(objdump -d build_make/tur) <(objdump -d build_cmake/tur)
  # Or use: cmp build_make/tur build_cmake/tur
  ```
- [ ] Run test suite with CMake-built compiler:
  ```bash
  # Set TUR_COMPILER or update tests/run.sh to use CMake-built tur
  TUR_COMPILER=$(pwd)/build/tur bash tests/run.sh
  ```
- [ ] Test all build variants:
  - Debug (AddressSanitizer + UBSan)
  - Release
  - TSan (ThreadSanitizer)
- [ ] Test games build with CMake-built compiler

**Acceptance Criteria:**
- All tests pass with CMake-built compiler
- Generated binaries are functionally equivalent
- All architecture-specific builds work (x86_64, arm64)

---

### Phase 3: Test Integration

**Objective:** Migrate test execution to CMake.

**Tasks:**
- [ ] Add CTest support to root `CMakeLists.txt`:
  ```cmake
  enable_testing()
  add_test(NAME tur_tests COMMAND bash tests/run.sh)
  add_test(NAME tur_cli_tests COMMAND bash tests/run-cli.sh)
  add_test(NAME tur_span_tests COMMAND bash tests/check-span-unknown.sh)
  ```
- [ ] Add TSan test variants using CTest's fixture mechanism or custom targets
- [ ] Ensure tests use the CMake-built `tur` binary:
  - Update `tests/run.sh` to accept `TUR_BIN` environment variable
  - Or pass the built compiler path directly in CTest commands
- [ ] Add test dependency: tests should depend on the `tur` target

**Verification:**
```bash
cmake --build . --target test
ctest --output-on-failure
```

---

### Phase 4: Add Justfile

**Objective:** Create a user-friendly Justfile with common development tasks.

**Proposed Justfile contents:**
```just
# Build targets
build: debug

debug:
    cmake --build build -j --config Debug

release:
    cmake --build build -j --config Release

tsan:
    cmake --build build -j --config TSan

# Configuration
configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

reconfigure:
    rm -rf build
    just configure

# Test targets
test: build
    ctest --output-on-failure --test-dir build

test-tsan: tsan
    TUR_TSAN=1 ctest --output-on-failure --test-dir build

# Cleanup
clean:
    cmake --build build --target clean
    rm -rf build

clean-test:
    rm -rf tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

# Utility
run file:
    ./build/tur run {{file}}

build-file file:
    ./build/tur build {{file}}

emit-c file:
    ./build/tur emit-c {{file}}

# Games
games:
    cmake --build build --target snake

run-snake: games
    ./build/examples/snake/snake

# Full rebuild
rebuild: clean configure build
```

**Tasks:**
- [ ] Create `Justfile` at project root
- [ ] Document Justfile in README.md (update build instructions)
- [ ] Test all Justfile recipes

---

### Phase 5: Decommission Makefile

**Objective:** Remove the Makefile after full CMake migration is verified.

**Tasks:**
- [ ] Confirm all stakeholders are aware of the migration
- [ ] Ensure CI/CD pipelines are updated to use CMake (if applicable)
- [ ] Verify Justfile covers all Makefile functionality:
  - `make` → `just` or `just build`
  - `make debug` → `just debug`
  - `make release` → `just release`
  - `make tsan` → `just tsan`
  - `make test` → `just test`
  - `make test-tsan` → `just test-tsan`
  - `make clean` → `just clean`
- [ ] Delete `Makefile`
- [ ] Remove any Makefile references from documentation
- [ ] Update README.md build instructions to use Just/CMake

**Files to delete:**
- `Makefile`

**Files to update:**
- `README.md` (build instructions)
- Any other documentation mentioning `make`

---

### Phase 6: Final Validation

**Objective:** Complete end-to-end verification of the migration.

**Tasks:**
- [ ] Clone fresh repository
- [ ] Run: `just` (should build and test)
- [ ] Run: `just test`
- [ ] Run: `just release`
- [ ] Run: `just games`
- [ ] Run: `just clean && just`
- [ ] Verify no `make` or `Makefile` references remain in active code

---

## Dependency Graph

```
Makefile (CURRENT)
├── build/tur (compiler binary)
│   ├── src/*.c (compiler sources)
│   └── src/fiber_ctx_*.S (arch-specific assembly)
├── test target
│   └── tests/run.sh (uses build/tur)
└── examples/ (via custom target)
    └── snake/ (uses build/tur)

CMake (TARGET)
├── tur (executable target)
│   ├── src/*.c
│   └── src/fiber_ctx_*.S
├── tests (CTest)
│   └── tur (target dependency)
└── snake (executable target)
    ├── tur (target dependency)
    └── raylib (CPM package)

Justfile (USER INTERFACE)
├── build → cmake --build
├── test → ctest
└── games → cmake --build --target snake
```

---

## Rollback Plan

If issues arise during migration:

1. **Keep Makefile temporarily:** Don't delete until Phase 6 is complete
2. **Hybrid mode:** The current CMakeLists.txt already works with Makefile - can be used as fallback
3. **Git checkout:** `git checkout HEAD -- Makefile` to restore if needed

---

## File Changes Summary

| File | Action | Description |
|------|--------|-------------|
| `src/CMakeLists.txt` | Create | Compiler source configuration |
| `CMakeLists.txt` | Modify | Replace Makefile delegation with native build |
| `examples/snake/CMakeLists.txt` | Modify | Depend on CMake tur target instead of custom target |
| `Justfile` | Create | User-friendly task runner |
| `Makefile` | Delete | After verification |
| `README.md` | Modify | Update build instructions to use Just/CMake |
| `tests/run.sh` | Maybe Modify | Accept TUR_BIN env var if not already supported |

---

## Timeline Estimate

| Phase | Estimated Duration |
|-------|-------------------|
| Phase 1: CMake Infrastructure | 2-4 hours |
| Phase 2: Parity Testing | 2-4 hours |
| Phase 3: Test Integration | 1-2 hours |
| Phase 4: Justfile | 1 hour |
| Phase 5: Decommission Makefile | 0.5 hour |
| Phase 6: Final Validation | 1-2 hours |
| **Total** | **8-15 hours** |

---

## Prerequisites

- CMake 3.20+ (already required by existing CMakeLists.txt)
- C99-compatible compiler
- Just 1.0+ (for Justfile support)

---

## Success Criteria

- [ ] `just` builds the compiler successfully
- [ ] `just test` passes all tests
- [ ] `just games` builds all games
- [ ] All build variants (debug, release, tsan) work
- [ ] No Makefile exists in the repository
- [ ] CI passes with new build system (if applicable)
