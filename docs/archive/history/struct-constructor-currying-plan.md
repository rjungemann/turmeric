---
title: Struct Constructor Currying Plan
category: Planning
description: Let an under-applied struct constructor (and, more generally, any indirectly-called function whose result is a by-value struct/ADT) return a usable value, by boxing by-value aggregate results through the type-erased closure ABI.
---

# Struct Constructor Currying -- Plan

> **Status: DONE.** All goals (CURRY-V0, V1, V2, DOC) landed. The reported
> finding is resolved and archived at
> `docs/archive/struct-return-through-closure-loses-type.md`, with the per-fix
> paper trail at `docs/archive/history/struct-return-through-closure-loses-type.md`.
> Constructor currying (`((Person "Bob") 40)`), struct/ADT-returning lambdas,
> and partial application of struct/ADT-returning functions all work; the full
> suite is green. Parameterized-struct constructor currying remains out of scope
> (the synthesizer declines; full application via make-struct still works).

## Context

`struct-ergonomics-plan` (now archived) landed the auto-bound constructor,
keyword construction, and `with`. It deliberately deferred **one** goal --
constructor currying -- because it is blocked by a pre-existing engine
limitation that is not specific to structs:

> A by-value struct result cannot flow through Turmeric's type-erased closure
> ABI. Every closure / partial application / first-class function value is
> invoked through a uniform `int64_t (*)(void*, int64_t...)` pointer, so the
> result is forced through the `int64_t` carrier. For a struct result this
> drops the `StructDef` (so the value's type is lost at the call site) and, for
> partial application, produces a C type error (the thunk is declared
> `int64_t` while its body returns the struct).

Full details, repro, and root cause are in the open finding
[`docs/reported/struct-return-through-closure-loses-type.md`](../reported/struct-return-through-closure-loses-type.md).
This plan tracks fixing that limitation so constructor currying -- and
struct-returning lambdas/partial-applications generally -- works.

This is intentionally scoped as its own plan because it is a codegen/ABI change
(boxing aggregate results), independent of the surface-syntax ergonomics that
already shipped.

## Goals

1. **CURRY-V0 -- indirect call result typing.** `(f x)` where `f : (fn [..] S)`
   and `S` is a by-value struct (or ADT) keeps the full result type `S` at
   elaboration, so a following `(.field ...)` / pattern match resolves. Today
   the indirect-call result is typed as the `int64_t` carrier.
2. **CURRY-V1 -- boxed aggregate results through the closure ABI.** A function
   invoked indirectly whose result is a by-value aggregate returns the value
   **boxed**: heap-allocate, return the pointer as the `int64_t` carrier; the
   call site casts back and dereferences (or copies) to recover the value.
   This makes partial-application thunks and struct-returning lambdas emit
   valid C.
3. **CURRY-V2 -- constructor currying.** With CURRY-V0/V1 in place, re-enable
   the auto-bound constructor's currying: `(Person "Bob")` returns a closure
   expecting `age`, and `((Person "Bob") 40)` yields a `Person`. Remove the
   "currying is intentionally NOT provided" carve-out in
   `elab_call` (CTOR-V0 routing) and the matching note in the structs guide.
4. **DOC -- update guide + reported.** Update the structs guide's currying note;
   resolve and archive
   `docs/reported/struct-return-through-closure-loses-type.md`.

Non-goals:

- Changing the direct-call ABI for struct returns (a direct `(mkp a b)` still
  returns by C value -- only the indirect/closure path boxes).
- Unifying the constructor with a real `defn` in the value namespace. The
  archived plan found a same-named value binding interferes with
  typeclass-instance ABI/dispatch; the shipped routing approach
  (`(Name ...)` -> `make-struct`) stays. Currying is added on top of the
  routing, not by reintroducing a synthesized `defn`.

## Design

### CURRY-V1 -- where the boxing lives

A function has one C signature, so it cannot return "by value for direct calls
and boxed for indirect calls." The boxing therefore lives in the **wrapper that
adapts a function to the closure ABI**, not in the function itself:

- **Partial-application thunks** (`__papN`, synthesized in
  `elab_partial_apply`, `src/compiler/elab_call.c`). The thunk is already a
  wrapper invoked only indirectly. It calls the underlying function (which
  returns the struct by value), boxes the result, and returns the carrier. Its
  result type must carry the full `Type` so emit declares the carrier return
  and inserts the box.
- **Lambdas / first-class function values** whose result is a by-value
  aggregate. A lambda is only ever called indirectly; its emitted body can box
  the result directly. A named function taken as a value needs a boxing shim
  (mirror the existing thin-fn/fat-closure shimming).

Prior art for the carrier/boxing machinery:
[`docs/archive/closure-result-monomorphization-plan.md`](../archive/closure-result-monomorphization-plan.md),
[`docs/archive/aggregate-carrier-abi-plan.md`](../archive/aggregate-carrier-abi-plan.md),
and the carrier-return bridges in `emit_fns.c`
(`emit_carrier_return_override`, the `result_full_type` return-type branch).

### CURRY-V0 -- result typing at the indirect call site

When elaborating `(f args...)` through a function value (`elab_call_head_expr`
/ the poly-call path), set the call expression's type to `f`'s declared result
`Type` (including `StructDef`), not `type_from_kind(result_kind)`. The emit
side then knows to unbox: cast the carrier `int64_t` back to `S*` and
dereference (copy out the by-value struct), so `(.field (f x))` works.

The partial-application thunk's result type must likewise be the full result
`Type` (the archived plan's discarded attempt set
`body_result_type = type_from_kind(result_kind)`, which drops the def).

### CURRY-V2 -- re-enable constructor currying

`elab_call`'s CTOR-V0 block currently rewrites *every* arity of `(Name ...)`
to `make-struct`. For currying, an under-applied positional `(Name a)` should
instead flow through the normal partial-application path so it yields a closure.
Options:

- Keep routing full/keyword forms to `make-struct`; route an under-applied
  *positional* form to a constructor function value that partial-applies. This
  needs a callable backing the constructor -- which is exactly the synthesized
  `defn` the archived plan removed for ABI reasons. Re-evaluate whether, with
  CURRY-V1's boxing, the synthesized `defn` can coexist (the original failure
  was struct-return ABI in instance methods; CURRY-V1 may subsume it), or
  whether a dedicated non-`defn` constructor closure avoids the name clash.

Resolve this once CURRY-V0/V1 land; the boxing fix may change which approach is
viable.

## Fixtures and tests

- `tests/fixtures/struct-curry-ctor/` -- `((Person "Bob") 40)` and a stored
  partial `(let [mk (Person "Bob")] (mk 40))` both produce the same value as
  `(Person "Bob" 40)`; `(.field ...)` on the result works.
- `tests/fixtures/lambda-returns-struct/` -- `(let [f (fn [a] : Person ...)]
  (.name (f 40)))` resolves and runs.
- `tests/fixtures/struct-returning-fn-as-value/` -- a named struct-returning
  function passed to a higher-order function (`map`-style) round-trips.
- Regression-guard the carrier paths: `make-struct-cstr-carrier-bridge` and the
  typeclass-instance-over-struct fixtures (`show-option`, `derive-show-struct`,
  `typeclass-struct-arg-dispatch`) must stay green -- they were the blast radius
  when a same-named value binding was introduced.

## Risks and open questions

- **Blast radius.** The closure/partial-apply emit path is shared by every
  higher-order use of every function. Boxing must be gated precisely on
  "result is a by-value aggregate," leaving scalar/pointer results untouched.
  Gate on the full result `Type`, run the full suite (`bash tests/run.sh`,
  timeout 600000) after each step.
- **Direct vs indirect.** Confirm a function used *both* directly and as a
  value still type-checks: direct calls return by value, the shimmed value
  boxes. The shim, not the function, owns the boxing.
- **ADT results.** ADTs already lower to an `int64_t` carrier in many places;
  verify they need no extra boxing (they may already round-trip), and scope
  CURRY-V1 to genuinely by-value aggregates.
- **Lifetime/ownership of the box.** A boxed `:copy` struct result is a fresh
  heap allocation; decide ownership (caller frees / arena / rc) consistent with
  how other heap aggregates are managed, to avoid leaks under the ASan/LSan
  gate.

## Order of work

1. CURRY-V0 -- indirect-call result typing + a failing fixture for
   `lambda-returns-struct` (elaboration only; codegen still errors).
2. CURRY-V1 -- box aggregate results in the partial-apply thunk and lambda emit;
   unbox at the call site. Land partial-application first (most contained), then
   lambdas, then named-fn-as-value shims.
3. CURRY-V2 -- re-enable constructor currying; fixtures.
4. DOC -- guide note; resolve + archive the reported finding.

Each step gates on the full suite.
