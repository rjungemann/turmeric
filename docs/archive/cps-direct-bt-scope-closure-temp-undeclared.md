---
title: A `bt-scope` in a non-main defn emits a call to an undeclared closure temp
category: Archive
description: RESOLVED 2026-09-04. Not the cps->direct bridge as filed -- the hoisted closure was classified as an inlinable partial application and DROPPED, because the pap check delegated its "var appears only as a call callee" proof to closure_binding_escapes, which answers a different question and clears a value passed to a non-retaining fn param. One leaf in pap_calls_saturated.
---

# `bt-scope` in a non-main defn: `cps->direct` call names an undeclared temp

**RESOLVED 2026-09-04**, and the filed root cause was wrong -- see *What it
actually was* below. Fixed in `src/passes/cps_ir.c`; regression fixture
`tests/fixtures/cps-bt-scope-thunk-calls-user-fn`, which carries all three
working neighbours as controls.

**Severity: medium.** A hard `cc` failure, not a wrong answer -- but it makes
the natural spelling of a bracketed helper (`(defn round [n] (bt-scope (fn []
...))))`) unusable, which is exactly the spelling `bt-scope` exists for. Every
in-tree caller happens to sit in `main` or pass a thunk that calls nothing, so
nothing caught it.

Found while wiring RM3 R4 (`docs/archive/regions-plan.md`), whose benchmark and
fixtures both wanted that spelling. Independent of `--enable=regions`: it
reproduces identically with the flag off.

## Repro

```turmeric
(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (+ acc n))))

(defn one-round [n : int] : int
  (bt-scope (fn [] (build n 0))))          ;; <- in a NON-main defn

(defn main [] : int (println (one-round 3)) 0)
```

```
$ ./build/tur run r3.tur
r3_tur.c:7012:52: error: '_un_unborrowc_un1444_1445' undeclared (first use in this function)
 7012 |     int64_t __t182 = bt_hyscope((int64_t)(intptr_t)_un_unborrowc_un1444_1445); /* cps->direct */
tur: cc invocation failed (status 256)
```

Three neighbours that all work, which is what narrows it:

- the same `bt-scope` inlined into `main` -- fine;
- `(bt-scope (fn [] (+ n 1)))` in a non-main defn (thunk calls nothing) -- fine;
- `bt-scope` in a non-main defn under a self-recursive caller -- same failure,
  so the caller is not the variable.

The trigger is: **a `bt-scope` in a non-main `defn` whose thunk body calls
another user function.**

## Emitted C

```c
static int64_t one_hyround__cps(int64_t n, DK *__kont) {
    int64_t __t182 = bt_hyscope((int64_t)(intptr_t)_un_unborrowc_un1444_1445); /* cps->direct */
    return dk_run(__kont, (intptr_t)(__t182));
}
```

## What the filing guessed, and why it was wrong

The report blamed the emitter: "the `cps->direct` bridge emits the CALL but not
the statements that build its `^fat` closure argument". That reads plausibly off
the emitted C -- the call is there, the operand setup is not -- and it is wrong.
`--dump-cps` settles it in one line:

```
cps-fn one-round [n] k:cont<int> internal
  tailcall bt-scope(__borrowc_1444 k)  ; cps->cps
cps-end
```

The CPS **IR itself** names a free variable. Nothing was dropped at emit; the
binding never entered the IR. Reading the emitted C and reasoning backwards
cost a wrong hypothesis that a single dump would have refuted -- the standing
lesson being to ask the earliest representation that can answer, not the last
one that shows the symptom.

## What it actually was

`hoist_borrowed_closure_args` (elab_call.c) rewrites the call to

```
(let [__borrowc_1444 (fn [] (build n 0))]
  (bt-scope __borrowc_1444))
```

`pap_register_let` (src/passes/cps_ir.c) then classified `__borrowc_1444` as an
inlinable **partial application** -- a closure whose only use is a saturated call
to it, which can be replaced by the underlying call and the closure dropped.
`cps_tail`'s EX_LET arm duly dropped the binding. But the use here is not a call
of the closure at all: it is the closure passed as an ARGUMENT, and the call that
receives it stayed.

The check that should have stopped this said so in its own comment:

> Paired with `closure_binding_escapes(e,var) == false` (which proves `var`
> appears ONLY as a call callee) ...

That parenthesis is false, and had been since the two were paired.
`closure_binding_escapes` answers **"may this env be freed at scope exit?"**, and
it deliberately reports NO escape for a value passed to a `^borrow` or
inferred-non-retaining fn param -- that relaxation is what makes RM1's env free
work at all. `bt-scope`'s `^fat body` is exactly such a param. So the pap check
was resting its soundness on a predicate that had been correctly relaxed for a
different client.

## The fix

One leaf in `pap_calls_saturated`:

```c
case EX_VAR:
    return e->as.var.binding != var;
```

A bare reference REACHED there is by construction a non-callee use -- the
EX_CALL arm never recurses into a callee that is `var` -- so an argument, an
init, or a branch value now refuses the inlining locally. The
`closure_binding_escapes` call stays at the registration site as a second,
independent condition rather than as this one's proof.

**Verified by subtraction:** disabling this leaf alone reproduces the failure
exactly; nothing else was needed.

## A tightening that was tried and REMOVED

`pap_extract` never checks that the closure's own arity matches the remaining
arity it computes, so `(fn [] (build n 0))` -- one capture, 2-arity target --
reads as a 1-argument partial application although it takes none. Adding
`c->fn->n_params == tarity - c->n_captures` looks like the obvious second guard.
It is wrong: `n_params` is the LIFTED lambda's count and includes the env
pointer, so the thunk measures 1 and satisfies the test, while a genuine pap
measures rem+1 and would be REJECTED -- silently disabling the optimization. It
fixed nothing here (checked: the repro still failed with it in and the EX_VAR
leaf out). A comment at that site records this so it is not re-derived.

## Neighbours that worked, and why nothing caught this

- the same bracket inlined into `main`;
- a thunk whose body calls nothing;
- and the shape under a self-recursive caller failed too, so the caller was
  never the variable.

Every in-tree `bt-scope` caller happened to be one of the first two. The
regression fixture keeps all three as controls, so a future tightening of the
pap check that over-corrects says so.

## Downstream

`benchmarks/bench-regions-subst.tur` and
`tests/fixtures/region-scope-value-survives` are on the natural spelling now.
That mattered beyond tidiness: with the bracket in a non-main defn the function
is CPS-lowered, and RM3 R4's region hook -- which lived only in `emit_value` --
never fired. The fixture measured 909 live blocks with `--enable=regions` on and
909 with it off. R4 gained a second hook on the CPS `CT_TAILCALL` cps->direct arm
as a direct result of this fix exposing the natural shape.
