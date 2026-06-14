---
title: Rewriting Eq[Vec] to call a constrained-poly helper drops unrelated sibling specs from the interning worklist
severity: blocks the M5 Eq Vec by-value rewrite (single-body-two-ABIs); silent miscompile risk -- a needed by-value specialization is omitted, leaving a carrier-base call against a by-value struct (cc type error)
date: 2026-06-14
---

## Summary

When `stdlib/vec.tur`'s `(definstance Eq [Vec])` is rewritten so its body
calls a sibling *constrained-polymorphic* helper (`vec-eq-loop-byval [A]
[(Eq A)] ...`) instead of the existing `^fat`-lambda + carrier
`vec-eq-loop`, the ABI-specialization worklist stops interning the
by-value spec for an **unrelated** stdlib accessor -- e.g. `thead`
(`(defn thead [A] [l : (Cons A)] : A (.head l))`).  The downstream
program then emits `thead(Cons__int)` against the carrier base
`thead(int64_t)` -> `error: incompatible type for argument 1 of 'thead'`.

This is the same *class* of defect as gap-4's hamt-delete regressor
(`m5-instance-spec-doesnt-propagate-constraint-var-bindings.md`): adding
or changing one instance-method's spec composition perturbs the global
spec-interning worklist and a sibling spec that used to be minted no
longer is.

## Repro

In `stdlib/vec.tur`, replace the `Eq [Vec]` instance with the by-value
form:

```turmeric
(defn vec-len-byval [A] [v : (Vec A)] : int
  (.len v))

(defn vec-get-byval [A] [v : (Vec A) i : int] : A
  (unsafe (:: (array-get-unchecked (.data v) i) A)))

(defn vec-eq-loop-byval [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (vec-get-byval x i) (vec-get-byval y i))
      (vec-eq-loop-byval x y (+ i 1) len)
      false)))

(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [lx (vec-len-byval (:: x (Vec A)))
          ly (vec-len-byval (:: y (Vec A)))]
      (if (= lx ly)
        (vec-eq-loop-byval (:: x (Vec A)) (:: y (Vec A)) 0 lx)
        false))))
```

Then `tur build tests/fixtures/list-basic/input.tur` fails:

```
error: incompatible type for argument 1 of 'thead'
note: expected 'int64_t' but argument is of type 'Cons__int'
```

The emitted C has **no** `thead__spec__...` (`grep -c thead__spec` == 0),
even though the call site constructs `Cons__int l = (Cons__int){...}`
by value and passes it to `thead`.

### Bisection (each step rebuilds + checks `list-basic`)

- clean tree: `list-basic` passes, `thead__spec` interned.
- add the three `*-byval` helpers only (Eq Vec UNCHANGED): `list-basic`
  passes.  The helpers' mere existence is harmless.
- rewrite the `Eq [Vec]` instance body to call `vec-eq-loop-byval`:
  `list-basic` fails, `thead__spec` missing.

So the trigger is specifically the **instance-method body calling a
constrained-poly helper**, not the helper definitions and not the
field-access codegen (the failure reproduces with the companion
`emit_expr.c` field-access change reverted).

## Observed vs expected

- Observed: `thead`'s by-value spec is not interned once `Eq[Vec]`'s
  body composes through a constrained-poly helper; `list-basic` (and
  ~10 other list/option/tuple fixtures) fail to build.
- Expected: rewriting one instance body must not change whether an
  unrelated accessor (`thead`/`ttail`/`unwrap`) gets its by-value spec.

## Root-cause hypothesis (not yet pinned)

The spec worklist in `emit_module.c` (`emit_abi_intern_spec` +
`emit_abi_register_call`) is order- and content-sensitive.  Gap 4's
emit-side augmentation (constraint-var bindings spliced onto the active
instance-method spec) now fires for `Eq[Vec]` because its body calls a
constrained-poly helper.  Plausible mechanisms, in order of suspicion:

1. The augmented composition mints additional specs (for
   `vec-eq-loop-byval` / `vec-get-byval` / `eq?` per element type) that
   change worklist iteration such that `thead`'s registration is visited
   in a state where `abi_changes` is computed false (so it is recorded
   as a carrier call instead of interned) -- mirrors the gap-4
   hamt-delete note about `emit_abi_clone_name` / carrier-call ordering.
2. A spec-name collision: the new specs mangle to a name that aliases an
   existing slot, evicting `thead`'s.
3. A fixed worklist capacity being hit earlier, dropping later
   registrations.

Pinning it needs per-registration tracing of `thead`'s binding through
`emit_abi_register_call` (does it reach the intern path or the
`emit_abi_note_carrier_call` path?) with and without the Eq Vec rewrite.

## Why it matters

This is the standing blocker for the M5 single-body-two-ABIs goal.  The
field-access half of that work (making `(.len v)` dual-ABI) is solvable
(see `docs/upcoming/m5-residual-straddle-retirement.md`), but it is moot
until the Eq Vec instance body can call a by-value helper *without*
perturbing sibling specs.  The same worklist fragility is the recurring
theme behind gap 4's hamt-delete regressor and the earlier session
reverts -- it deserves a focused, instrumented fix to the worklist's
registration/ordering invariants rather than another local patch.

## Validation under a fix

With the Eq Vec by-value rewrite in `stdlib/vec.tur`:
`bash tests/run.sh` must intern `thead__spec`/`ttail__spec`/etc. exactly
as on the clean tree (the ~11 list/option/tuple/hrt fixtures that
regressed must pass), and `vec-of-tvec-eq-manual` must still link via the
Eq Vec carrier base.

## Related

- `docs/upcoming/m5-residual-straddle-retirement.md` -- the design map;
  findings 1-4 of the session-4 continuation.
- `docs/reported/m5-instance-spec-doesnt-propagate-constraint-var-bindings.md`
  -- gap 4 (FIXED); its hamt-delete regressor was the same worklist
  fragility, sidestepped by scoping the augmentation, not by fixing the
  worklist invariant.
