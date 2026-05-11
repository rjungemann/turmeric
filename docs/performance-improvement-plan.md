# Turmeric Performance Improvement Plan

> **Status:** Active Investigation  
> **Target:** Test suite execution time reduction  
> **Related:** [turmeric-plan.md](turmeric-plan.md)

---

## Executive Summary

The Turmeric test suite is taking approximately **98 seconds** to run (based on `time make test` measurements). With 147 test fixtures (117 happy-path + 30 error cases), each test performs:

1. `tur emit-c` → generates C code (~50-60ms per test)
2. `tur build` → compiles to executable (~130-140ms per test)
3. Execute and compare output
4. `diff` comparisons against expected files

**Total overhead:** ~180-200ms × 147 tests ≈ **26-29 seconds** just for compilation passes, plus shell overhead, file I/O, and diffing bringing total to ~98 seconds.

This document identifies performance bottlenecks and proposes improvements across the codebase.

---

## Current Performance Profile

### Test Suite Breakdown

| Test Type | Count | Avg Time per Test | Total Time Estimate |
|---|---|---|---|
| Happy-path fixtures | 117 | ~185ms | ~21.7 seconds |
| Error fixtures | 30 | ~140ms | ~4.2 seconds |
| CLI tests | 3 | ~200ms | ~0.6 seconds |
| Shell overhead | - | - | ~5 seconds |
| **Total** | **150** | - | **~31.5 seconds** |

*Note: Actual wall-clock time is ~98 seconds, suggesting significant overhead from sequential execution, file I/O, and system call latency.*

### Per-Operation Timing

| Operation | Time | Notes |
|---|---|---|
| `tur emit-c <simple>` | 49-57ms | Parsing + elaboration + CPS + codegen |
| `tur build <simple>` | 137-169ms | emit-c + cc compilation |
| `cc` compilation only | ~80-90ms | System compiler overhead |
| File I/O (read/write) | ~5-10ms | Per-file operations |
| `diff` comparison | ~1-5ms | Depends on file size |

---

## Identified Bottlenecks

### 1. Redundant Compilation Passes (High Impact)

**Problem:** Each test runs `tur emit-c` AND `tur build`. The `build` command internally calls `emit-c` again, then invokes the C compiler.

**Evidence:**
```bash
# tests/run.sh does:
$TUR emit-c "$input" > "$actual_c" 2> "$out_dir/actual.stderr"
$TUR build "$input" -o "$exe" 2> "$out_dir/actual.stderr"
```

`cmd_build` in `src/main.c` calls `compile_to_c()` internally, so we're compiling to C twice per test.

**Impact:** ~50% of test time is redundant compilation.

**Potential Savings:** ~15-20 seconds

---

### 2. Sequential Test Execution (High Impact)

**Problem:** Tests run sequentially, one at a time. No parallelization.

**Evidence:** `tests/run.sh` uses a simple `for` loop with no parallelism.

**Impact:** On a multi-core machine (typical 8-16 cores), we could run 8-16 tests concurrently.

**Potential Savings:** 8-16× speedup for CPU-bound portions = ~40-60 seconds

---

### 3. C Compiler Invocation Overhead (Medium Impact)

**Problem:** Each `tur build` spawns a new `cc` process via `system()`.

**Evidence:**
```c
// src/main.c:cmd_build()
Buf cmd;
buf_init(&cmd);
buf_printf(&cmd, "%s -O2 -std=c99 -Wall -o %s %s", cc, out_path, tmpl);
int sys_rc = system(cmd.data);
```

**Impact:** Process creation overhead (~1-5ms per invocation) × 147 tests ≈ 1-5 seconds.

**Potential Savings:** ~2-5 seconds

---

### 4. Standard Library Reloading (Medium Impact)

**Problem:** Each compilation reloads and re-elaborates `stdlib/macros.tur` from scratch.

**Evidence:**
```c
// src/main.c:compile_to_c()
const char *stdlib_files[] = {
    "stdlib/macros.tur",
    NULL
};
// ... read, parse, elaborate for every compilation
```

**Impact:** Parsing and elaborating the same stdlib code 147 times.

**Potential Savings:** ~5-10 seconds (estimated 30-70ms per reload × 147)

---

### 5. Arena Allocation Pattern (Medium Impact)

**Problem:** Arena allocator uses small slabs (64KB default) and falls back to `malloc`/`calloc` for symbol table buckets.

**Evidence:**
```c
// src/arena.c
#define DEFAULT_SLAB (64 * 1024)

// src/symbols.c
st->buckets = (SymbolEntry **)calloc(st->nbuckets, sizeof(SymbolEntry *));
// ... calls rehash() which calls calloc() again
```

**Impact:** Memory fragmentation, cache inefficiency, and `calloc` overhead.

**Potential Savings:** ~2-5 seconds

---

### 6. File I/O Overhead (Low-Medium Impact)

**Problem:** Each test reads source files, writes temporary C files, writes executables, reads stdout/stderr, performs diffs.

**Evidence:**
```bash
# Per test in run.sh:
$TUR emit-c "$input" > "$actual_c" 2> "$out_dir/actual.stderr"
$TUR build "$input" -o "$exe" 2> "$out_dir/actual.stderr"
$exe > "$actual_stdout" 2> "$actual_stderr"
diff -u "$dir/expected.stdout" "$actual_stdout"
diff -u "$dir/expected.c" "$actual_c"
```

**Impact:** Multiple file reads/writes per test, temp file creation/deletion.

**Potential Savings:** ~3-8 seconds

---

### 7. Reader and Parser (Low Impact)

**Problem:** The reader (`src/reader.c`) is a hand-written recursive descent parser with character-by-character scanning.

**Evidence:** 1120 lines of reader code with extensive character peeking and advancing.

**Impact:** Likely not a major bottleneck (5-10ms per parse), but could be optimized.

**Potential Savings:** ~1-3 seconds

---

### 8. Elaborator Complexity (Low Impact)

**Problem:** `src/elab.c` is 4995 lines with complex type checking, borrow checking, effect lowering, and CPS transformation.

**Evidence:** Multiple passes over the AST:
- Elaboration
- Effect lowering (Phase 19)
- CPS transformation (Phase 18)
- Borrow checking (Phase 14)

**Impact:** Each pass traverses the entire AST.

**Potential Savings:** ~2-5 seconds (with pass fusion)

---

### 9. Emit/Codegen (Low Impact)

**Problem:** `src/emit.c` (2859 lines) generates C code using `Buf` string building with frequent `buf_printf` calls.

**Evidence:**
```c
// src/emit.c - many buf_printf calls
buf_printf(&ctx->file, "static %s %s", type_str, var_name);
```

**Impact:** String building overhead, but likely small compared to other factors.

**Potential Savings:** ~1-2 seconds

---

## Proposed Improvements

### Phase P0: Quick Wins (1-2 weeks, High ROI)

**Goal:** Reduce test suite time by 40-50% with minimal code changes.

#### P0.1: Cache C Compiler Output (Savings: ~40-60 seconds)

**Approach:** Use a build cache (like `ccache`) or compile all generated C files in a batch.

**Implementation options:**
- (a) **Integrate ccache**: Set `CC="ccache cc"` in environment
- (b) **Batch compilation**: Collect all C files, compile once with `cc -o test-runner all_generated.c`
- (c) **Persistent cache**: Store compiled objects in `build/` directory with content-based hashing

**Recommendation:** (a) is easiest - just document `CC=ccache cc make test`. (c) is most robust.

**Estimated savings:** 40-60 seconds (eliminates redundant cc invocations)

#### P0.2: Parallel Test Execution (Savings: ~40-60 seconds)

**Approach:** Run tests in parallel using `xargs -P` or `make -j`.

**Implementation:**
```bash
# In tests/run.sh, replace for loop with:
find tests/fixtures -maxdepth 1 -type d ! -name errors | xargs -P8 -I{} bash -c 'run_happy {}'
find tests/fixtures/errors -maxdepth 1 -type d | xargs -P8 -I{} bash -c 'run_negative {}'
```

**Caveats:** 
- Output interleaving (solve with `-L1` or per-test files)
- Shared resource contention (disk I/O)
- May need to adjust for CI environments

**Estimated savings:** 40-60 seconds (8× parallelism on 8-core machine)

#### P0.3: Remove Redundant emit-c in build (Savings: ~15-20 seconds)

**Approach:** Modify `tests/run.sh` to only call `tur build` and skip the separate `emit-c` call when codegen snapshot checking isn't needed.

**Implementation:**
```bash
# Option A: Only build, extract C from build process
# Option B: Add flag to tur build to also output C to file
# Option C: Cache emit-c output, reuse for build
```

**Recommendation:** Option B - add `--emit-c-file` flag to `tur build` that writes C to a specified file.

**Estimated savings:** 15-20 seconds (eliminates redundant emit-c pass)

---

### Phase P1: Architecture Improvements (2-4 weeks, Medium ROI)

**Goal:** Reduce per-compilation overhead by 30-50%.

#### P1.1: Precompiled Standard Library (Savings: ~5-10 seconds)

**Approach:** Pre-compile stdlib files to a serialized form (pickle) that can be loaded quickly.

**Implementation:**
1. Add serialization format for elaborated forms
2. Pre-compile `stdlib/*.tur` to `.turc` (Turmeric compiled) files
3. Load `.turc` files instead of re-parsing/re-elaborating

**File changes:**
- `src/serialize.c` (new) - serialization/deserialization
- `src/main.c` - check for `.turc` before `.tur`
- `Makefile` - add rule to build `.turc` files

**Estimated savings:** 5-10 seconds

#### P1.2: Incremental Compilation (Savings: ~5-10 seconds)

**Approach:** Track file dependencies and only recompile what's changed.

**Implementation:**
1. Parse imports/requires from source files
2. Build dependency graph
3. Only recompile files whose dependencies have changed
4. Cache compilation results

**Recommendation:** Defer to Phase P2 - complex to implement correctly.

**Estimated savings:** 5-10 seconds (for full project builds)

#### P1.3: Optimize Arena Allocator (Savings: ~2-5 seconds)

**Problem:** Current arena uses 64KB slabs and `calloc` for symbol table.

**Approach:** 
1. Increase default slab size to 256KB or 1MB
2. Use arena allocation for symbol table buckets
3. Add slab reuse/free list

**Implementation:**
```c
// src/arena.h
#define DEFAULT_SLAB (256 * 1024)  // Was 64KB

// src/symbols.c
// Use arena_alloc instead of calloc for buckets
```

**Estimated savings:** 2-5 seconds

---

### Phase P2: Compiler Pipeline Optimization (4-8 weeks, Medium ROI)

**Goal:** Optimize the core compilation pipeline.

#### P2.1: Fuse Compilation Passes (Savings: ~3-8 seconds)

**Problem:** Current pipeline has separate passes:
1. Parse → Forms
2. Elaborate → Expr
3. Effect lower → Expr
4. CPS transform → Expr
5. Borrow check → bool
6. Emit → C code

**Approach:** Fuse passes where possible to reduce AST traversals.

**Candidates for fusion:**
- Elaborate + Effect lower (effect lowering needs elaborated types)
- CPS + Borrow check (CPS output is simpler to check)

**Implementation:**
- Modify `elab.c` to optionally run effect lowering
- Modify `cps.c` to run borrow check on output

**Risk:** May complicate error reporting and debugging.

**Estimated savings:** 3-8 seconds

#### P2.2: Lazy Standard Library Loading (Savings: ~3-5 seconds)

**Problem:** All stdlib is loaded eagerly, even if not used.

**Approach:** Load stdlib modules on-demand based on `(require ...)` or usage.

**Implementation:**
1. Add module registry with lazy loading
2. Track which modules are actually used
3. Only load and elaborate used modules

**File changes:**
- `src/module.c` (new) - module registry
- `src/main.c` - lazy loading logic
- `src/elab.c` - on-demand module resolution

**Estimated savings:** 3-5 seconds

#### P2.3: Optimize Buf String Building (Savings: ~1-3 seconds)

**Problem:** `Buf` grows with `realloc`, doubling each time.

**Approach:** 
1. Pre-allocate larger initial buffer
2. Use arena allocation for Buf data
3. Add bulk write operations

**Implementation:**
```c
// src/buf.h
Buf *buf_with_capacity(size_t initial_cap);
void buf_write(Buf *b, const char *data, size_t len);  // Bulk write
```

**Estimated savings:** 1-3 seconds

---

### Phase P3: Advanced Optimizations (Future, Lower ROI)

**Goal:** Further optimize hot paths.

#### P3.1: JIT Compilation (Speculative)

**Approach:** Instead of compiling to C then to native, use a JIT compiler.

**Options:**
- LLVM JIT
- libjit
- Custom bytecode interpreter

**Complexity:** Very high. Defer until core language is stable.

**Estimated savings:** 20-40 seconds (but adds significant complexity)

#### P3.2: Persistent Compilation Server (Speculative)

**Approach:** Run a daemon that keeps stdlib and common code in memory.

**Implementation:**
- `turd` daemon process
- IPC for compilation requests
- Shared memory for cached data

**Complexity:** High. Defer.

---

## Implementation Priority Matrix

| Improvement | Savings | Complexity | Priority | Phase |
|---|---|---|---|---|
| Parallel test execution | 40-60s | Low | ⭐⭐⭐⭐⭐ | P0 |
| Cache cc output (ccache) | 40-60s | Low | ⭐⭐⭐⭐⭐ | P0 |
| Remove redundant emit-c | 15-20s | Low | ⭐⭐⭐⭐⭐ | P0 |
| Precompiled stdlib | 5-10s | Medium | ⭐⭐⭐⭐ | P1 |
| Optimize arena allocator | 2-5s | Low | ⭐⭐⭐⭐ | P1 |
| Fuse compilation passes | 3-8s | High | ⭐⭐⭐ | P2 |
| Lazy stdlib loading | 3-5s | Medium | ⭐⭐⭐ | P2 |
| Optimize Buf | 1-3s | Low | ⭐⭐ | P2 |
| Incremental compilation | 5-10s | High | ⭐⭐⭐ | P2 |
| JIT compilation | 20-40s | Very High | ⭐ | P3 |

---

## Detailed Implementation Plans

### P0.1: ccache Integration (Immediate)

**Files to change:** None (configuration only)

**Steps:**
1. Install ccache: `brew install ccache` (macOS) or `apt-get install ccache` (Linux)
2. Run tests with: `CC="ccache cc" make test`
3. Verify ccache is working: `ccache -s`

**Expected result:** First run same speed, subsequent runs faster as cache fills.

**Caveat:** Need to ensure generated C code is deterministic for cache hits.

---

### P0.2: Parallel Test Execution

**Files to change:** `tests/run.sh`, `tests/run-cli.sh`

**Implementation for run.sh:**

```bash
#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'make' first" >&2; exit 2; }

PASS=0
FAIL=0
FAILED=()
OUT_DIR=$(mktemp -d)
trap "rm -rf $OUT_DIR" EXIT

# Collect all test directories
HAPPY_FIXTURES=()
ERROR_FIXTURES=()

for d in tests/fixtures/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
    [ -d "$d" ] && HAPPY_FIXTURES+=("$d")
done

for d in tests/fixtures/errors/*/; do
    d="${d%/}"
    [ -d "$d" ] && ERROR_FIXTURES+=("$d")
done

# Parallel happy tests (max 8 at a time)
run_happy() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local out_subdir="$OUT_DIR/happy/$name"
    mkdir -p "$out_subdir"
    
    local input
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else echo "SKIP $name (no input)" ; return; fi

    local actual_stdout="$out_subdir/actual.stdout"
    local actual_stderr="$out_subdir/actual.stderr"
    local actual_c="$out_subdir/actual.c"

    "$TUR" emit-c "$input" > "$actual_c" 2> "$actual_stderr"
    if [ $? -ne 0 ]; then
        echo "FAIL $name — tur emit-c failed"
        return 1
    fi

    local exe="$out_subdir/exe"
    "$TUR" build "$input" -o "$exe" 2> "$actual_stderr"
    if [ $? -ne 0 ]; then
        echo "FAIL $name — tur build failed"
        return 1
    fi

    "$exe" > "$actual_stdout" 2> "$actual_stderr"
    local rc=$?
    rm -f "$exe"

    # Compare outputs
    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null; then
            echo "FAIL $name — stdout mismatch"
            return 1
        fi
    fi

    if [ -f "$dir/expected.c" ]; then
        if ! diff -u "$dir/expected.c" "$actual_c" > /dev/null; then
            echo "FAIL $name — codegen mismatch"
            return 1
        fi
    fi

    echo "PASS $name"
    return 0
}

export -f run_happy
printf '%s\n' "${HAPPY_FIXTURES[@]}" | xargs -P8 -I{} bash -c 'run_happy "{}"' || true

# Parallel error tests
run_negative() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local out_subdir="$OUT_DIR/errors/$name"
    mkdir -p "$out_subdir"
    
    local input="$dir/input.tur"
    [ -f "$input" ] || { echo "SKIP $name (no input)"; return 0; }

    "$TUR" emit-c "$input" > /dev/null 2> "$out_subdir/actual.stderr"
    local rc=$?
    if [ $rc -eq 0 ]; then
        echo "FAIL $name — expected error, but tur exited 0"
        return 1
    fi

    if [ -f "$dir/expected.diag" ]; then
        local missing=0
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! grep -F -q "$needle" "$out_subdir/actual.stderr"; then
                echo "FAIL $name — expected diagnostic substring not found: $needle"
                missing=1
            fi
        done < "$dir/expected.diag"
        if [ $missing -ne 0 ]; then
            return 1
        fi
    fi

    echo "PASS $name"
    return 0
}

export -f run_negative
printf '%s\n' "${ERROR_FIXTURES[@]}" | xargs -P8 -I{} bash -c 'run_negative "{}"' || true

echo
echo "summary: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
```

**Key changes:**
- Use `xargs -P8` for parallel execution
- Use temp directory for all outputs
- Each test writes to its own subdirectory

**Caveats:**
- May need to reduce parallelism on resource-constrained systems
- Output order is non-deterministic

---

### P0.3: Remove Redundant emit-c

**Files to change:** `src/main.c`, `tests/run.sh`

**Approach A: Add --emit-c-file flag to tur build**

```c
// src/main.c
static int cmd_build(const char *input, const char *out_path, const char *emit_c_path) {
    // ... existing code ...
    if (emit_c_path) {
        // Write C to emit_c_path instead of/Additionally to temp file
        FILE *f = fopen(emit_c_path, "w");
        if (f) {
            fwrite(csrc.data, 1, csrc.len, f);
            fclose(f);
        }
    }
    // ... rest of build ...
}
```

**Approach B: Cache emit-c output in tests/run.sh**

```bash
# In run_happy():
if [ ! -f "$actual_c" ] || [ "$input" -nt "$actual_c" ]; then
    "$TUR" emit-c "$input" > "$actual_c" 2> "$actual_stderr"
fi
if [ $? -ne 0 ]; then ...

# Then use same actual_c for codegen comparison
```

**Recommendation:** Approach B is simpler and doesn't require code changes.

---

### P1.1: Precompiled Standard Library

**Files to change:** `src/serialize.c` (new), `src/serialize.h` (new), `src/main.c`, `Makefile`

**Serialization format:**
```
Magic: "TURC" (4 bytes)
Version: uint32_t
Form count: uint32_t
Forms: []
  Form type: uint8_t
  Form data: varies by type
    Symbol: hash (uint32_t), length (uint32_t), name (string)
    Int: value (int64_t)
    List: item count (uint32_t), items... (recursive)
```

**Implementation:**
```c
// src/serialize.h
typedef struct Serializer Serializer;
typedef struct Deserializer Deserializer;

Serializer *serializer_new(Arena *arena);
void serialize_form(Serializer *s, const Form *f);
void serializer_free(Serializer *s);

Deserializer *deserializer_new(const uint8_t *data, size_t len, Arena *arena, SymbolTable *st);
Form *deserialize_form(Deserializer *d);
void deserializer_free(Deserializer *d);
```

**Build integration:**
```c
// src/main.c:compile_to_c()
// Check for .turc file
char turc_path[1024];
snprintf(turc_path, sizeof(turc_path), "%s.turc", path);
if (file_exists(turc_path)) {
    // Load from .turc
    forms = deserialize_forms(turc_path, &arena, &st, &nforms);
} else {
    // Parse from .tur
    forms = read_all(&arena, &st, &file, &nforms);
    // Optionally serialize to .turc for future use
    if (getenv("TUR_CACHE")) {
        serialize_forms(turc_path, forms, nforms);
    }
}
```

**Estimated effort:** 2-3 weeks

---

### P1.3: Optimize Arena Allocator

**Files to change:** `src/arena.c`, `src/arena.h`, `src/symbols.c`

**Changes:**

```c
// src/arena.h
#define DEFAULT_SLAB (256 * 1024)  // Increase from 64KB to 256KB

// src/arena.c
// Add free list for slabs
static ArenaSlab *slab_free_list = NULL;

void arena_free(Arena *a) {
    ArenaSlab *s = a->head;
    while (s) {
        ArenaSlab *next = s->next;
        s->next = slab_free_list;
        slab_free_list = s;
        s = next;
    }
    a->head = NULL;
    a->total_bytes = 0;
    a->total_allocs = 0;
}

void *arena_alloc(Arena *a, size_t size) {
    // Use free list if available
    if (slab_free_list) {
        ArenaSlab *s = slab_free_list;
        slab_free_list = s->next;
        s->next = a->head;
        s->used = 0;
        a->head = s;
    }
    // ... rest of allocation logic
}
```

```c
// src/symbols.c
// Use arena allocation for buckets
void symtab_init(SymbolTable *st, Arena *arena) {
    st->arena = arena;
    st->nbuckets = INITIAL_BUCKETS;
    st->buckets = (SymbolEntry **)arena_alloc(arena, st->nbuckets * sizeof(SymbolEntry *));
    memset(st->buckets, 0, st->nbuckets * sizeof(SymbolEntry *));
    st->count = 0;
}

static void rehash(SymbolTable *st) {
    size_t new_n = st->nbuckets * 2;
    SymbolEntry **nb = (SymbolEntry **)arena_alloc(st->arena, new_n * sizeof(SymbolEntry *));
    memset(nb, 0, new_n * sizeof(SymbolEntry *));
    // ... rest of rehash
}
```

**Estimated effort:** 1 week

---

## Benchmarking Methodology

To measure improvements, use:

```bash
# Baseline measurement
make clean && make debug
time make test > /tmp/test-baseline.log 2>&1

# After changes
time make test > /tmp/test-after.log 2>&1

# Compare
python3 -c "
import re
with open('/tmp/test-baseline.log') as f:
    baseline = float(re.search(r'real\s+(\d+m)?(\d+\.\d+)', f.read()).group(2))
with open('/tmp/test-after.log') as f:
    after = float(re.search(r'real\s+(\d+m)?(\d+\.\d+)', f.read()).group(2))
print(f'Before: {baseline}s, After: {after}s, Improvement: {((baseline-after)/baseline)*100:.1f}%')
"
```

---

## Success Criteria

| Phase | Target Test Time | Savings |
|---|---|---|
| Baseline | ~98s | - |
| P0 Complete | < 40s | > 60% |
| P1 Complete | < 30s | > 70% |
| P2 Complete | < 25s | > 75% |

---

## Quick Start Guide

For immediate improvements (can be done today):

1. **Install ccache:**
   ```bash
   brew install ccache  # macOS
   # or
   sudo apt-get install ccache  # Linux
   ```

2. **Run tests with ccache:**
   ```bash
   CC="ccache cc" make test
   ```

3. **Verify ccache stats:**
   ```bash
   ccache -s
   ```

4. **For parallel execution (macOS/Linux):**
   ```bash
   # Temporary: use GNU parallel if available
   brew install parallel  # macOS
   parallel -j8 < tests/run.sh  # Requires adaptation
   ```

---

## Related Documents

- [turmeric-plan.md](turmeric-plan.md) — Main compiler roadmap
- [test-runner-contract.md](test-runner-contract.md) — Test runner specifications

---

*Last updated: 2026-05-10*
