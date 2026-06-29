# Turmeric Interpreter (turi) Test Parity Plan

Status: DONE -- 100% turi parity reached (`bash tests/run-turi.sh`: 1383 passed, 0 failed, 450 inline-c carve-outs)
Owner: unassigned
Track: developer-experience / test-parity
Parent: [debugger-plan.md](./debugger-plan.md)

## Goal

Bring the tree-walking interpreter (`turi`) to 100% test parity with the native C compiler backend. Currently, `turi` passes 1345 test fixtures but fails 39. This plan documents these failures, categorizes their root causes, and maps out incremental phases to resolve them.

## Resolution (all phases landed)

All 39 (later 40, after the harness picked up `if-narrow-chained` /
`applied-struct-instance-element-discrimination`) failures are resolved; the
`run-turi.sh` suite is fully green. The fixes, by category:

1. **Stdlib inline-C divergence** -- the list/map/vec/slice natives assumed the
   carrier representation; made `native_list_*` walk the interpreter's struct
   chains, bound `map-new` to the `set_wrap` carrier, bridged the HAMT iterator
   runtime for the pure-Turmeric `Eq[Map]` loop, re-tagged by-value aggregate vec
   cells as structs, handled the `(void)x;` no-op inline-C body, and made
   extern-c `free` a no-op (interpreter allocations are arena-owned).
2. **Struct/ADT representation** -- a `defstruct` lowers to a record ADT, so the
   `TuriStruct` now carries its `CtorDef`; field-name lookup (inline-C), `type-of`
   and `cast` recover struct-ness + field names from it.
3. **Typeclass dispatch / monomorphization** -- primitive-aware instance
   re-resolution plus a runtime-value re-dispatch at the call site (narrow:
   primitives always, single-instance struct heads only), and rc<T> auto-deref
   field access + recursive rc drop-glue.
4. **Uniqueness + async** -- preload `unique.tur` (with-unique/consume/replace),
   register reactor handle natives over the `tur_reactor_*` runtime, and move the
   async-cancel test off the removed throw/try/catch surface onto the rejection
   API.

---

## Technical Context: Why the Interpreter and Native Path Diverged

The native backend has recently received massive upgrades: **by-value monomorphization** (structs, ADTs, options, results passed by-value) and extensive migration of the standard library (`stdlib/`) to **inline-C blocks** for performance and platform integration.

Because `turi` is a pure tree-walking interpreter, it does not compile to C. It is affected by three main categories of divergences:
1. **Inline-C stdlib primitives:** Many standard types (`List`, `Map`, `Vec`, `Set`, `Slice`) now have methods implemented via inline-C blocks. While `turi` carves out explicitly marked `inline-c` tests, normal tests importing these standard library modules still fail when they execute those native-backed methods.
2. **Representational differences (By-Value vs. Lisp values):** `turi` represents struct/ADT values as high-level Lisp-like values. Native code uses low-level C memory representations. Introspection features (like `any-box-struct` or `typeof` assertions) surface these differences.
3. **Low-level pointer/carrier operations:** Unique/affine resource checking and raw pointer operations behave differently when evaluated dynamically on Lisp values vs. compiled C structures.

---

## Inventory of Failing Tests (39 total)

Below is the exhaustive list of current `turi` failures grouped by their technical root cause.

### 1. Standard Library / Inline-C Divergence (20 failures)
These tests fail because the standard library methods they depend on (from `list.tur`, `map.tur`, `vec.tur`, `set.tur`, or `slice.tur`) invoke inline-C code blocks that `turi` cannot evaluate.

*   `list-basic`
*   `list-count-phantom-opaque-aggregate-element`
*   `list-homog-byvalue-aggregate-element`
*   `list-length-byvalue-aggregate-element`
*   `map-basic`
*   `map-eq`
*   `map-typed-consumer`
*   `typed/list-basic`
*   `typed/list-concat`
*   `typed/list-macro`
*   `typed/list-macro-float`
*   `typed/map-basic`
*   `typed/map-collision`
*   `typed/map-eq`
*   `typed/slice-basic`
*   `vec-push-byvalue-aggregate-escapes-frame`
*   `vec-push-heap-struct-element-carrier-cast`
*   `constrained-loop-vec-push-byvalue-result-element`
*   `constrained-generic-instance-vec-element-dispatch`
*   `constrained-generic-instance-vec-element-unascribed`

### 2. Introspection & Representation Mismatches (4 failures)
These tests assert runtime metadata, such as type names or reflection layouts, where `turi`'s dynamic object model differs from the native C compilation layout.

*   `any-box-struct` (Output mismatch: Turi prints `adt` instead of `struct` because `defstruct` lowers to an ADT-like layout internally in the interpreter).
*   `ascribe-fat-closure-call` (Output mismatch: Closure coercion/ascription behaves differently under dynamic frame interpretation).
*   `tuplen-struct-param-passing` (Tuple unpacking and scalar layout mismatches when passing arguments to sub-closures).
*   `defopaque-phantom-param` (Opaque type wrapping/unwrapping behaves differently without native carrier erasure).

### 3. Monomorphization & Typeclass Dispatch Divergence (6 failures)
These tests exercise advanced monomorphization edge cases or complex nested container method dispatch, where `turi`'s runtime typeclass dictionary resolution gets out of sync or fails to unify.

*   `constrained-defn-cons-return-monomorphize`
*   `constrained-generic-nested-container-element-dispatch`
*   `constrained-instance-closure-element-dispatch`
*   `conv-byval-adt-rc-drop`
*   `conv-defstruct-pointer-field-lowering`
*   `refined-nonempty` (Dynamic value coercion/bitcasting under dependent type limits produces pointer-mangled integers under `turi` instead of numeric values).

### 4. Memory Ownership, Lifetimes & Unique Types (6 failures)
These tests verify compile-time uniqueness, linear/affine resource consumption, or reference-counted ADT structures. Because `turi`'s dynamic walker does not track C-level allocations, value consumption behaviors diverge.

*   `conv-rc-adt-field-access`
*   `conv-rc-adt-record-field-deref`
*   `unique-consume`
*   `unique-replace`
*   `unique-with-unique`
*   `errors/unique-with-unique-twice` (Interpreter diagnostic mismatch: compile-time uniqueness check fails or reports differently).

### 5. Asynchronous Runtime & OS Interactions (3 failures)
These tests assert cooperative multitasking, thread cancellation, or IO reactors that require the native `libturi` task-scheduler.

*   `reactor-linear`
*   `eval-async-cancel`
*   `ascribe-fat-closure-call` (related to async thunk wrapping)

---

## Incremental Parity Plan

To achieve 100% green test status under `turi`, we will execute the following phased plan.

### Phase 1 -- Standard Library Mocking / Polyfills for Turi
Since the majority of failures stem from standard library methods utilizing inline-C blocks, we will introduce a standard library polyfill layer specifically for the interpreter.
*   **Action:** Add an internal mechanism in the compiler (`src/turi/eval.c` or `elab_toplevel.c`) that detects when we are in interpreter mode, and redirects standard library inline-C method definitions to pure-Turmeric equivalents.
*   **Focus:** Implement pure-Turmeric fallback definitions for `map-new`, `map-get`, `vec-push!`, `list-concat`, and `slice-get`.
*   **Acceptance:** Resolve the 20 standard library-related test failures.

### Phase 2 -- Aligning Struct and ADT Representation
Bridge the representational gap between compiled structs and interpreter-represented structs.
*   **Action:** Refactor `src/turi/eval.c`'s object model so that `defstruct` instances are explicitly represented as `VALUE_STRUCT` with named fields and correct metadata, rather than reusing the generic `VALUE_ADT` representation.
*   **Acceptance:** Fix `any-box-struct`, `tuplen-struct-param-passing`, and `defopaque-phantom-param`.

### Phase 3 -- Enhancing Typeclass Dictionary Unification
Refine the runtime typeclass resolution engine in `turi`.
*   **Action:** Align the runtime dictionary resolution in `src/turi/` with the exact monomorphization rules used during compilation. Ensure that when parametric/higher-kinded types (like `List` and `Vec`) are instantiated, the correct dictionary is resolved and bound.
*   **Acceptance:** Fix `constrained-defn-cons-return-monomorphize`, `refined-nonempty`, and nested container dispatch failures.

### Phase 4 -- Dynamic Uniqueness and Resource Lifetime Emulation
Add shallow ownership emulations to the interpreter to align diagnostic messages and resource drops.
*   **Action:** Integrate a lightweight value-consumption tracker in `turi`'s evaluation environment. When a unique or linear value is consumed, mark its reference state as invalidated to trigger appropriate runtime errors matching compile-time checks.
*   **Acceptance:** Fix `unique-consume`, `unique-replace`, and `unique-with-unique`.

---

## Open Questions & Risks

*   **Is 100% parity always desirable?**
    Yes. A fully green interpreter test suite guarantees that developers can use `tur debug` / `tur run` during interactive loops with complete confidence, knowing the program's behavior matches native output.
*   **Performance cost of polyfills:**
    The fallback polyfills only run under the interpreter (`turi`), keeping the native path optimized.
