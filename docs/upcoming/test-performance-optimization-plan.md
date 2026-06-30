# Test Performance Optimization Plan (CI / Test Runner Parity)

* **Status**: Partially Implemented (Phase 3 Completed)
* **Author**: Gemini CLI
* **Date**: June 29, 2026

## 1. Problem Statement

The Turmeric test suite currently takes approximately **7 minutes and 10 seconds** to run end-to-end sequentially. This is slow enough to impede local developer iteration speed and inflate CI runner execution costs.

The root causes of this performance envelope fall into three primary categories:

1. **Sequential CTest Execution**: The root `Justfile` runs CTest sequentially. With 68 registered test targets (including massive compiled and interpreted fixture runners), the cumulative runtime is bound to the sequential sum of all targets rather than the single slowest component.
2. **Mac OS X / Bash 3.2 Doctest Incompatibilities & Hangs**:
   - **`mapfile` missing**: `tools/run-doctests.sh` relies on `mapfile`, which is absent from macOS's default Bash 3.2 shell. This causes array extraction to fail, leading to hundreds of false failures.
   - **Background Terminal Control Suspends (`SIGTTOU`)**: The `term/set-raw` doctest invokes terminal state modifications (`tcsetattr`) while executing inside background xargs workers/subshells. In POSIX environments, background process groups modifying terminal attributes are stopped by a kernel `SIGTTOU` signal, resulting in silent, infinite hangs.
   - **Recursive Shadowing in Math Functions**: Wrapper functions in `stdlib/math.tur` (e.g., `defn sqrt` and `defn fabs`) are compiled as `static double sqrt(double x)`. The nested inline-C bodies declare `double sqrt(double); return sqrt(x);`. C name resolution prioritizes the local static function definition over the standard math library, causing infinite recursion, stack overflows, and hangs during execution.
3. **Redundant Process Spawns during Stamp Checks**: Both `tests/run.sh` and `tests/run-turi.sh` query the compiler binary (`$TUR`) modification time via `stat` for *every individual fixture*. Across thousands of files, this incurs 3,700+ redundant, sequential process spawns.

---

## 2. Objectives & Goals

- **Target Cold Runtime**: Under **5 minutes** (down from 7m 10s, a ~30% reduction).
- **Target Warm/Cached Runtime**: Under **1.5 minutes** (down from 3m+).
- **Correctness & Stability**: Resolve all infinite hangs, correct false-positives under macOS's default shell environment, and enable robust, successful caching across all test suites.

---

## 3. Technical Design & Proposed Phases

We propose a structured, 4-phase optimization plan that surgically resolves these issues without compromising compiler correctness or altering external testing semantics.

### Phase 1: Parallelize CTest Execution
Currently, the `Justfile` invokes CTest sequentially:
```justfile
test: build doctest
    timeout 300 ctest --output-on-failure --progress --test-dir build
```
By default, modern CTest supports execution parallelism using all active logical cores when invoked with the `--parallel` or `-j` options without a specific thread limit.

We will modify the `test` recipe in `Justfile` to run CTest in parallel:
```justfile
test: build doctest
    timeout 300 ctest -j --output-on-failure --progress --test-dir build
```
*Technical Rationale*: Under parallel CTest, the overall test execution duration is bounded by the single slowest test runner (`tests/run.sh` cold-recompilation, currently taking ~5.5 minutes) rather than the sequential sum.

### Phase 2: Portability & Hang Fixes for Doctests

To ensure doctests complete successfully and cache their results, we will resolve the three environment-specific blocks:

#### 2.1 POSIX Bash-3 Compatible Array Extraction
We will rewrite `mapfile` usages in `tools/run-doctests.sh` using standard portable `while read` loops:
```bash
expected_arr=()
while IFS= read -r line || [ -n "$line" ]; do
    expected_arr+=("$line")
done < "$expected_file"

names_arr=()
while IFS= read -r line || [ -n "$line" ]; do
    names_arr+=("$line")
done < "$names_file"
```

#### 2.2 Background Job Terminal State Modification Guard
To prevent background processes executing `term/set-raw` and `term/set-cooked` from being suspended by `SIGTTOU`, we will guard `tcsetattr` calls in `stdlib/term.tur` by checking whether the process group is currently in the terminal's foreground using `tcgetpgrp`:
```c
#include <unistd.h>
pid_t fg = tcgetpgrp((int)fd);
if (fg == -1 || fg != getpgrp()) {
    // Process is in the background or stdout is not a terminal; avoid SIGTTOU
    return -1;
}
```

#### 2.3 Math Library Wrapper Infinite Recursion Prevention
To prevent `static` generated functions from recursively calling themselves, we will replace the recursive declarations with standard C compiler math builtins (`__builtin_sqrt`, `__builtin_fabs`, etc.), which are standard in both GCC and Clang and resolve without local shadowing:
```c
(defn sqrt [x : float] : float
  ```c
  return __builtin_sqrt(x);
  ```)

(defn fabs [x : float] : float
  ```c
  return __builtin_fabs(x);
  ```)

(defn floor [x : float] : float
  ```c
  return __builtin_floor(x);
  ```)

(defn ceil [x : float] : float
  ```c
  return __builtin_ceil(x);
  ```)

(defn pow [x : float y : float] : float
  ```c
  return __builtin_pow(x, y);
  ```)
```

### Phase 3: Cached `$TUR` Compiler Binary mtime (Stamp-Cache Optimization)
To eliminate redundant process spawns during fixture stamp-caching, we will cache the modification time of the `$TUR` compiler binary once at the beginning of the runner script, exporting it as an environment variable (`TUR_MTIME`).

*Feasibility Analysis*: 
Because the parallel test harnesses run under `xargs` which spawns new `bash` child processes, child processes inherit any variables exported via `export` in the parent shell. Caching the `$TUR` binary's modification time into a `TUR_MTIME` environment variable ensures all parallel workers access the value with **zero process spawn overhead** and **zero IPC cost**.

We will apply this optimization across all three test runners:

#### 3.1 In `tests/run.sh` (Compiled Fixture Runner):
```bash
# Locate around line 245 inside run.sh (right below _tur_mtime definition)
_tur_mtime() {
    stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"
}

# Cache compiler modification time and export it for xargs workers
export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() {
    local input="$1"
    local dir
    dir="$(dirname "$input")"
    local ec_hash=""
    [ -f "$dir/expected.c" ] && ec_hash="$(_tur_hash_file "$dir/expected.c")"
    echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}"
}
```

#### 3.2 In `tests/run-turi.sh` (Interpreter Fixture Runner):
```bash
# Locate around line 73 inside run-turi.sh
_tur_mtime() { stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"; }

# Cache and export compiler mtime once
export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() { echo "$(_tur_hash_file "$1")-${TUR_MTIME}"; }
```

#### 3.3 In `tools/run-doctests.sh` (Doctest Runner):
```bash
# Locate around line 35 inside run-doctests.sh
_tur_mtime() { stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"; }

# Cache compiler mtime
TUR_MTIME="$(_tur_mtime "$TUR")"

# Refactored stamp_key
stamp_key() { echo "$(_tur_hash_file "$1")-${TUR_MTIME}"; }
```

*Technical Rationale*: Caching this single value avoids over 3,700 sequential `stat` process spawns during parallel execution, shaving several seconds off both cold and warm test starts.

### Phase 4: Verification and Validation
Following implementation, the test suite must be fully executed using `tur run test`. We will verify:
1. End-to-end success of all 68 tests.
2. Complete, clean execution of doctests on macOS.
3. Successful population of stamp caches (`tests/.stamp-cache*`), verifying that subsequent runs execute in under 1.5 minutes.

---

## 4. Rollout Strategy

1. **Document Design Plan**: Commit this plan to `docs/upcoming/test-performance-optimization-plan.md` (Completed).
2. **Implement Phase 1 (CTest Parallelism)**: Update `Justfile` and measure baseline timing.
3. **Implement Phase 2 (Doctest Fixes)**: Update `tools/run-doctests.sh`, `stdlib/term.tur`, and `stdlib/math.tur` sequentially. Verify local doctest runs.
4. **Implement Phase 3 (Stamp Cache)**: Inject cached `TUR_MTIME` logic into test runners and verify stamp validation speeds. (Completed)
5. **Final Validation Gate**: Execute end-to-end local test suite and confirm target performance characteristics.
