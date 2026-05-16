# Build & Test UX Improvements Plan

Two independent quality-of-life issues that slow down the inner development loop.

---

## Issue 1 — Test runner produces no output until all tests complete

### Root cause

`tests/run.sh` uses `xargs -P $JOBS` to run fixtures in parallel.  Each
worker (`run_happy_worker` / `run_negative_worker`) writes its outcome to a
temp file under `$RESULTS_DIR` instead of printing to stdout.  The single
reporting loop that calls `echo "PASS …"` / `echo "FAIL …"` only runs *after
every worker has finished*, so the terminal is silent for the entire run.

### Options

#### Option A — Print immediately from each worker (recommended, low effort)

Add an `echo` call inside `run_happy` and `run_negative` at the point each
outcome is determined, *in addition to* the existing `write_result` call.
Single-line `echo` calls are atomic on Linux/macOS for lines under the pipe
buffer limit (~4 KB), so output from concurrent workers does not interleave.

Change the final summary loop to only tally counts and print failure details,
not to re-print PASS lines.

**Pros:** minimal change, works today, no new dependencies.  
**Cons:** PASS/FAIL lines arrive in non-deterministic order (fixture start
order, not fixture name order).  Acceptable for a progress feed.

Implementation sketch for `run_happy`:

```bash
# at each early-return / success path, after write_result:
echo "PASS $name"          # or "FAIL $name — $detail"
```

Then in the summary loop replace the PASS echo with just the tally:

```bash
if [ "$kind" = "PASS" ]; then
    PASS=$((PASS + 1))
    # line already printed by worker
elif [ "$kind" = "FAIL" ]; then
    ...
fi
```

#### Option B — Progress dots with final summary (very low effort)

Each worker prints a single character (`·` for pass, `F` for fail) using
`printf` immediately before `write_result`.  The final loop prints the full
PASS/FAIL listing as before.  This avoids re-ordering concerns but still
shows activity.

```bash
printf '·'    # in run_happy on success
printf 'F'    # in run_happy on failure
```

Add a newline before the summary loop.  No change to the summary loop itself.

**Pros:** zero interleaving issues, keeps final ordered listing.  
**Cons:** you still wait for the full report, just with visible progress.

#### Option C — CTest parallel with `--progress`

CTest 3.17+ has `--progress` which prints `N/M tests done` lines.  Each test
is currently a single big shell script, so parallelism at the CTest level
(`ctest -j8`) would only help if the four test suites (`tur_tests`,
`tur_cli_tests`, etc.) are independent and can overlap, which they are.

Add to the `test` recipe in `Justfile`:

```just
test: build
    ctest -j4 --output-on-failure --progress --test-dir build
```

**Pros:** no changes to shell scripts.  
**Cons:** the four test targets are coarse-grained; progress only shows which
suite completed, not individual fixtures.  Works best combined with Option A
or B inside the scripts.

### Recommendation

Implement **Option A** first (immediate PASS/FAIL echo per fixture), then add
`--progress` to the `ctest` invocation (Option C) so the outer CTest layer
also shows incremental output.  Together these give fine-grained live feedback
with minimal code changes.

---

## Issue 2 — Raylib causes long rebuilds

### Root cause

`examples/snake/CMakeLists.txt` calls `CPMAddPackage(raylib …)` at *configure
time*, unconditionally.  This means:

1. Every `cmake -S . -B build` (including `just reconfigure` / `just rebuild`)
   re-downloads or re-extracts Raylib from the CPM cache and re-runs its full
   CMake configure.
2. Every time the `build/` directory is wiped, all of Raylib's C sources are
   recompiled from scratch — Raylib is a non-trivial library and this takes
   30–60 seconds on most machines.
3. `snake` is already `EXCLUDE_FROM_ALL`, so `just build` does not compile it,
   but configure overhead still occurs.

### Options

#### Option A — Set `CPM_SOURCE_CACHE` (quick win, partial fix)

CPM respects `CPM_SOURCE_CACHE` to store downloaded sources outside the build
directory.  Setting it means `rm -rf build` no longer re-downloads Raylib, but
CMake still re-compiles all Raylib C sources on the next build.

Add to `CMakeLists.txt` or export from the environment / `Justfile`:

```cmake
# CMakeLists.txt, before CPMAddPackage calls
set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/cpm" CACHE PATH "CPM source cache")
```

Or in `Justfile`:

```just
export CPM_SOURCE_CACHE := env_var_or_default("CPM_SOURCE_CACHE", home_directory() + "/.cache/cpm")
```

**Pros:** one-line change, prevents repeated downloads.  
**Cons:** does not eliminate the full rebuild of Raylib's C code after
`just rebuild`.

#### Option B — Prefer a system-installed Raylib via `find_package` (recommended)

Before calling `CPMAddPackage`, try `find_package(raylib QUIET)`.  If a
system Raylib is present (e.g., installed via `brew install raylib`), use it
directly — no download, no compilation.  Fall back to CPM only on CI or when
the library is not installed.

```cmake
find_package(raylib 5.0 QUIET)
if (NOT raylib_FOUND)
    CPMAddPackage(
        NAME raylib
        GITHUB_REPOSITORY raysan5/raylib
        GIT_TAG 5.0
        OPTIONS "BUILD_EXAMPLES OFF" "BUILD_GAMES OFF" "WITH_PIC ON"
    )
endif()
```

Developer setup: `brew install raylib` (macOS) or `apt install libraylib-dev`.

**Pros:** zero compile time for the library in the common developer case;
CPM fallback keeps CI hermetic.  
**Cons:** requires a one-time `brew install`; system version must match the
expected API (Raylib 5.0 is stable).

#### Option C — Move examples behind a CMake option (clean separation)

Wrap the `add_subdirectory(examples)` call in an option so the examples
subtree — including the Raylib CPMAddPackage — is only included when
explicitly requested:

```cmake
# CMakeLists.txt
option(TUR_EXAMPLES "Build example programs (requires Raylib)" OFF)
if (TUR_EXAMPLES)
    add_subdirectory(examples)
endif()
```

Add convenience recipes to `Justfile`:

```just
configure-examples:
    cmake -S . -B build -DTUR_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5

games:
    cmake --build build --target snake
```

**Pros:** `just configure` / `just build` / `just test` have zero Raylib
overhead; games are an explicit opt-in.  
**Cons:** slightly more friction to build examples; developers must reconfigure
with `-DTUR_EXAMPLES=ON` to work on snake.

#### Option D — Replace Raylib with a header-only alternative

[sokol](https://github.com/floooh/sokol) provides `sokol_app.h` +
`sokol_gfx.h` + `sokol_gl.h` — header-only, cross-platform (macOS Metal,
Linux GL, Windows D3D11), with a permissive MIT license.  No separate compile
step: one `.c` file with `#define SOKOL_IMPL` compiles everything in a few
seconds.

The snake example would need its rendering shim rewritten from Raylib calls to
sokol calls, which is a moderate port effort but well within reach for a 2D
game.

**Pros:** no external library compile time at all; no Homebrew dependency; CPM
can just download a few header files.  
**Cons:** non-trivial code change to the snake shim; sokol's API is lower-level
than Raylib (no built-in `DrawRectangle` helpers without `sokol_gl`).

#### Option E — Apply `ccache` to the CMake build (builds faster after first clean)

Set `CMAKE_C_COMPILER_LAUNCHER=ccache` during configure.  Raylib's C sources
will be cached by ccache after the first full compile; subsequent clean
rebuilds hit the ccache and finish in seconds.

```just
configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

Requires `brew install ccache` (already used by `tests/run.sh` for fixture
compilation, so it is likely already present).

**Pros:** completely transparent; no code changes to examples; helps all
targets, not just Raylib.  
**Cons:** first clean build is still slow; ccache miss rate is higher after
changes to `CMakeCache.txt`.

### Recommendation

Apply in order of effort:

1. **Option C** (examples behind a CMake option) — zero-cost baseline; `just
   test` and `just build` are completely unaffected by Raylib.
2. **Option B** (prefer system Raylib via `find_package`) — add to the snake
   `CMakeLists.txt`; document `brew install raylib` in `README.md`.
3. **Option A** (`CPM_SOURCE_CACHE`) — one-line addition, prevents
   re-downloads on CI and in fresh checkouts.
4. **Option E** (`ccache` launcher) — add to `configure` recipe; benefits
   the whole build, not just Raylib.

Option D (replace Raylib with sokol) is worth considering if the snake example
is actively developed and the port effort is acceptable, but is otherwise
low priority given the other mitigations.

---

## Summary

| Issue | Quick win | Best long-term fix |
|---|---|---|
| No incremental test output | Progress dots (Option B) | Immediate PASS/FAIL per fixture (Option A) + `ctest --progress` |
| Raylib slow rebuild | `CPM_SOURCE_CACHE` (Option A) | Examples opt-in flag (Option C) + system `find_package` (Option B) |
