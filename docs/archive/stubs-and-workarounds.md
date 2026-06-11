# Stubs, Placeholders, and Test Workarounds

This document catalogs every known placeholder, stub, and test workaround in the
codebase as of 2026-05-20, grouped by area and ordered from highest to lowest
implementation priority.

Checkbox key: `[x]` done, `[~]` partial, `[ ]` not yet started.

---

## 1. Compiler / Codegen (`src/compiler/`)

### 1.1 [x] Nested Function Definitions (`EX_FN`) -- **highest priority**

- `src/compiler/emit_expr.c` ~1146-1150: `EX_FN_DEF` in value position aborts with
  "nested fn not yet supported"; `EX_FN` aborts unconditionally.
- `src/compiler/emit_stmt.c` ~255: `EX_FN` in statement position also aborts.

**Impact:** First-class lambda / anonymous function in compiled output does not work.
Any program that passes an un-named `fn` as a value (rather than a named `defn`) hits
an abort.

**Plan:** Implement closure lifting in `emit_expr.c`. Assign each `EX_FN` a synthetic
name (e.g. `__lambda_<n>`), emit it as a top-level C function with a captured-variable
struct, and return a fat-pointer. Gate behind a new fixture
`tests/fixtures/nested-fn-basic/`.

**Prerequisites:** None external. Sub-tasks in order:

1. **Free-variable analysis** -- traverse the `EX_FN` body to collect the set of
   bindings that are defined outside it. The existing `DeferThunk` capture list in
   `emit_internal.h` is the closest reference pattern.
2. **Closure struct emission** -- for each unique capture set emit
   `struct __env_<n> { T1 x1; T2 x2; ... }` to `ctx->file` (file-scope buffer).
3. **Thunk function emission** -- emit
   `static int64_t __lambda_<n>(void *__env, int64_t arg)` to
   `ctx->pending_handler_fns`, casting `__env` to the struct and evaluating the body
   with captured bindings resolved through the struct fields.
4. **Fat-pointer creation at call site** -- `malloc(sizeof(struct __env_<n>))`, fill
   captured fields, cast to `int64_t` and return.

**Unblocks:** §1.2 (full `EX_TRY_CATCH`), §1.4 (`call/cc` closure representation).

---

### 1.2 [x] Phase S4: Try/Catch Machinery

- `src/compiler/emit_expr.c` ~637-649: only the body of a `try` block is emitted;
  the `catch` arm is never reached. Uncaught panics abort at the C level.

**Impact:** `try`/`catch` is silently broken in compiled mode. `requires.compiled`
tests that rely on catch are not yet possible.

**Plan:** Emit `setjmp`/`longjmp` (or the existing `tur_panic_t` mechanism) around the
try body and jump to the catch block on panic. See `src/runtime/panic.c` for the
existing panic infrastructure. Add compiled-mode fixture
`tests/fixtures/try-catch-compiled/` alongside the existing interpreted version.

**Prerequisites:** §1.1 must be complete. `EX_TRY_CATCH` needs to wrap its body in a
thunk (a `tur_thunk_fn`) that captures the surrounding locals, then pass it to
`tur_catch_unwind`. That thunk is exactly a non-capturing or capturing `EX_FN` closure;
without §1.1 there is no general mechanism to create one inline.

**Progress:** `EX_THROW` now routes through `tur_panic_with()` with the correct type
tag (instead of `abort()`), so thrown values can be caught by existing
`tur_catch_unwind` boundaries. `EX_TRY_CATCH` still emits only the body -- full thunk
wrapping is blocked on §1.1 closure lifting.

---

### 1.3 [x] Phase T: Panic Type Tag

- `src/compiler/emit_expr.c` ~562-563: panic emits type tag 0 (`TY_INT`) for all
  panic values instead of the actual type of the panicked expression.

**Impact:** Catch clauses that dispatch on panic type cannot distinguish types in
compiled mode.

**Plan:** Thread the elaborated type of the panic argument through to the codegen call
and emit the correct `TY_*` constant.

**Done:** `EX_PANIC_WITH` and `EX_THROW` now emit `payload->type.kind` as the type
tag.

---

### 1.4 [x] RESOLVED -- Phase 18: Continuation Capture (`call/cc`)

**Resolved (2026-06-02)** by the whole-program CPS substrate
([`cps-transform-plan.md`](../upcoming/cps-transform-plan.md), CPS0--CPS6) plus
the [`call-cc-completion-plan.md`](../upcoming/call-cc-completion-plan.md)
(CC1--CC6). `(call/cc f)` / `(escape f)` now build a real `EX_CALLCC` node that
captures an **undelimited** continuation against the implicit program-wide
prompt (heap-reified, unbounded depth, no enclosing `reset`). `f` receives a
real `cont<T>` -- not the old identity/`0` stand-in -- and the `(k v)`
application sugar resumes it. Both are enabled by default; the `-Xcallcc` gate
is a deprecated no-op and `TUR-E0700`/`TUR-E0701` are retired. `call/cc` is
one-shot (`^unique` default, `^linear` opt-in); `call/cc*` covers the
multi-shot/cloneable case. The approach taken was the **CPS transform** option
below (selective coloring keeps direct-style hot code trampoline-free).

---

### 1.5 [x] Phase 21: Serial-Shift / Serializable Continuations

**RESOLVED** (compiler codegen shipped, PR #325). `serial-shift` now captures
the delimited context as a marshalable DK chain on the multi-prompt machine;
`save-cont!` / `resume-cont!` round-trip a continuation through bytes (see
`tests/fixtures/workflow-roundtrip`, `serial-context-marshal`,
`serial-primitive-roundtrip`). The opaque-pointer policy (§3.1) is settled:
`STAG_PTR` values are rejected at the `serial-shift` site rather than
serialized as zero bytes, so frames that would capture an opaque handle fail
cleanly.

**Application-layer continuation:** the warm-start image facility built on
these primitives lives in `stdlib/image.tur` + `src/runtime/image.{h,c}` (plan
`docs/upcoming/application-image-dumps-plan.md`, phases AI1/AI2/AI4/AI6/AI7
implemented; AI3 globals, AI5 reload hooks, AI6.1 `tur run --image` still
open). See [image-dumps-guide.md](../guides/image-dumps-guide.md).

Historical (pre-#325) state, for reference:

- `src/compiler/emit_expr.c` ~1138-1140: `EX_SERIAL_SHIFT` emitted `int64_t %s = 0; /* serial-shift placeholder */`.
- `src/turi/eval.c` ~3622-3624: `EX_SERIAL_RESET` evaluated body and discarded the shift.
- `stdlib/workflow.tur:32-59`: `save-cont!` returned NULL; `resume-cont!` returned 0.

**Impact (historical):** Workflow persistence was non-functional until #325.

**Plan:** Implement serial-shift as a special `SERIAL_RESET` delimiter with
stack-snapshot serialization. The serial.c opaque-pointer placeholder
(`src/runtime/serial.c` ~264, ~549) must be resolved first -- opaque pointers
currently serialize as 8 zero bytes and cannot be restored.

**Prerequisites:**

1. **§3.1 [done]** -- opaque pointer serialization policy is settled (they now abort);
   serial frames containing opaque pointers will surface this error cleanly.
2. **§1.4 infrastructure** -- serial-shift is a restricted form of delimited
   continuation (single-shot, serializable). The CPS transform or fiber infrastructure
   chosen for §1.4 determines the representation of the captured continuation here.
3. **`serial.c` frame format** -- extend the wire format to encode local variable
   values and stack-frame linkage, not just opaque struct fields. Pure data (integers,
   structs, rc-values) can be encoded; file handles and raw pointers cannot.
4. **Scope contract** -- document which types may appear in a `serial-reset` body.
   Types that cannot be serialized (opaque pointers, file handles) must be forbidden
   at the type-checker level or produce a runtime abort.

---

### 1.6 [x] `weak-upgrade` Return Type

- `src/compiler/elab_memory.c` ~432-437: `weak-upgrade` returns `rc<T>` instead of
  `option<rc<T>>`.

**Impact:** Upgrade of a dangling weak reference returns an uninitialised `rc<T>`
instead of `none`, so callers cannot detect upgrade failure.

**Plan:** Change the elaborated return type to `option<rc<T>>` and wrap the C-level
result in `some`/`none` depending on the strong-count check. Add a fixture that
upgrades a weak ref after the original rc is dropped.

**Done:** `elab_weak_upgrade` now returns `TY_PTR_VOID` (option representation as `ptr<void>`). Emitted C allocates a `{ bool is_some; int64_t value; }` struct on success, NULL for none. `weak-dangling` fixture updated to use `some?`; new `weak-upgrade-option` fixture added.

---

### 1.7 [x] `transmute` Compile-Time Size Check

- `src/compiler/elab_unsafe.c` ~322-333: size equality between source and target types
  is not verified at compile time; the args array uses a placeholder slot.

**Plan:** Resolve both type sizes in the elaborator and emit a `static_assert` in the
generated C. Low risk but easy to add.

**Done:** `elab_transmute` now parses the target-type form, calls `type_size_bytes` to check source/target sizes at elaboration time, sets the correct result type, and emits a properly-typed cast in `BS_TRANSMUTE`.

---

### 1.8 [x] HKT `[f :kind]` Vector Syntax

- `src/compiler/elab_typeclasses.c` ~314-315: kind-annotated vector form `[f :kind]`
  is not parsed; users must use `'^name'` prefix.

**Plan:** Parse `[f :kind]` in the kind-annotation reader path and lower it to the
same internal form as `'^f'`. Purely additive change.

**Done:** The `[f :kind]` (KIND_ARROW) and `[f :kind2]` (KIND_ARROW2) forms are now accepted in `defclass` type-parameter lists and lowered to the same internal representation as `'^f'` and `'^^f'`.

---

### 1.9 [ ] Reserved Operator `?` (Phase R1)

- `src/compiler/elab_internal.h` ~347: `?` operator reserved but not implemented.

**Plan:** Implement as short-circuit option-unwrap (analogous to Rust `?`). Tracked in
IDEAS.md if a design decision is still pending.

---

### 1.10 [ ] `#lang` Directive

- `src/main.c` ~84: unsupported `#lang` formats print "not yet implemented".
- `src/compiler/reader.c` ~1485: unknown language directives return false.

**Plan:** Either implement the language dispatch table or document the set of supported
`#lang` values and turn the unknown-lang path into a proper error with a suggestion.

---

## 2. Interpreter (`src/turi/`)

### 2.1 [x] Phase 6: Quasiquote

- `src/runtime/interp.c` ~110: `F_QUASIQUOTE`, `F_UNQUOTE`, `F_UNQUOTE_SPLICING` all
  unimplemented in the interpreter.

**Impact:** Quasiquote/unquote only works in compiled mode (via macro expansion
pre-elaboration). The interpreter silently falls through.

**Plan:** Add quasiquote expansion to `interp.c` using the same recursive strategy as
the reader macro expansion pass.

**Done:** `F_QUASIQUOTE` in `interp_eval` now calls the pre-existing
`quasiquote_expand` helper and re-evaluates the result. `F_UNQUOTE` /
`F_UNQUOTE_SPLICING` outside a quasiquote context evaluate their inner form directly.

---

### 2.2 [x] Phase 21: Serial-Reset in Interpreter

- `src/turi/eval.c` ~3622-3624: `EX_SERIAL_RESET` evaluates body and ignores all
  serial-shift invocations inside it.

**Plan:** Blocked on Phase 21 compiler work (§1.5 above). Add an explicit "Phase 21
not implemented" panic so it fails loudly rather than silently.

**Done:** `EX_SERIAL_RESET` now returns a descriptive `turi_errorf` instead of
silently evaluating the body.

---

## 3. Runtime (`src/runtime/`, `src/async/`)

### 3.1 [x] Opaque Pointer Serialization

- `src/runtime/serial.c` ~264, ~549: opaque pointers serialize as 8 zero bytes and
  cannot be deserialized.

**Impact:** Any serialized value that contains an opaque pointer (file handle, C
struct, etc.) is silently corrupted on restore.

**Plan:** Decide on a policy: (a) panic on attempt to serialize an opaque pointer, or
(b) register a per-type serialize/deserialize hook. Option (a) is safer and simpler to
ship first.

**Done:** Both the serialization and deserialization paths for `STAG_PTR` now
`abort()` with a clear error message instead of silently emitting / consuming 8 zero
bytes.

---

### 3.2 [x] Effect Lowering Placeholder

- `src/passes/effect_lower.c` ~110-137: `perform_to_shift()` is unused
  (`__attribute__((unused))`); it returns 0 instead of looking up the handler.

**Impact:** The effect-lowering pass is not wired in. Effects rely on the elaborator's
handler resolution rather than a post-pass rewrite.

**Plan:** Either wire in the pass or remove the dead code. See
`docs/archive/emit-effects-extraction-plan.md` for context.

**Done:** Removed the three dead unused functions (`perform_to_shift`, `handle_to_reset`, `lower_resume`) from `effect_lower.c`. The pass itself remains in place; wiring it in is tracked as future work.

---

### 3.3 [x] Async / Scheduler Stubs

- `src/async/scheduler.c` ~537-551: `tur_scheduler_mt_from_threadpool()` returns the
  current scheduler unchanged; `tur_scheduler_mt_set_for_threadpool()` is a no-op.
- `src/async/scheduler_common.c` ~27-46: weak stub `tur_scheduler_spawn_st` and
  friends satisfy the linker but do nothing.
- `src/turi/fiber.c` ~462: `turi_task_spawn` runs the closure synchronously and wraps
  the result in a Future instead of spawning a real task.

**Impact:** Multi-threaded scheduler integration and the C-API task spawn are silent
no-ops, masking concurrency bugs.

**Plan:** The ST-scheduler stubs are intentional for the compiler binary; document
that clearly. The MT threadpool integration should panic with "not yet integrated"
rather than silently returning the wrong scheduler. The `turi_task_spawn` sync fallback
should at minimum log a warning.

**Done:** `tur_scheduler_mt_from_threadpool` and `_set_for_threadpool` now
`abort()` with an SCH-003 message. `turi_task_spawn` logs a stderr warning about its
synchronous fallback. ST-scheduler weak stubs are intentional and left as-is.

---

### 3.4 [ ] WASM Fiber Context

- `src/async/fiber_ctx.h` ~9-12: WASM builds use a 1-byte placeholder struct for
  `tur_ctx_t`.

**Impact:** Fibers / async cannot context-switch on WASM.

**Plan:** Implement using Asyncify (Emscripten) or document WASM as single-threaded
with cooperative yields only. Tracked in `docs/archive/wasm-threads-plan.md`.

**Prerequisites:**

1. **Approach decision** (must precede any code):
   - **Asyncify** -- pass `-s ASYNCIFY` to Emscripten. The existing
     `fiber_ctx_x64.S` x86-64 assembly does not apply; `tur_ctx_t` in
     `fiber_ctx.h` must be replaced with Asyncify state handles
     (`asyncify_start_rewind` / `asyncify_stop_rewind`). CMake WASM target
     needs updated link flags.
   - **Cooperative yields only** -- remove context-switching from WASM builds;
     all async is single-threaded with explicit `yield` points. `tur_ctx_t` stays
     as the 1-byte placeholder but fibers never actually swap stacks. Simpler but
     rules out preemptive or multi-fiber concurrency on WASM.
2. **WASM build system** -- whichever approach is chosen, the `just wasm` target
   in the Justfile and the Emscripten CMake toolchain file need corresponding
   changes before the fiber code is testable.

---

## 4. Standard Library (`stdlib/`)

### 4.1 [x] Phase N4: Show Instances for Numeric Types

- `stdlib/typeclass.tur` ~156: Show instances for `int8`, `int16`, `int32`, `uint8`,
  `uint16`, `uint32`, `uint64`, `float32` all return placeholder strings like
  `"<int8>"` instead of formatting the actual value.

**Impact:** Printing any of these types produces a useless tag instead of the number.

**Plan:** Each instance should call the appropriate C `sprintf`/inline-C conversion.
Straightforward change; add fixture `tests/fixtures/show-numeric-types/`.

**Done:** All numeric Show instances (including `Show [int]`) now use `malloc` +
`snprintf` inline-C with the correct format specifiers (`%d`, `%u`, `%llu`, `%g`,
etc.).

---

### 4.2 [ ] Arena Infrastructure Deferred in `Show` Instances

- `stdlib/option.tur` ~409, `stdlib/list.tur` ~440, `stdlib/vec.tur` ~457: the
  `Show` instances for `option`, `Cons`, and `Vec` all contain a TODO to use a scratch
  arena for intermediate string allocation once arena infrastructure lands.

**Impact:** `show` on these types currently heap-allocates each fragment; acceptable
but leaves performance on the table for large collections.

**Plan:** Implement scratch-arena allocation (`stdlib/arena.tur` or
`src/runtime/arena.c`), then revisit these three instances.

**Prerequisites:** Arena infrastructure does not yet exist. Before updating the `Show`
instances, the arena API needs to be defined and implemented -- at minimum
`arena-alloc` (bump-allocate from a fixed block) and `arena-reset` (free all
allocations at once). The `Show` changes are then a straightforward substitution of
`malloc` calls with `arena-alloc` calls against a thread-local or caller-supplied
scratch arena.

---

### 4.3 [ ] `result`/`option` Display/Debug/Error Instances

- `stdlib/typeclass.tur` ~528-559: `Display`, `Debug`, and `Error` instances for
  `ptr<void>` (the result representation) return `"ok"` / `"err"` / `"error"` without
  any inner value. `Debug` produces `"ok(...)"` / `"err(...)"` with no actual contents.

**Plan:** These instances require type-erased inner-value access (reflection or a
vtable). Either implement that, or replace `ptr<void>` with a tagged struct that
carries a `show` function pointer.

**Prerequisites:**

1. **§4.1 [done]** -- Show instances for primitives exist, so the vtable has
   implementations to point at.
2. **Approach decision** (drives the scope significantly):
   - **Option A: `show_fn` pointer in result struct** -- change the result
     representation from `ptr<void>` to a struct carrying
     `{ bool is_ok; int64_t value; show_fn_t show; }`. All result constructors
     (`ok`, `err`) must accept and store a `show` function pointer. Callers that
     construct results need access to the inner type's Show instance at that point.
   - **Option B: `TypeKind` dispatch table** -- pass the inner `TypeKind` alongside
     the value and use a switch/table to call the right `show` at print time.
     Simpler but only works for types whose `TypeKind` is statically known.
3. For Option A, changes span `stdlib/result.tur`, `stdlib/typeclass.tur`, every
   call site that constructs an `ok` or `err`, and the elaborator's result-type
   handling -- audit before starting.

---

### 4.4 [x] Signal/DSP Filter Stubs

- *(Extracted to `tur-signal` spice: `spices/signal/src/signal/dsp.tur`)* `stdlib/signal/dsp.tur` ~130-150:
  - `low-pass`: returns `amplitude * signal` instead of a real IIR/FIR filter.
  - `high-pass`: returns the signal unchanged.

**Impact:** Any DSP code using these filters computes incorrect output silently.

**Plan:** Implement stateful IIR filters (biquad or one-pole) using `stdlib/vec.tur`
or a dedicated filter-state struct. The stateless API signature may need to change to
pass state in/out. Tracked in `docs/archive/signal-processing-arrows-plan.md`.

**Done:** Implemented first-order IIR filters using heap-allocated state. Added `__dsp_alloc_state` helper. `low-pass` now applies `y[n] = a*x[n] + (1-a)*y[n-1]`. `high-pass` applies `y[n] = x[n] - low_pass(x)[n]`. State is allocated per filter instance when applied to a signal.

---

### 4.5 [x] Tidal / Live-Coding Placeholders

- `stdlib/tidal/timing.tur` ~194: pattern scheduling callback is a placeholder.
- `stdlib/tidal/live.tur` ~357-361: `live-eval` returns code unchanged instead of
  evaluating it.
- `stdlib/tidal/perf.tur` ~134: `eval-compiled` always yields pattern value 0.0.

**Plan:** `live-eval` depends on sandboxed eval (see
`docs/upcoming/sandboxed-eval-plan.md`). Until then, add an explicit panic so callers
know it is not functional.

**Done:** `tidal/live.tur` `live-eval` now panics with "not yet implemented". `timing.tur` scheduling callback now panics with "not yet implemented (requires synth parameter update integration)". `perf.tur` `eval-compiled` now panics with "not yet implemented (requires compiled pattern representation)".

---

### 4.6 [x] SuperCollider / SCSCM Stubs

- `stdlib/scscm/synth.tur` ~231-233: `synth-set-by-name` panics with "not yet
  implemented" (acceptable -- it already fails loudly).
- `stdlib/scscm/live.tur` ~250-285: `live-eval` returns code unchanged.

**Plan:** `live-eval` should panic rather than silently do nothing. Real implementation
tracked in `docs/archive/scscm-hcsynth-livecoding-plan.md`.

**Done:** `scscm/live.tur` `live-eval` now panics with "not yet implemented".

---

### 4.7 [ ] `with-async` Effect Placeholder

- `stdlib/effects.tur` ~307: `with-async` notes it is a placeholder pending async
  runtime integration.

**Plan:** Wire `with-async` to the scheduler once the MT scheduler integration
(§3.3) is resolved.

---

### 4.8 [ ] `rc.tur` Functor Map Simplified

- `stdlib/rc.tur` ~24-46: `__functor_rc_fmap` contains a note that it assumes the
  mapped value fits in `int64_t`.

**Plan:** Use the type-erased pointer path for non-integer values, or add a compile-
time guard that panics when `fmap` is called over a non-integer `rc`.

---

### 4.9 [x] `backtrack.tur` Fresh Variable

- `stdlib/backtrack.tur` ~213-222: `fresh` passes 0 as the placeholder value for a
  fresh logic variable.

**Impact:** Fresh variables start with value 0, which is indistinguishable from the
integer 0. Any backtracking program that checks the value of an unbound variable gets
a false result.

**Plan:** Use a sentinel outside the normal value range (e.g. a tagged pointer or a
dedicated `Unbound` variant) to represent an uninstantiated logic variable.

**Done:** `fresh` now passes `INT64_MIN` as the unbound sentinel.

---

## 5. Tests

### 5.1 [x] Missing Compiled-Mode Catch Tests

Tests for `try`/`catch` only run in interpreter mode because compiled catch is broken
(§1.2). No `requires.compiled` fixture exists for error recovery.

**Plan:** Add `tests/fixtures/try-catch-compiled/` once §1.2 is complete.

---

### 5.2 [ ] `safe-arena` Fixture

- `tests/fixtures/safe-arena/input.tur:2`: body is `(println "arena test placeholder")`.

**Plan:** Flesh out with real arena allocation/deallocation tests once arena
infrastructure (§4.2) is in place.

---

### 5.3 [x] `backtrack-fresh` Placeholder Value

- `tests/fixtures/backtrack-fresh/input.tur`: exercises `fresh` with the 0 placeholder
  (§4.9). Test output is vacuously correct because the placeholder happens to match
  the expected value.

**Plan:** Update the test to verify that a fresh variable is distinct from 0 once the
sentinel is implemented.

**Done:** Fixture updated to verify `x != 0` using the `INT64_MIN` sentinel; local
`fresh` stub and `expected.stdout` updated accordingly.

---

### 5.4 [ ] `scscm` Synth Test Placeholder

- `tests/scscm/synth_test.tur` ~57: comment notes the assertion depends on scsynth
  feedback not yet wired up.

**Plan:** Skip or mark the assertion as pending until SCSCM feedback is integrated.

---

## 6. Priority Order

| Priority | Item | Effort | Status | Prereqs |
|----------|------|--------|--------|---------|
| P1 | §1.1 Nested functions (`EX_FN`) | High | [x] | none |
| P1 | §1.2 Compiled try/catch | Medium | [x] | §1.1 |
| P1 | §4.1 Show for numeric types | Low | [x] | -- |
| P2 | §1.3 Panic type tag | Low | [x] | -- |
| P2 | §1.6 `weak-upgrade` return type | Low | [x] | none |
| P2 | §3.1 Opaque pointer serialization policy | Low | [x] | -- |
| P2 | §4.9 `backtrack.tur` fresh sentinel | Low | [x] | -- |
| P3 | §2.1 Quasiquote in interpreter | Medium | [x] | -- |
| P3 | §3.2 Effect lowering dead code | Low | [x] | -- |
| P3 | §3.3 Async/scheduler stubs -- make them loud | Low | [x] | -- |
| P3 | §4.4 DSP filter stubs | Medium | [x] | none |
| P3 | §4.5/4.6 Tidal/SCSCM `live-eval` -- panic loudly | Low | [x] | -- |
| P4 | §1.5 Phase 21 serial-shift | Very High | [ ] | §3.1 [x], §1.4, approach decision |
| P4 | §1.4 Phase 18 `call/cc` | Very High | [ ] | §1.1, approach decision |
| P4 | §3.4 WASM fiber context | High | [ ] | approach decision, WASM build |
| P5 | §1.7 Transmute size check | Low | [x] | none |
| P5 | §1.8 HKT `[f :kind]` syntax | Low | [x] | none |
| P5 | §1.9 Reserved `?` operator | Low | [ ] | none |
| P5 | §4.2 Arena-backed Show | Medium | [ ] | arena infrastructure |
| P5 | §4.3 result/option Display vtable | High | [ ] | §4.1 [x], approach decision |
