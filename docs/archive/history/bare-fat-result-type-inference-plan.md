---
title: Bare `^fat` Result-Type Inference Plan
category: Planning
description: Close the missing functionality behind bare-fat-param-non-int-result-miscompiles -- a signature-less `^fat g` parameter cannot return a non-int (e.g. :float) result, because the bare-^fat direct-call path erases the closure's result type to int64. Today this is diagnosed (a hard error); this plan makes it work by inferring the result type from the call's context, with a first-class closure type as the end-state.
---

# Bare `^fat` Result-Type Inference -- Plan

> **Status:** ARCHIVED -- Phase A implemented and merged (#208); reported gap
> closed and the plan is functionally delivered. Phase B was **deferred** (see
> "Phase B feasibility & decision" below) and has been **broken out into its own
> plan**:
> [docs/upcoming/v1/bare-fat-result-monomorphization-plan.md](../../upcoming/v1/bare-fat-result-monomorphization-plan.md).
> This document is retained for the Phase A record and the deferral rationale.
> **Last Updated:** 2026-06-05
> **Type:** compiler -- closure ABI / type inference
> **Resolves the missing functionality flagged in:**
> - [bare-fat-param-non-int-result-miscompiles.md](../reported/bare-fat-param-non-int-result-miscompiles.md)
> **Builds on:**
> - [closure-representation-unification-plan.md](closure-representation-unification-plan.md)
> - [closure-first-class-type-plan.md](closure-first-class-type-plan.md)
> - [closure-typed-invocation-abi-plan.md](closure-typed-invocation-abi-plan.md)

---

## The missing functionality

A `^fat` parameter has two spellings:

| Spelling | Generic over signature? | Result type | Float result works? |
|---|---|---|---|
| `^fat g :(fn [:float] #{} :float)` (annotated) | no -- pins one signature | threaded from the annotation | **yes** |
| `^fat g` (bare, signature-less) | yes -- any arity/types | **erased to int64** | **no** |

The annotated form already works for any register class (it carries the
result type, and `emit_expr.c` dispatches through a typed thunk keyed on
the call expr's `e->type`). The **bare** form is the generic,
"don't-pin-the-signature" form used by int-carrier combinators (arrow
`>>>`, `option-map`, ...). Because a bare `^fat g` records no signature,
the direct-call path (`elab_call.c`, the `TY_PTR_VOID` fat-dispatch
branch) hard-codes the call's result type to `TYPE_INT`. For a closure
that actually returns `:float`, the int64 result is read from the integer
register (rax) instead of xmm0 -- a silent miscompile.

**Today's state (interim):** that miscompile is *diagnosed*, not fixed. A
bare-`^fat` call in the result position of a `:float` function is a hard
error pointing at the annotated form (`elab_fns.c`,
`bare_fat_int_tail_call` + the check in `elab_defn`; fixture
`tests/fixtures/errors/bare-fat-float-result`).

**The missing functionality this plan delivers:** a single *generic*,
signature-less `^fat g` parameter that returns a non-int result (`:float`
first; any register-class-distinct type by construction). After this
plan, `(defn run-with [^fat g x :float] :float (g x))` compiles and
prints `7` -- no annotation required -- and the diagnostic is replaced by
successful inference.

## Why it is tractable

`emit_expr.c` already lowers the bare-`^fat` (CY2) dispatch using the call
expression's `e->type` (`ret_c = type_c_name(e->type)` and
`ensure_typed_thunk_typedef(..., e->type, ...)`, around `emit_expr.c:1799`).
So codegen is **already correct for any result type** -- the only thing
wrong is that elaboration stamps the call `TYPE_INT`. If elaboration sets
the right result type on the `EX_CALL`, codegen follows with no further
change. The whole problem is recovering the result type at elaboration
time.
## Approach

Two layers, smallest first.

- **Phase A -- tail-position retype pass.** A *post-elaboration* pass that
  re-stamps the result type of a bare-`^fat` call when it sits in a
  position whose expected type is known and unambiguous (a function's
  result/tail position; an annotated `let` binding). Delivers the
  user-visible functionality (`:float` returns with no annotation) with a
  contained, self-correcting change. **This phase alone closes the
  reported gap.**
- **Phase B -- first-class closure result type.** Carry the result kind on
  the bare-`^fat` value's type so the call reads it directly, removing the
  need to infer from context at all. The end-state already chosen for the
  closure work; scoped here as the follow-through that subsumes A.

### Why a post-pass, not a stateful "expected-type" hint

The obvious design -- thread an `expected_result_kind` field on `Elab`,
set before elaborating the body, consult at the call -- is **not
tail-precise**. The field would be live for the *entire* body subtree, so
a bare-`^fat` call in a non-tail position (e.g. an argument to another
call) would wrongly pick up the function's return type. Making it precise
would require every elaborator that descends into a non-result position to
save/clear the field -- invasive and fragile.

A **post-elaboration tail walk** is tail-precise *by construction*: it only
visits result positions (do-last, let-body, both if-branches), so it can
never retype a non-tail call. It needs no new `Elab` state and cannot leak
into nested lambdas (each `defn`/`fn` runs the pass on its own body). The
`EX_CALL` node is mutable and `emit_expr.c` reads the call's `e->type` at
codegen, so re-stamping `tail_call->type` after elaboration is sufficient
-- no second elaboration pass.

### Hard constraint -- soundness

Contextual inference **trusts the declared return type as the closure's
result type**: "this function is declared `:float` and its tail is
`(g x)`, therefore `g` returns `:float`." This is correct whenever the
signature is honest. If a bare-`^fat` closure's *real* result type differs
from the declared return, the bare form cannot detect it (it has no
signature) -- that latent mismatch is inherent to the signature-less form
and is exactly why the annotated form exists for the checked case. Phase B
(value-typed) is the checked path. Document this limitation next to the
pass.

The pass must **never** retype a call whose value does not flow into the
inferred slot. The tail-only walk guarantees this; A6 is the regression
gate.

---

## Phase A -- tail-position retype pass (delivers the functionality)

### A1. Generalize the tail walk to a retype helper

`elab_fns.c` already has `bare_fat_int_tail_call` (the diagnostic's
detector) and `check_no_borrow_escape` (the tail-walk shape). Replace the
detector with a **mutating** walk that re-stamps every bare-`^fat` int64
tail call to a target kind, and reports whether it changed anything:

```c
/* Re-stamp bare-^fat int64 calls in `tail`'s result position(s) to
 * `target` (a concrete non-int register-class kind).  Returns true if any
 * call was retyped.  Tail-precise: only result positions are visited, so a
 * non-tail bare-^fat call is never touched.  See
 * docs/upcoming/bare-fat-result-type-inference-plan.md. */
static bool retype_bare_fat_tail_calls(Expr *tail, TypeKind target) {
    if (!tail) return false;
    switch (tail->kind) {
        case EX_DO: {
            if (tail->as.do_.n == 0) return false;
            Expr *last = tail->as.do_.items[tail->as.do_.n - 1];
            bool changed = retype_bare_fat_tail_calls(last, target);
            if (changed) tail->type = last->type;   /* keep wrapper consistent */
            return changed;
        }
        case EX_LET:
        case EX_LETREC: {
            bool changed = retype_bare_fat_tail_calls(tail->as.let_.body, target);
            if (changed) tail->type = tail->as.let_.body->type;
            return changed;
        }
        case EX_IF: {
            bool c1 = retype_bare_fat_tail_calls(tail->as.if_.then_, target);
            bool c2 = retype_bare_fat_tail_calls(tail->as.if_.else_or_null, target);
            /* The if's type follows its (now-consistent) then-branch. */
            if ((c1 || c2) && tail->as.if_.then_) tail->type = tail->as.if_.then_->type;
            return c1 || c2;
        }
        case EX_CALL: {
            Binding *b = tail->as.call_.fn_binding;
            if (b && b->is_fat && b->type.kind == TY_PTR_VOID &&
                tail->type.kind == TY_INT) {
                tail->type = type_from_kind(target);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}
```

Notes:
- `tail->type` on `EX_CALL` is what `emit_expr.c:~1799` reads
  (`type_c_name(e->type)` / `ensure_typed_thunk_typedef(..., e->type, ...)`),
  so re-stamping it is the entire fix for codegen.
- Keep `bare_fat_int_tail_call` too (or have the diagnostic call the new
  walk and check the boolean) -- the diagnostic becomes the *fallback* for
  positions the retype pass does not reach (A5).

### A2. Run the pass on `defn` bodies

In `elab_defn` (`elab_fns.c`), the diagnostic block currently sits right
after the `any`-widening and before `check_no_borrow_escape` (it returns a
hard error for `return_kind == TY_FLOAT` + a bare-`^fat` tail call).
**Replace** that error with the retype call, and re-derive `body->type`:

```c
/* bare-fat-param-non-int-result: a bare `^fat g` call in result position
 * has no recorded result type (typed int64).  When the function declares a
 * non-int register-class return, infer the closure's result type from the
 * declared return and re-stamp the tail call(s) so codegen reads the right
 * register.  Tail-precise; sound under an honest signature (see plan). */
if (return_kind == TY_FLOAT) {            /* extend the set in A7 */
    if (retype_bare_fat_tail_calls(body, return_kind) &&
        body->type.kind == TY_INT) {
        body->type = type_from_kind(return_kind);
    }
}
```

`return_kind` is already finalized here (annotated returns are parsed
~`elab_fns.c:1300+`, before the body at ~`1702`). No new state, no
save/restore, no early-return bookkeeping.

### A3. Run the pass on `fn` (lambda) bodies

`elab_fn` builds its own `return_kind` (~`elab_fns.c:2526+`) and elaborates
its body (~`2638+`). Add the identical retype call just before it
constructs the `EX_CLOSURE`/`fd` (after `body` is final, ~`2717`). This
makes a lambda whose declared result is `:float` and whose tail is a
bare-`^fat` call work too, and -- because each `fn` runs its own pass --
there is no cross-lambda leakage.

### A4. Annotated `let` bindings

`LetBinding { Binding *binding; Expr *init; }` (`expr.h:379`). In the `let`
elaborator (`elab_forms.c`), after a binding's `init` is elaborated, if
`binding->type.kind` is a non-int register-class kind and the init's tail
is a bare-`^fat` int64 call, retype it:

```c
if (lb->binding->type.kind == TY_FLOAT &&       /* declared-typed binding */
    retype_bare_fat_tail_calls(lb->init, lb->binding->type.kind) &&
    lb->init->type.kind == TY_INT) {
    lb->init->type = type_from_kind(lb->binding->type.kind);
}
```

Enables `(let [y :float (g x)] ...)`. Lower priority than A2/A3; ship if
the `let`-binding path is reachable in real code, otherwise defer and note
it.

### A5. Demote the diagnostic to a fallback

After A2, the float-tail case is *handled*, so the hard error from the
current diagnostic must no longer fire for it. Two options, pick one:

- **Preferred:** delete the hard-error diagnostic entirely. The retype
  pass covers every tail position; any remaining bare-`^fat` int64 value
  reaching a float slot is a genuinely ambiguous non-tail position, which
  stays int64 (today's behavior) rather than erroring -- consistent with
  how every other non-tail int-carrier use is treated.
- **Conservative:** keep the diagnostic but only fire it when
  `retype_bare_fat_tail_calls` returned `false` *and* the body type is
  still int64 against a float return -- i.e. the value reaches the return
  through a path the walk did not cover. Reword the message: "could not
  infer the closure's result type here; annotate `^fat g :(fn [...] :T)`."

Decide during implementation; default to **preferred** unless a fixture
shows an uncovered tail shape.

### A6. Soundness gate -- non-tail must NOT be retyped

Add a fixture that breaks if the walk ever leaves tail position:

```turmeric
;; (g x) is an ARGUMENT to int->float, not the function's tail, so it must
;; stay int64 and round-trip.  If the retype pass wrongly fired here, the
;; float read would be garbage.
(defn use-int-cb [^fat g x : int] : float (int->float (g x)))
(defn make-inc [n : int] : ptr<void> (fn [x : int] : int (+ x n)))
(defn main [] : int
  (println (use-int-cb (make-inc 1) 4))   ;; (4+1)=5 -> 5.0
  0)
```

Expected stdout `5` (as a float). The walk's `EX_CALL` case only matches
when the call *is* the tail; here the tail is the `int->float` call, whose
`fn_binding` is not `^fat`, so recursion stops -- the inner `(g x)` is
never visited.

### A7. Generalize the target-kind set

`return_kind == TY_FLOAT` is the first cut. Audit `TypeKind` for other
non-int register classes (any future `TY_FLOAT32`/SIMD/by-value-small-
struct carrier). Extract a predicate:

```c
static bool kind_is_non_int_register_class(TypeKind k) {
    /* Integer-register carriers (int/cstr/ptr/rc/...) round-trip through
     * the int64 slot; only xmm/by-value-distinct returns need retyping. */
    return k == TY_FLOAT /* || k == TY_FLOAT32 || ... */;
}
```

Use it in A2/A3/A4 instead of the bare `== TY_FLOAT`.

### Phase A validation

- [x] Reported repro compiles **with no annotation** and prints `7`:
      `(defn run-with [^fat g x :float] :float (g x))` + `(make-scale 2.0)`.
      (`tests/fixtures/bare-fat-float-result`)
- [x] `tests/fixtures/errors/bare-fat-float-result` is **moved out of
      `errors/`** to a positive fixture (`bare-fat-float-result` ->
      stdout `7`) since it now compiles. **A5-preferred** was taken: the
      hard-error diagnostic is deleted outright (the tail walk covers every
      tail position; a residual non-tail bare-`^fat` int64 value stays
      int64, consistent with every other int-carrier use).
- [x] Register-class-distinct fixture: one bare-`^fat` combinator shape
      applied to an `:int` closure (stays int) and a `:float` closure
      (infers float) in the same file
      (`tests/fixtures/bare-fat-int-and-float-combinator`).
- [x] A6 non-tail fixture round-trips (`5`)
      (`tests/fixtures/bare-fat-nontail-int-roundtrip`).
- [x] A4 `let`-binding fixture shipped
      (`tests/fixtures/bare-fat-let-float-binding`); the retype runs before
      the annotation-vs-init kind check so `[y : float (g x)]` is accepted.
- [x] `arrow-capturing-closure`, `fat-param-capturing-closure`,
      `fat-param-direct-call`, `fat-param-nullary-closure` still green
      (int-carrier + annotated paths unchanged).
- [x] `bash tests/run.sh`: **1347 passed, 0 failed**, leak detection on.
      New fixtures carry regenerated `expected.c` snapshots.

> **A3 note (updated 2026-06-04):** the `elab_fn` (lambda) retype call is in
> place for symmetry. `^fat` on a *lambda* binder (`(fn [^fat g ...] ...)`) is
> now accepted -- the pass fires on `fn` binders too, per
> [bare-fat-lambda-param-plan.md](../archive/history/bare-fat-lambda-param-plan.md). The lambda
> path is no longer dormant; fixtures under
> `tests/fixtures/{bare-fat-lambda-param,annotated-fat-lambda-param,
> bare-fat-lambda-closure-returning}` exercise it.

---

## Phase B feasibility & decision (2026-06-04)

Phase B was investigated end-to-end before committing to it. The conclusion:
**its only sound general delivery is a new monomorphization mechanism, and
there is no current consumer that needs it.** It is therefore *deferred*, not
abandoned -- the design sketch below is ready to pick up the moment a real
non-tail float-result consumer appears.

### What the investigation found

1. **Codegen is already result-type-correct.** The CY2 bare-`^fat` dispatch in
   `emit_expr.c` (the `n_args > 0` fat-call branch, ~`emit_expr.c:1815-1852`)
   reads the call's `e->type` and builds a typed thunk via
   `ensure_typed_thunk_typedef(ctx, ctx->file, e->type, ...)`. A `:float`
   result lands in xmm0 the moment `e->type` is `TY_FLOAT`.
2. **The only defect is in elaboration.** The CY2 branch in `elab_call.c`
   (`fn_binding->type.kind == TY_PTR_VOID` fat-dispatch, ~`elab_call.c:1717`)
   hard-stamps the call `TYPE_INT` (~`elab_call.c:1729`). Phase A re-stamps
   that `e->type` *post-hoc* in tail positions (`retype_bare_fat_tail_calls`,
   `elab_fns.c:378`).
3. **The non-tail gap is the entire remaining delta.** A bare-`^fat` float call
   in a non-tail position -- e.g. `(let [y (g x)] (use-float y))` where `y`
   carries no annotation -- is left int64 by Phase A's tail-only walk. This is
   the one thing Phase B set out to add.
4. **No closure-result monomorphization infra exists.** There is per-use-site
   *ADT* monomorphisation (`elab_call.c:~1219`, TY_APP) and binding-name
   mangling (`elab_mangle_binding_name`, `elab_core.c:1621`), but nothing that
   specializes a *function body* over an incoming closure's result kind.

### Why a value-typed binding (B1 as literally written) does not work

B1 says "thread the incoming closure's `result_kind` onto the parameter
binding." But a bare `^fat g` is **generic over any closure signature**, and the
body of (e.g.) `run-with` is compiled **once**. The closure's concrete result
kind only exists at each *call site* of `run-with`, never inside its single
compiled body. Float is exactly the case that **cannot** share a uniform
representation (integer register vs xmm), so the body cannot be made
representation-agnostic the way an int-carrier combinator can. There is no
honest in-body source for `g`'s result kind, so any "store it on the binding"
scheme degenerates back to the contextual return-type heuristic Phase A already
implements -- which is tail-only and would be **unsound** if naively extended to
non-tail positions (the function's declared return need not equal the closure's
result type off the tail).

### The only sound general mechanism: monomorphize over closure result-kind

To type `(g x)` as `:float` in *any* position, a bare-`^fat` function must be
**specialized per call site** to the incoming closure's result kind:

- At a call `(run-with <closure> ...)`, read the boxed-`TY_FN` argument's
  `result_full_type`/`result_kind` (CRU B-1 already carries these on a
  first-class closure value).
- Clone + mangle a specialized body `run-with$float` whose `^fat g` binding
  records `result_kind = TY_FLOAT`. Dedup specializations by (callee, result
  kind) so each distinct kind is emitted once.
- Then the **B2** call-site typing falls out: the CY2 branch at
  `elab_call.c:~1717` prefers, in order, (1) the binding's recorded result kind
  (now present in the specialized clone), (2) the Phase A contextual retype, (3)
  the `TYPE_INT` default. Because (1) is set at the call site, the call types
  correctly **regardless of position** -- delivering the non-tail case.
- **B3** then retires the Phase A tail-retype pass for any call whose binding
  carries a result kind, keeping it only for the residual "opaque int64 carrier
  with no result type" case (e.g. a closure handed in from inline-C as a raw
  `int64`).

Open issues this mechanism must handle (the reason it is *large*): recursive and
mutually-recursive bare-`^fat` callees, interaction with exported bindings and
the existing poly-call / `is_poly_fn` paths, specialization-set dedup across
modules, and snapshot churn from the newly emitted specialized C functions.

### Decision: deferred until a real consumer exists

Phase B is **not built now**, on cost/value grounds:

- Phase A already closed the reported gap; the repro prints `7`
  (`tests/fixtures/bare-fat-float-result`).
- There is **no current consumer** that needs the non-tail float case. Every
  real bare-`^fat` combinator in the tree (arrow `>>>`, `option-map`) is
  int-carrier; the only float-result use is the tail-position repro, which
  already works. Per the Phase A note above, even bare-`^fat` *lambda* params
  are still dormant/unsupported.
- The cost -- a whole new monomorphization pass on the closure ABI -- is
  disproportionate to demonstrated need and is pure new ABI risk.

**Re-open trigger:** a real (non-fixture) caller that needs a bare-`^fat`
`:float` (or any future non-int register-class) result in a non-tail position,
or bare-`^fat` lambda params becoming supported and surfacing the same need.
When that lands, implement the monomorphization mechanism sketched above
(B1'/B2/B3/B4), gate with a non-tail float fixture
(`(let [y (g x)] (use-float y))`), regenerate snapshots, and keep the suite
green.

## Phase B -- broken out into its own plan

The Phase B design (first-class closure result type, and the
monomorphization framing that supersedes the original B1-B4 value-typed
sketch) now lives in its own document:

- **[docs/upcoming/v1/bare-fat-result-monomorphization-plan.md](../../upcoming/v1/bare-fat-result-monomorphization-plan.md)**

That plan carries the remaining-gap statement, the per-call-site
monomorphization mechanism (B1'/B2/B3/B4), the open issues (recursion,
exports, cross-module dedup, snapshot churn), the non-tail gate fixture,
and the validation checklist. It stays deferred until a real non-tail
consumer appears (the re-open trigger above).

---

## Risks

- **New silent miscompile (Phase A).** Mitigated structurally by the
  tail-only walk; A6 is the blocking gate. Any fixture where a retyped
  call's value reaches a differently-typed slot is a release blocker.
- **Dishonest signature (Phase A).** Contextual inference trusts the
  declared return as the closure's result type; a bare-`^fat` closure that
  actually returns a different type is a latent mismatch the bare form
  cannot check. Documented limitation; Phase B (value-typed) is the
  checked path. Not a regression -- the bare form had no checking before.
- **Int-carrier regression.** Arrow/option-map rely on bare `^fat` = int64.
  The pass only fires for `kind_is_non_int_register_class(return_kind)`, so
  their `:int`/closure-returning bodies are untouched. Gate with
  `arrow-capturing-closure`.
- **Snapshot churn.** New float thunks change emitted C; regenerate
  `expected.c` in the same PR (see CLAUDE.md "Fixture Snapshots").

## Acceptance checklist

- [ ] A bare `^fat g` returns `:float` with no annotation in tail position
      (Phase A) and in any position (Phase B).
- [ ] The reported repro prints `7` without annotation.
- [ ] A6 non-tail fixture round-trips -- no new miscompile.
- [ ] Arrow/option-map/`fat-param-*` fixtures stay green.
- [ ] `bash tests/run.sh`: 0 FAIL, leak detection on.

## Implementation order (single PR per phase)

1. A1 helper -> A2 `defn` -> A6 gate fixture -> A7 predicate -> A5 demote
   diagnostic -> A3 `fn` -> Phase A validation -> regenerate snapshots.
   (A4 `let` optional, same PR or a follow-up.)
2. **Phase B is deferred** (see "Phase B feasibility & decision"). When a real
   non-tail consumer appears: monomorphize bare-`^fat` callees over the
   closure's result kind (B1') -> B2 call-site typing -> B3 retire the tail
   pass -> B4 generalize -> Phase B validation. Separate PR.
