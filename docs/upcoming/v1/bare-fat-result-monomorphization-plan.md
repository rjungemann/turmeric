---
title: Bare `^fat` Result-Kind Monomorphization Plan (Phase B)
category: Planning
description: Make a signature-less `^fat g` parameter return a non-int register-class result (e.g. :float) in ANY position -- including non-tail -- by monomorphizing the bare-^fat callee over the incoming closure's result kind. Subsumes the Phase A tail-only retype pass shipped in the predecessor plan.
---

# Bare `^fat` Result-Kind Monomorphization -- Plan (Phase B)

> **Status:** Deferred -- not started. The reported gap is already closed by
> Phase A (tail-position retype pass), which shipped and merged in #208. This
> plan is the *follow-through* that handles the one case Phase A structurally
> cannot: a bare-`^fat` non-int result consumed in a **non-tail** position.
> **Re-open trigger:** a real (non-fixture) caller that needs a bare-`^fat`
> `:float` (or any future non-int register-class) result off the tail, **or**
> bare-`^fat` lambda params surfacing the same need.
> **Last updated:** 2026-06-05
> **Type:** compiler -- closure ABI / monomorphization
> **Supersedes Phase B of:**
> - [bare-fat-result-type-inference-plan.md](../../archive/history/bare-fat-result-type-inference-plan.md) (Phase A shipped; this is its deferred Phase B)
> **Builds on:**
> - [closure-representation-unification-plan.md](../../archive/closure-representation-unification-plan.md)
> - [closure-first-class-type-plan.md](../../archive/closure-first-class-type-plan.md)
> - [closure-typed-invocation-abi-plan.md](../../archive/closure-typed-invocation-abi-plan.md)

---

## The remaining gap (what Phase A cannot do)

Phase A re-stamps a bare-`^fat` int64 call's result type **in tail positions
only** (do-last, let-body, both if-branches), inferring it from the enclosing
function's declared return. That is tail-precise *by construction* and sound
under an honest signature, but it leaves exactly one case open:

```turmeric
;; `y` carries no annotation, and `(g x)` is NOT the function's tail -- it
;; feeds `use-float`.  Phase A's tail-only walk never visits it, so the call
;; stays int64 and the :float result is read from rax instead of xmm0.
(defn run-with [^fat g x : float] : float
  (let [y (g x)]          ;; non-tail bare-^fat float call -- left int64
    (use-float y)))
```

Phase A's design note explains why this is *deliberately* out of scope: the
function body is compiled **once**, the closure's concrete result kind only
exists at each *call site* of `run-with`, and float cannot share a uniform
integer-register representation. Extending the contextual return-type heuristic
to non-tail positions would be **unsound** -- the declared return need not equal
the closure's result type off the tail.

## Why a value-typed binding does not work (recap)

The original "thread the incoming closure's `result_kind` onto the parameter
binding" sketch fails because a bare `^fat g` is **generic over any closure
signature** and the body is compiled once. There is no honest in-body source for
`g`'s result kind, so any "store it on the binding" scheme degenerates back to
the Phase A contextual heuristic -- which is tail-only and unsound if naively
extended. The only sound general mechanism specializes the body **per call
site**.

## The mechanism: monomorphize over closure result-kind

To type `(g x)` as `:float` in *any* position, a bare-`^fat` function must be
**specialized per call site** to the incoming closure's result kind.

### B1'. Specialize the bare-`^fat` callee per closure result-kind

At a call `(run-with <closure> ...)`:

- Read the boxed-`TY_FN` argument's `result_full_type` / `result_kind` (CRU B-1
  already carries these on a first-class closure value).
- Clone + mangle a specialized body `run-with$float` whose `^fat g` binding
  records `result_kind = TY_FLOAT`.
- **Dedup** specializations by `(callee, result_kind)` so each distinct kind is
  emitted once. Reuse the existing binding-name mangling
  (`elab_mangle_binding_name`, `elab_core.c`) rather than inventing a parallel
  scheme.

Model the clone+dedup on the per-use-site **ADT** monomorphization already in
`elab_call.c` (TY_APP path); this is the closest existing precedent.

### B2. Type the direct call from the binding's own result kind

In the `elab_call.c` CY2 `TY_PTR_VOID` fat-dispatch branch, prefer, in order:

1. the binding's recorded result kind (set by B1' in the specialized clone),
2. the Phase A contextual retype (`retype_bare_fat_tail_calls`),
3. the `TYPE_INT` default.

Because (1) is established at the call site, the call types correctly
**regardless of position** -- delivering the non-tail case. Codegen needs no
change: `emit_expr.c` already reads `e->type` and builds the typed thunk via
`ensure_typed_thunk_typedef`, so a `:float` result lands in xmm0 the moment
`e->type` is `TY_FLOAT`.

### B3. Retire the Phase A tail-retype pass where B2 covers it

Remove `retype_bare_fat_tail_calls` for any call whose binding now carries a
result kind (B2 handles it from the call site). Keep the pass only for the
residual "opaque int64 carrier with no result type" case (e.g. a closure handed
in from inline-C as a raw `int64`), or drop it entirely if no such case remains.

### B4. Generalize beyond `:float`

With the result kind on the specialized binding, every register-class-distinct
return works uniformly through `ensure_typed_thunk_typedef`. Add a fixture per
distinct carrier as it arises. Drive the target set from
`kind_is_non_int_register_class` (`elab_fns.c`), the predicate Phase A already
extracted.

## Open issues this mechanism must handle

These are the reason Phase B is *large* and was deferred:

- **Recursive / mutually-recursive** bare-`^fat` callees (a specialization that
  calls itself).
- Interaction with **exported bindings** and the existing poly-call /
  `is_poly_fn` paths.
- **Specialization-set dedup across modules** (the same `(callee, kind)` reached
  from two translation units).
- **Snapshot churn** from newly emitted specialized C functions -- regenerate
  `tests/fixtures/*/expected.c` in the same PR (see CLAUDE.md "Fixture
  Snapshots").

## Validation

- [ ] Every Phase A fixture still passes (B2 covers them from the call site):
      `bare-fat-float-result`, `bare-fat-int-and-float-combinator`,
      `bare-fat-nontail-int-roundtrip`, `bare-fat-let-float-binding`.
- [ ] **New gate (the deliverable):** a bare-`^fat` `:float` result consumed in
      a **non-tail** position with no annotation round-trips correctly --
      `(let [y (g x)] (use-float y))`, which Phase A leaves int64.
- [ ] A bare-`^fat` combinator reused across `:int` and `:float` closures in the
      same file, with no annotation and no tail-position requirement, emits one
      specialization per result kind (dedup verified in `expected.c`).
- [ ] Arrow `>>>` / `option-map` / `fat-param-*` fixtures stay green (int-carrier
      path is the `result_kind == TY_INT` specialization, unchanged shape).
- [ ] `bash tests/run.sh`: 0 FAIL, leak detection on.

## Risks

- **New ABI risk.** This is a whole new monomorphization pass on the closure
  ABI; the open issues above (recursion, exports, cross-module dedup) are where
  miscompiles would hide. Gate aggressively with the non-tail fixture before
  retiring any Phase A path.
- **Snapshot churn.** Specialized C functions change emitted output; regenerate
  snapshots in the same PR.

## Implementation order (single PR)

B1' specialize callee per result-kind -> B2 call-site typing -> non-tail gate
fixture -> B3 retire the tail pass where covered -> B4 generalize -> regenerate
snapshots -> Phase B validation.
