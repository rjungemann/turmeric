---
title: Bare `^fat` Result-Type Inference Plan
category: Planning
description: Close the missing functionality behind bare-fat-param-non-int-result-miscompiles -- a signature-less `^fat g` parameter cannot return a non-int (e.g. :float) result, because the bare-^fat direct-call path erases the closure's result type to int64. Today this is diagnosed (a hard error); this plan makes it work by inferring the result type from the call's context, with a first-class closure type as the end-state.
---

# Bare `^fat` Result-Type Inference -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-03
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
(defn use-int-cb [^fat g x :int] :float (int->float (g x)))
(defn make-inc [n :int] :ptr<void> (fn [x :int] :int (+ x n)))
(defn main [] :int
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

- [ ] Reported repro compiles **with no annotation** and prints `7`:
      `(defn run-with [^fat g x :float] :float (g x))` + `(make-scale 2.0)`.
- [ ] `tests/fixtures/errors/bare-fat-float-result` is **moved out of
      `errors/`** to a positive fixture (`bare-fat-float-result` ->
      stdout `7`) since it now compiles -- or, under A5-conservative, kept
      with the reworded message. State which in the commit.
- [ ] Register-class-distinct fixture: one bare-`^fat` combinator applied
      to an `:int` closure (stays int) and a `:float` closure (infers
      float) in the same file.
- [ ] A6 non-tail fixture round-trips (`5.0`).
- [ ] A4 `let`-binding fixture (if shipped).
- [ ] `arrow-capturing-closure`, `fat-param-capturing-closure`,
      `fat-param-direct-call`, `fat-param-nullary-closure` still green
      (int-carrier + annotated paths unchanged).
- [ ] `bash tests/run.sh`: 0 FAIL, leak detection on. Regenerate any
      `expected.c` snapshots whose emitted thunk gained a float signature.

---

## Phase B -- first-class closure result type (end-state, subsumes A)

`closure-first-class-type-plan.md` (CRU Phase 3 / Option B) already made a
capturing closure a first-class boxed `TY_FN` value that knows its
signature. Phase B extends that so a bare-`^fat` *parameter* retains the
incoming closure's **result kind**, and the call reads it off the value's
type -- no context inference needed.

### B1. Preserve `result_kind` onto the bare-`^fat` parameter binding

Today a bare `^fat g` parameter binding is `TY_PTR_VOID` with no result
info (`closure-representation-unification` Phase 0 made bare `^fat`
default to `:ptr<void>`). Where the parameter binding is built, if the
*argument* flowing in is a boxed `TY_FN` (CRU B-1) carrying
`result_full_type`/`result_kind`, thread that onto the binding's type
(e.g. store a `result_kind` on the binding, or promote the binding to the
annotated `TY_FN`-with-`is_fat` representation that already threads the
result type at `elab_call.c`). Reuse the annotated-`^fat` machinery rather
than inventing a parallel path.

- Pointer: the annotated path at `elab_call.c:~1787+` (`fn_type.kind ==
  TY_FN`, `is_fat`) already threads `e->type` from the declared signature.
  Goal: make the bare path reach the *same* code once the result type is
  known, instead of the `TYPE_INT` default at `elab_call.c:~1704`.

### B2. Type the direct call from the binding's own result type

In the `elab_call.c` `TY_PTR_VOID` fat-dispatch branch (~`1704`), prefer,
in order: (1) the binding's recorded result type (B1); (2) the Phase A
context retype; (3) the `TYPE_INT` default. Once B1 covers a closure, the
call is correctly typed at the *call site* regardless of position -- so a
bare-`^fat` value works in **non-tail** positions too (the one thing
Phase A cannot do).

### B3. Retire Phase A where B2 covers it

Remove the tail-retype pass for any call whose binding carries a result
type (B2 handles it). Keep the pass only for the residual "opaque int64
carrier with no result type" case (e.g. a closure handed in from inline-C
as a raw `int64`), or drop it entirely if no such case remains. Delete the
diagnostic if B2 leaves no float-class ambiguity.

### B4. Generalize beyond `:float`

With the result kind on the value's type, every register-class-distinct
return works uniformly through the existing typed-thunk machinery
(`ensure_typed_thunk_typedef`). Add a fixture per distinct carrier as they
arise.

### Phase B validation

- [ ] Every Phase A fixture passes with the tail-retype pass removed (B2
      covers them from the call site).
- [ ] A bare-`^fat` combinator reused across `:int` and `:float` closures
      with **no annotation and no tail-position requirement** -- including
      a `:float` result consumed in a **non-tail** position (`(let [y (g
      x)] (use-float y))`), which Phase A leaves int64.
- [ ] `bash tests/run.sh`: 0 FAIL.

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
2. B1 -> B2 -> B3 -> B4 -> Phase B validation. Separate PR; only after
   Phase A is green in `main`.
