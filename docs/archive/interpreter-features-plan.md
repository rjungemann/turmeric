# Interpreter Features Plan

> **Branch:** `claude/plan-interpreter-features-WwXiJ`
> **Status:** Active — addresses all known gaps in `src/turi/eval.c` and related
> interpreter infrastructure, plus test workarounds that must be resolved.

---

## Overview

This document catalogues every known missing feature and required workaround fix
in the Turmeric interpreter (`src/turi/eval.c`, `libturi`). Items are grouped by
area and ordered within each group by dependency (blocking items first).

The plan covers four categories:

1. **Phase S4 completeness** — language features the interpreter doesn't yet handle
2. **Eval silent-failure fixes** — cases that silently return `nil` instead of
   working or erroring clearly
3. **Test workarounds** — test files that stub out tests because a feature was
   missing when they were written; the feature now exists and the stubs must be
   replaced
4. **Async / S7 remaining items** — loose ends in the fiber scheduler

---

## 1. Phase S4 Completeness

These are the four items still marked `[ ]` in `docs/self-hosted-interpreter-plan.md`
(Phase S4, lines 289–292) plus concrete details from `eval.c`.

---

### 1.1 Pattern matching — literal and nested patterns

**Status:** Partial. `eval.c:2524–2610` (`EX_MATCH`) handles wildcards, variable
captures, union type-narrowing, ADT constructor matching, and when-guards. What
it does **not** handle:

- **Literal patterns** — matching against a specific integer, boolean, float, or
  string value (e.g., `(match n 0 "zero" 1 "one" _ "other")`). The `MatchPattern`
  struct (`src/compiler/expr.h:389`) has no `lit_value` field; a new field and a
  new arm in the `EX_MATCH` evaluator are needed.
- **Nested constructor patterns** — the existing code only binds the direct fields
  of the outer constructor. It does not recurse into fields that are themselves
  constructor patterns (e.g., `(Some (Pair x y))`).

**What to do:**

1. Add a `lit_value` / `is_literal` path to `MatchPattern` in `expr.h` (or reuse
   the existing `var_sym` + a new `is_literal` bool) and populate it in
   `elab_forms.c` when the pattern form is a literal.
2. In `eval.c` `EX_MATCH`, add a branch that compares `val` against the literal
   before the `ctor` branch.
3. Make the `ctor` field branch recurse: for each `bindings[i]` that is itself a
   `CtorDef` pattern, call a helper `match_pattern(val->fields[i], bindings[i])`
   recursively.
4. Add fixture: `tests/fixtures/match-literal/` covering integer, bool, string
   literals; `tests/fixtures/match-nested/` covering two-level constructor nesting.

---

### 1.2 Typeclass / instance registration in the interpreter

**Status:** `EX_TYPECLASS_DEF` and `EX_INSTANCE_DEF` both hit the same two-line
case in `eval.c:2402–2404` that returns `turi_nil()` and does nothing. The
`EX_DICT` lookup at `eval.c:2936` correctly retrieves a method closure from an
`inst->method_impls` array — but only if the instance was registered. Because
`EX_INSTANCE_DEF` is a no-op, no instance is ever registered, so `EX_DICT` never
finds anything and returns an error.

**What to do:**

1. In the `EX_INSTANCE_DEF` case, iterate over the instance's `method_impls`
   and register each as a closure in the env. The closure should capture
   `instance->method_impls[i]` (a `FnDef *`) and the current lexical frame.
   The naming scheme must match what `EX_DICT` uses when it looks up by
   sanitized method name, or store the closure directly on the
   `TypeClassInstance` struct.
2. In the `EX_TYPECLASS_DEF` case, register the typeclass declaration so that
   any elaborator-level dispatch (`EX_DICT`) that fires during subsequent eval
   calls can resolve the instance.
3. Add interpreter-mode tests (in `tests/turi/`) that call a typeclass method
   through the REPL, e.g., defining `defclass` + `definstance` + calling the
   method.

---

### 1.3 Module loading (`import`) in the interpreter

**Status:** `EX_DEFMODULE` at `eval.c:2407` evaluates the module body in-place,
which works for modules inlined within the same compilation unit. It does **not**
load an external file. When the elaborator produces a `defmodule` node for
`(import foo/bar)`, the module body is always present (the elaborator has already
found and parsed the file). This means `import` is actually likely working for the
common case — but it has not been verified end-to-end through the REPL or
`turi_eval_file`.

**What to do:**

1. Write a smoke test (`tests/turi/eval-import.sh` already exists for the compiled
   path; create a REPL-path counterpart) that:
   a. Creates a temp `.tur` file with a `(defn helper ...)`.
   b. Calls `:reload` on a file that `(import ...)` it and calls `helper`.
   c. Verifies the correct output.
2. If the test fails, trace whether `EX_DEFMODULE` receives the imported module's
   body. If the elab path doesn't populate the module body for interpreter mode,
   add a `turi_eval_file` call inside `EX_DEFMODULE` as a fallback.
3. Mark item `[ ] Module loading` in the plan as `[x]` once the test passes.

---

### 1.4 Macro expansion in the interpreter / REPL

**Status:** Macros are expanded by the elaborator before the interpreter ever
sees the `Expr` tree. For compiled programs this works transparently. In the REPL,
each `:reload` or typed expression is re-elaborated from scratch, so macros defined
in earlier lines are NOT available for subsequent lines unless they are in the same
re-elaborated batch. Specifically:

- Typing `(defmacro foo ...)` in one REPL line and `(foo ...)` in the next will
  fail because the second elaboration doesn't see the first session's macro.

The old `src/runtime/interp.c` was intended as a macro-expansion bridge but is
incomplete (see its `TODO` comments; macro lookup is broken there).

**What to do:**

1. Store macro definitions in `TuriEnv` alongside value bindings when `defmacro`
   is elaborated. Tag them distinctly (e.g., `TURI_MACRO` value tag or a separate
   `env->macros` hash table).
2. Before re-elaborating a new REPL line, inject any REPL-session macros back into
   the elaboration scope so the elaborator sees them.
3. Alternatively, maintain a "macro source accumulation buffer" in `TuriEnv` (all
   `defmacro` source seen so far) and prepend it to each new elaboration batch.
   This is simpler and reuses the existing elaborator without changes to `interp.c`.
4. Add a REPL test that defines a macro in one input and uses it in the next.

---

## 2. Eval Silent-Failure Fixes

These are cases in `eval.c` that either silently return `nil` or make incorrect
simplifications that would confuse callers.

---

### 2.1 `EX_INLINE_C` silently returns nil (non-sandboxed)

**Location:** `eval.c:2748–2756`

The comment says "silently return nil" for inline-C in non-sandboxed mode so that
"stdlib functions with inline-C bodies run through the interpreter even without
native C execution". This sounds harmless but means every stdlib function that uses
inline-C for its implementation (e.g., string ops, vec ops, file I/O, math) returns
`nil` in interpreter mode — silently, with no error. Callers get `nil` and may
crash later with a confusing tag-mismatch error.

**What to do:**

1. Instead of silently returning `nil`, return a descriptive error:
   ```c
   return turi_errorf("eval: inline-C not supported in interpreter mode "
                      "(function uses native implementation)");
   ```
2. For the small set of stdlib functions whose inline-C bodies are **pure
   arithmetic** (e.g., `str-length`, `vec-size`), add native interpreter
   implementations registered via `turi_env_register_native`. Priority list:
   - String: `strlen`, `str-concat`, `str-slice`, `str-index`
   - Vec: `vec-size`, `vec-get`, `vec-push`, `vec-set`
   - Math: `sqrt`, `floor`, `ceil`, `abs` (already available via libm)
3. Mark the unimplemented ones with a clear `TURI_ERR` so debugging is fast.

---

### 2.2 `default:` case silently returns nil

**Location:** `eval.c:3256–3257`

Any `ExprKind` not explicitly handled falls through to `return turi_nil()`. This
makes it impossible to know at runtime when an unimplemented node was reached.

**What to do:**

Replace the `default:` with:
```c
default:
    return turi_errorf("eval: unhandled expression kind %d "
                       "(not yet implemented in interpreter)", (int)e->kind);
```
This immediately surfaces new gaps instead of hiding them as silent `nil` returns.
Run the full test suite after making this change to identify any expression kinds
that are currently reached but not explicitly handled.

---

### 2.3 `EX_WEAK_UPGRADE` doesn't wrap in `some()`

**Location:** `eval.c:3202–3204`

The comment says "Simplified: always succeeds; return some(value) as struct" but
the code just returns the inner value directly — it does not actually build a
`some(v)` struct. Any code that pattern-matches the result of `weak-upgrade` will
see the bare value instead of a `(some value)` constructor and fail to match.

**What to do:**

Wrap the result in a one-field struct named `"some"`:
```c
case EX_WEAK_UPGRADE: {
    TuriValue inner = eval_expr(env, frame, e->as.weak_upgrade_.expr);
    if (turi_is_error(inner) || env->returning || env->throwing) return inner;
    return make_struct_val("some", 1, &inner);
}
```
Also add the `weak-upgrade-none` path: if the value indicates a dropped weak ref
(currently impossible in the interpreter since all refs are valid), return a `none`
struct.

---

### 2.4 `EX_REF_PRED` always returns `true`

**Location:** `eval.c:3196–3197`

Returns `turi_bool(true)` unconditionally. In the interpreter all refs are always
valid so this is defensible — but it should at least null-check the value.

**What to do:**

```c
case EX_REF_PRED: {
    TuriValue v = eval_expr(env, frame, e->as.ref_pred_.expr);
    if (turi_is_error(v) || env->returning || env->throwing) return v;
    return turi_bool(v.tag != TURI_NIL);
}
```

---

### 2.5 `EX_SERIAL_RESET` evaluates body but ignores serial-shift

**Location:** `eval.c:3252–3253`

Serial continuations (Phase 21, used for workflow serialization) are stubbed to
"just evaluate body". Any `EX_SERIAL_SHIFT` node would hit the `default:` nil
path. This is acceptable while Phase 21 is not complete, but needs a clear error
rather than a silent nil (addressed by §2.2 above) and a tracking note.

**What to do:**

Once §2.2 is done, `EX_SERIAL_SHIFT` will produce a clear error instead of a
silent nil. Add a comment in the code confirming this is the expected behavior
until Phase 21 is complete:
```c
case EX_SERIAL_RESET:
    /* Phase 21 not yet in interpreter; evaluate body, ignore serial-shift. */
    return eval_expr(env, frame, e->as.serial_reset_.body);
```

---

## 3. Test Workarounds — catch-unwind stubs

Five test functions use `false` as a no-op stub where the comment says to use
`(catch-unwind ...)` once that feature was available. `catch-unwind` is now
implemented (`tests/fixtures/panic-catch-unwind/` exists). These stubs must be
replaced.

Each function below:
- Currently returns `false` unconditionally (test passes trivially)
- Should call the function that panics, wrap it in `(catch-unwind ...)`, and
  assert that a panic was caught

---

### 3.1 `tests/scscm/msg_test.tur:135–142` — `test-unsupported-type`

```turmeric
;; Before (stub):
(defn test-unsupported-type []
  (with-osc-message [msg "/test"]
    ;; TODO: update to use (catch-unwind ...) to test panic behavior
    true))

;; After:
(defn test-unsupported-type []
  (let [panicked (catch-unwind
                    (fn [] (with-osc-message [msg "/test"]
                              (osc-add! msg (list 1 2 3)))))]
    (assert (panic? panicked) "Adding a list to an OSC message should panic")
    true))
```

---

### 3.2 `tests/scscm/errors_test.tur:168–172` — `test-result-unwrap-error`

```turmeric
;; Before (stub):
(defn test-result-unwrap-error []
  (let [result (make-err :connection-failed)]
    false  ;; TODO: update to use (catch-unwind ...) to test that result-unwrap panics
    true))

;; After:
(defn test-result-unwrap-error []
  (let [result (make-err :connection-failed)]
    (let [panicked (catch-unwind (fn [] (result-unwrap result)))]
      (assert (panic? panicked) "result-unwrap on err should panic")
      true)))
```

---

### 3.3 `tests/tidal/timing_test.tur:108–112` — `test-clock-zero-bpm`

```turmeric
;; Before (stub):
(defn test-clock-zero-bpm []
  ;; TODO: update to use (catch-unwind ...) to test that make-beat-clock 0.0 panics
  false)

;; After:
(defn test-clock-zero-bpm []
  (let [panicked (catch-unwind (fn [] (make-beat-clock 0.0 44100.0)))]
    (assert (panic? panicked) "make-beat-clock 0.0 should panic")
    true))
```

---

### 3.4 `tests/tidal/polyrhythm_test.tur:144–148` — `test-polymeter-zero`

```turmeric
;; Before (stub):
(defn test-polymeter-zero []
  ;; TODO: update to use (catch-unwind ...) to test that polymeter 0.0 panics
  false)

;; After:
(defn test-polymeter-zero []
  (let [panicked (catch-unwind (fn [] (polymeter 0.0 (cycle 1 2))))]
    (assert (panic? panicked) "polymeter 0.0 should panic")
    true))
```

---

### 3.5 `tests/tidal/temporal_test.tur:160–164` — `test-slow-zero`

```turmeric
;; Before (stub):
(defn test-slow-zero []
  ;; TODO: update to use (catch-unwind ...) to test that slow 0 panics
  false)

;; After:
(defn test-slow-zero []
  (let [panicked (catch-unwind (fn [] (slow 0 (cycle 1 2))))]
    (assert (panic? panicked) "slow 0 should panic")
    true))
```

**Note:** The exact catch-unwind API (predicate name, argument order) must be
confirmed against `tests/fixtures/panic-catch-unwind/input.tur` before
implementing these. Adjust accordingly.

---

## 4. Async / S7 Remaining Items

---

### 4.1 Cancellation propagation through `async-race` and `with-timeout`

**Status:** `eval-async-cancel.tur` / `eval-async-cancel.sh` exist (Phase S7.8).
Item `S7.8.2` is explicitly `[ ]` in the plan:
> Cancellation propagates through `async-race` (cancels losers) and
> `with-timeout` (cancels timed-out task)

**What to do:**

1. In `async-race`, when one future resolves, call `turi_task_cancel` on all
   remaining in-flight fibers before returning the winner's value.
2. In `with-timeout`, when the deadline fires and the task fiber is still running,
   call `turi_task_cancel` on it before throwing the timeout error.
3. Update `eval-async-cancel.tur` to include a test case verifying that the losing
   task in a race has its `task-cancelled?` flag set.

---

### 4.2 Async test fixtures not wired into the primary test runner

**Status:** All seven `eval-async-*.sh` scripts exist and are self-contained.
However, they are not included in `tests/run.sh` or the CTest suite.

**What to do:**

1. Add each `tests/turi/eval-async-*.sh` to `tests/run.sh` (or a dedicated
   `tests/run-turi.sh` included from the main runner).
2. Add CTest entries in `CMakeLists.txt` for the turi eval tests, grouped under a
   `turi` label so `ctest -L turi` runs them selectively.
3. Mark the `[ ]` fixture items in the plan as `[x]` once the tests pass.

---

## 5. Optional: Phase S6 — Self-Hosted Eval

**Status:** `[ ]` (optional). The plan's Phase S6 items:
- `src/turi/eval.tur` — tree-walk evaluator in Turmeric
- Bootstrap: C core runs `eval.tur`, which then handles subsequent eval calls
- Performance parity with C evaluator within 2×

This is **not required** for any of the above features and should be deferred until
S4 completeness (§1) and S7 (§4) are fully resolved. Once those are done, S6 is a
good dogfooding exercise.

---

## Priority Order

| # | Item | Effort | Blocks |
|---|------|--------|--------|
| 1 | §2.2 — `default:` returns error instead of nil | Trivial | Reveals all hidden gaps |
| 2 | §3.1–3.5 — Replace catch-unwind stubs | Small | Clean test coverage |
| 3 | §2.1 — `EX_INLINE_C` returns error + native impls | Medium | Stdlib usability in REPL |
| 4 | §1.2 — Typeclass instance registration | Medium | Typeclass methods in REPL |
| 5 | §1.1 — Literal and nested pattern matching | Medium | Full `match` in REPL |
| 6 | §1.4 — Macro persistence across REPL lines | Medium | Interactive macro use |
| 7 | §2.3 — `EX_WEAK_UPGRADE` wraps in `some()` | Small | Weak ref code in REPL |
| 8 | §1.3 — Module loading smoke test + fix | Small | `import` in REPL |
| 9 | §4.1 — Cancellation through `async-race`/`with-timeout` | Medium | Correct async cancel |
| 10 | §4.2 — Wire async tests into test runner | Small | CI coverage |
| 11 | §2.4 — `EX_REF_PRED` null-check | Trivial | Correctness |
| 12 | §5 — Phase S6 self-hosted eval | Large | (optional) |

---

*Last updated: 2026-05-20*
