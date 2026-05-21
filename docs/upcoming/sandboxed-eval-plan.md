# Sandboxed Eval -- Implementation Plan (SB0--SB4)

> **Status:** Partially implemented. The `sandboxed` flag exists and blocks
> I/O builtins, inline-C, and async. SB1--SB4 close the remaining gaps.
>
> **Prerequisites:** None beyond the current `TuriEnv.sandboxed` field and
> `turi_env_new_sandboxed()` already in `src/turi/env.c`.
>
> **Last updated:** 2026-05-20

---

## Motivation

`turi_env_new_sandboxed()` already exists and is documented in
`docs/guides/eval-api.md`, but the current implementation blocks only a
subset of dangerous operations. An embedder using it to evaluate untrusted
code (REPL widgets, plug-ins, user scripts) is not actually safe:

- Unsafe pointer/memory builtins (`raw-malloc`, `ptr-deref`, `ptr-write`, …)
  are unrestricted in sandboxed mode and can corrupt the host process.
- `(import …)` triggers filesystem reads; a sandboxed script can walk the
  stdlib and any reachable `.tur` files on the host.
- No step-fuel or memory quota exists, so infinite loops and stack exhaustion
  can hang or OOM the host.
- The C API gives embedders no way to allow a specific capability
  (e.g. async-only, no filesystem) without forking the environment
  constructor.

This plan closes those gaps in four phases while keeping `turi_env_new()`
fully unrestricted.

---

## Current State

### What `sandboxed = true` blocks today

| Location | What is blocked |
|----------|----------------|
| `eval_builtin` (`eval.c:788`) | `BS_PRINTLN_*`, `BS_DLOPEN`, `BS_DLSYM`, `BS_DLCLOSE` |
| `EX_INLINE_C` (`eval.c:3065`) | All inline-C blocks |
| `EX_ASYNC` (`eval.c:3074`) | `(async …)` forms |

### What is NOT blocked (gaps)

| Operation | Risk |
|-----------|------|
| `BS_RAW_MALLOC`, `BS_RAW_FREE` | Arbitrary host heap allocation/free |
| `BS_PTR_DEREF`, `BS_PTR_WRITE`, `BS_PTR_ARITH` | Arbitrary pointer reads and writes |
| `BS_RAW_MEMSET`, `BS_RAW_MEMCPY` | Bulk memory corruption |
| `BS_ARRAY_GET_UNCHECKED`, `BS_ARRAY_SET_UNCHECKED` | Out-of-bounds array access |
| `BS_UNSAFE_CAST`, `BS_REINTERPRET`, `BS_TRANSMUTE` | Type-punning, potential misuse |
| `(import module-name)` | Reads `.tur` files from the host filesystem |
| Unbounded recursion beyond the default depth | Stack overflow in the host |
| Unbounded `(loop …)` / tail calls | Host process hangs / OOM |

### Related files

```
src/turi/env.h          -- TuriEnv.sandboxed field; turi_env_new_sandboxed()
src/turi/env.c          -- turi_env_new_sandboxed sets sandboxed = true
src/turi/eval.c         -- is_io_builtin(), EX_INLINE_C and EX_ASYNC guards
src/compiler/elab_module.c -- module/import elaboration (no sandbox check)
docs/guides/eval-api.md -- public API docs including sandboxed calculator example
```

---

## Architecture Overview

```
src/turi/env.h          -- add TuriSandboxCaps bitmask field (SB4)
src/turi/env.c          -- turi_env_new_sandboxed: set caps to TURI_CAP_NONE (SB4)
src/turi/eval.c         -- extend is_io_builtin → is_blocked_builtin (SB1)
                           add step-fuel decrement and check (SB3)
src/compiler/elab_module.c -- reject (import) when sandboxed (SB2)
tests/fixtures/sandbox/ -- SB0 fixtures covering all blocked operations
docs/guides/eval-api.md -- update: document new C API additions (SB3, SB4)
```

---

## Phase SB0 -- Specification and fixtures

**Goal:** Write runnable fixtures that document every guarantee the sandbox
must enforce. All fixtures in this phase are expected to *pass* (return a
`TURI_ERROR`) once SB1--SB3 are complete.

### Fixture layout

```
tests/fixtures/sandbox/
  sb-inline-c.tur           -- (` `` `c int x = 1;` `` `) => error (already passes)
  sb-println.tur            -- (println-int 1) => error (already passes)
  sb-dlopen.tur             -- (dlopen "libc.so") => error (already passes)
  sb-async.tur              -- (async (fn [] :int 1)) => error (already passes)
  sb-raw-malloc.tur         -- (raw-malloc 1024) => error
  sb-raw-free.tur           -- (raw-free ptr) => error
  sb-ptr-deref.tur          -- (ptr-deref ptr) => error
  sb-ptr-write.tur          -- (ptr-write ptr 0) => error
  sb-ptr-arith.tur          -- (ptr-arith ptr 1) => error
  sb-raw-memset.tur         -- (raw-memset ptr 0 16) => error
  sb-raw-memcpy.tur         -- (raw-memcpy dst src 16) => error
  sb-unsafe-cast.tur        -- (unsafe-cast x :int) => error
  sb-reinterpret.tur        -- (reinterpret x :int) => error
  sb-transmute.tur          -- (transmute x :int) => error
  sb-import.tur             -- (import turi/list) => error
  sb-step-limit.tur         -- infinite-loop terminated by step fuel (SB3)
  sb-depth-limit.tur        -- deep mutual recursion terminated (SB3)
```

Each fixture is a standalone `.tur` file evaluated via the C test harness
with a sandboxed environment. The harness asserts the returned `TuriValue`
satisfies `turi_is_error(v)`.

**Exit criterion:** All fixture files exist; the four currently-passing ones
pass; the rest fail with a panic or wrong-value error until SB1--SB3 land.

---

## Phase SB1 -- Block unsafe memory builtins

**Goal:** Extend `is_io_builtin` to cover all builtins that are unsafe in
sandboxed mode.

### Changes to `src/turi/eval.c`

Rename `is_io_builtin` to `is_blocked_builtin` and add the missing shapes:

```c
static bool is_blocked_builtin(bool sandboxed, BuiltinShape shape) {
    if (!sandboxed) return false;
    switch (shape) {
    /* I/O */
    case BS_PRINTLN_INT: case BS_PRINTLN_FLOAT: case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR: case BS_PRINTLN_UINT: case BS_PRINTLN_FLOAT32:
    case BS_DLOPEN: case BS_DLSYM: case BS_DLCLOSE:
    /* Unsafe pointer / memory */
    case BS_RAW_MALLOC: case BS_RAW_FREE:
    case BS_PTR_DEREF: case BS_PTR_WRITE: case BS_PTR_ARITH:
    case BS_RAW_MEMSET: case BS_RAW_MEMCPY:
    case BS_ARRAY_GET_UNCHECKED: case BS_ARRAY_SET_UNCHECKED:
    case BS_UNSAFE_CAST: case BS_REINTERPRET: case BS_TRANSMUTE:
        return true;
    default:
        return false;
    }
}
```

Update the single call site in `eval_builtin`:

```c
if (is_blocked_builtin(env->sandboxed, shape))
    return turi_error("eval: builtin not allowed in sandboxed environment");
```

### Tasks

- [ ] Rename `is_io_builtin` to `is_blocked_builtin` with the new signature.
- [ ] Add all unsafe-memory `BuiltinShape` cases listed above.
- [ ] Update the call site in `eval_builtin`.
- [ ] All `sb-raw-malloc.tur` through `sb-transmute.tur` fixtures pass.
- [ ] `just test` -- no regressions.

**Exit criterion:** Every unsafe-memory builtin returns a `TURI_ERROR` in a
sandboxed environment. Existing non-sandboxed tests are unaffected.

---

## Phase SB2 -- Block `(import ...)` in sandboxed mode

**Goal:** Prevent sandboxed code from reading files via `(import …)`.

### Changes to `src/compiler/elab_module.c`

At the top of `elab_load_module`, check the sandboxed flag before opening
any file:

```c
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span import_span) {
    if (e->sandboxed) {
        diag_emit(DIAG_ERROR, import_span,
                  "import not allowed in sandboxed environment");
        return NULL;
    }
    /* ... existing logic ... */
}
```

`Elab` already holds a pointer to `TuriEnv`; the `sandboxed` flag is
readable from there (or pass it as a separate bool field on `Elab`).

### Edge case: stdlib preloaded before sandboxing

If an embedder wants to allow sandboxed code to call stdlib functions, the
recommended pattern is to evaluate the stdlib in an unrestricted environment
and share bindings via `turi_env_set` -- not to allow `(import …)` inside
the sandbox. Document this in `eval-api.md`.

### Tasks

- [ ] Add sandboxed check in `elab_load_module` (or wherever `import` is
  dispatched in the elaborator pipeline).
- [ ] `sb-import.tur` fixture passes.
- [ ] `just test` -- no regressions.

**Exit criterion:** `(import anything)` inside a sandboxed environment
produces a `TURI_ERROR`; unrestricted environments are unaffected.

---

## Phase SB3 -- Resource limits

**Goal:** Let embedders bound CPU usage and stack depth so that infinite
loops and deep recursion cannot hang or crash the host process.

### New fields on `TuriEnv` (`src/turi/env.h`)

```c
/* SB3: resource limits (0 = unlimited) */
uint64_t    step_fuel;          /* decremented each eval step; error at 0 */
uint64_t    step_fuel_limit;    /* initial fuel; set by turi_env_set_fuel */
```

The existing `max_eval_depth` field already provides recursion limiting;
`turi_env_new_sandboxed` should set it to a tighter default (e.g. 256
instead of 4096).

### Step-fuel counter

In the main eval dispatch loop in `eval.c`, add a single decrement before
each expression evaluation:

```c
if (env->step_fuel_limit > 0) {
    if (env->step_fuel == 0)
        return turi_error("eval: step fuel exhausted");
    env->step_fuel--;
}
```

The check is skipped entirely when `step_fuel_limit == 0` (default for
unrestricted envs), so there is no cost to non-sandboxed code.

### New C API functions (`src/turi/eval.h`)

```c
/* Set the step-fuel limit for env.  Each eval step consumes one unit.
 * When fuel reaches 0, turi_eval returns TURI_ERROR.
 * Pass 0 to disable (default for unrestricted environments).
 * turi_env_new_sandboxed sets a default limit of TURI_DEFAULT_SANDBOX_FUEL. */
void turi_env_set_fuel(TuriEnv *env, uint64_t steps);

/* Override the maximum recursion depth (default: 4096 unrestricted,
 * TURI_DEFAULT_SANDBOX_DEPTH for sandboxed envs). */
void turi_env_set_max_depth(TuriEnv *env, uint32_t depth);
```

### Default limits for `turi_env_new_sandboxed`

| Limit | Unrestricted default | Sandboxed default | Constant |
|-------|---------------------|-------------------|----------|
| Step fuel | 0 (off) | 10,000,000 | `TURI_DEFAULT_SANDBOX_FUEL` |
| Max recursion depth | 4096 | 256 | `TURI_DEFAULT_SANDBOX_DEPTH` |

Both constants are `#define`d in `eval.h` so embedders can override them
at compile time.

### Tasks

- [ ] Add `step_fuel` and `step_fuel_limit` fields to `TuriEnv`.
- [ ] Add fuel decrement in the eval dispatch loop.
- [ ] `turi_env_new_sandboxed` sets fuel to `TURI_DEFAULT_SANDBOX_FUEL` and
  depth to `TURI_DEFAULT_SANDBOX_DEPTH`.
- [ ] Add `turi_env_set_fuel` and `turi_env_set_max_depth` to `eval.h` /
  `eval.c`.
- [ ] `sb-step-limit.tur` and `sb-depth-limit.tur` fixtures pass.
- [ ] `just test` -- no regressions; fuel check path is not hit by any
  existing test (they all run in unrestricted envs).
- [ ] Update `docs/guides/eval-api.md` with the new functions and constants.

**Exit criterion:** An infinite-loop script terminates with `TURI_ERROR` in
a sandboxed env; a deeply-recursive script terminates at 256 frames.

---

## Phase SB4 -- Capability allow-list C API

**Goal:** Let embedders selectively re-enable individual capabilities from
a sandboxed baseline, rather than only choosing between fully unrestricted
and fully sandboxed.

### Design

A bitmask on `TuriEnv` replaces the boolean `sandboxed` flag as the
source of truth. The flag is kept for backward compatibility but becomes
a derived property.

```c
/* Capability bits (src/turi/env.h) */
typedef uint32_t TuriCaps;

#define TURI_CAP_IO        (1u << 0)  /* println-*, file I/O builtins */
#define TURI_CAP_FFI       (1u << 1)  /* dlopen/dlsym/dlclose */
#define TURI_CAP_INLINE_C  (1u << 2)  /* inline-C expressions */
#define TURI_CAP_ASYNC     (1u << 3)  /* (async ...) forms */
#define TURI_CAP_UNSAFE    (1u << 4)  /* raw-malloc, ptr-deref, ... */
#define TURI_CAP_IMPORT    (1u << 5)  /* (import ...) module loading */
#define TURI_CAP_ALL       (~(TuriCaps)0)
#define TURI_CAP_NONE      ((TuriCaps)0)
```

`TuriEnv` gains:

```c
TuriCaps    caps;   /* replaces bool sandboxed */
```

The existing `sandboxed` field stays as:

```c
#define turi_env_is_sandboxed(env) ((env)->caps != TURI_CAP_ALL)
```

`turi_env_new()` sets `caps = TURI_CAP_ALL`.
`turi_env_new_sandboxed()` sets `caps = TURI_CAP_NONE`.

### New C API (`src/turi/eval.h`)

```c
/* Grant a capability to a sandboxed environment.
 * No-op if the env is unrestricted. */
void turi_env_allow(TuriEnv *env, TuriCaps cap);

/* Revoke a capability from any environment. */
void turi_env_deny(TuriEnv *env, TuriCaps cap);

/* Query whether a capability is currently granted. */
bool turi_env_has_cap(TuriEnv *env, TuriCaps cap);
```

### Migration of existing checks

| Old check | New check |
|-----------|-----------|
| `env->sandboxed && is_io_builtin(shape)` | `!turi_env_has_cap(env, TURI_CAP_IO) && is_io_builtin(shape)` |
| `env->sandboxed && is_unsafe_builtin(shape)` | `!turi_env_has_cap(env, TURI_CAP_UNSAFE) && is_unsafe_builtin(shape)` |
| `env->sandboxed` (inline-C) | `!turi_env_has_cap(env, TURI_CAP_INLINE_C)` |
| `env->sandboxed` (async) | `!turi_env_has_cap(env, TURI_CAP_ASYNC)` |
| `e->sandboxed` (import) | `!turi_env_has_cap(env, TURI_CAP_IMPORT)` |

### Usage example

```c
/* Sandboxed env that can use async but not I/O or FFI. */
TuriEnv *env = turi_env_new_sandboxed();
turi_env_allow(env, TURI_CAP_ASYNC);

turi_eval(env, "(async (fn [] :int 42))");  /* ok */
turi_eval(env, "(println-int 1)");          /* error: no IO cap */
```

### Tasks

- [ ] Add `TuriCaps` typedef and `TURI_CAP_*` constants to `env.h`.
- [ ] Add `caps` field to `TuriEnv`; set to `TURI_CAP_ALL` in `turi_env_new`,
  `TURI_CAP_NONE` in `turi_env_new_sandboxed`.
- [ ] Replace all `env->sandboxed` reads with `turi_env_has_cap` calls.
- [ ] Keep the `bool sandboxed` field as a deprecated alias
  (`#define` or inline read of caps) to avoid breaking existing embedders.
- [ ] Add `turi_env_allow`, `turi_env_deny`, `turi_env_has_cap` to
  `eval.h` / `eval.c`.
- [ ] Add a test for a mixed-caps environment (async allowed, I/O denied).
- [ ] Update `docs/guides/eval-api.md` with the new API and a mixed-caps
  example.
- [ ] `just test` -- all existing tests pass.

**Exit criterion:** Embedders can construct an environment with any subset
of capabilities. The existing `turi_env_new()` / `turi_env_new_sandboxed()`
entry points continue to work unchanged.

---

## Open Questions

1. **Fuel granularity** -- Should fuel be consumed per AST node evaluated,
   per function call, or per loop iteration? Per-node is simplest to
   implement and most precise; per-call is cheaper but lets tight
   non-recursive loops run unbounded. **Proposed decision: per AST node.**
   The overhead is a single branch in the hot path; microbenchmarks can
   verify it is negligible for unrestricted envs (fuel check skipped when
   `step_fuel_limit == 0`).

2. **Checked vs. unchecked array ops in sandbox** -- `BS_ARRAY_GET_UNCHECKED`
   and `BS_ARRAY_SET_UNCHECKED` are blocked by SB1. Should the sandbox
   automatically substitute checked variants, or just error? **Proposed
   decision: error.** The checked variants are separate builtins; the
   sandboxed script should use them explicitly.

3. **Pre-loading stdlib before sandboxing** -- The recommended pattern (SB2)
   is to call `turi_eval_file(unrestricted_env, "stdlib/list.tur")` and then
   copy bindings into a sandboxed env via `turi_env_set`. This is awkward.
   A cleaner API might be `turi_env_clone_bindings(dst, src)`. Deferred to
   a follow-up; document the `turi_env_set` workaround in `eval-api.md` for
   now.

4. **`bool sandboxed` removal** -- The field is retained as a deprecated alias
   in SB4 to avoid breaking any embedder that reads it directly. A future
   cleanup phase can remove it once downstream users have migrated to
   `turi_env_has_cap`.
