# `match` on a parameter typed as an applied parametric ADT is rejected

**Found by:** while reducing the hkt-cata function-carrier repro
(`docs/reported/hkt-cata-function-typed-carrier-not-threaded.md`)
**Verified on:** turmeric 0.22.0, branch
`claude/turmeric-cata-function-carrier-iw7lt0` (post #487)
**Severity:** Low-Medium. Blocks the natural way to destructure a parametric
`:copy` defdata received as a parameter; the workaround (re-ascribe with `::`,
or wrap the param in a non-applied ADT) is awkward.

## Summary

Matching directly on a parameter whose declared type is an *applied* parametric
ADT -- `(Pair int)`, `(ExprF (fn [int] int))`, any `(F A)` -- fails
elaboration:

    error: match: scrutinee must be an ADT type, got app

even though the scrutinee is plainly a constructor of that ADT.

## Minimal repro

    (defdata Pair :copy [a] (P a a))

    (defn use-pair [p : (Pair int)] : int
      (match p (P x y) (+ x y)))         ;; error here

    (defn main [] : int 0)

Output:

    error: match: scrutinee must be an ADT type, got app
      (match p (P x y) (+ x y)))
             ^

The non-parametric case (`(defdata IntPair :copy (IP :int :int))`, `match` on a
`: IntPair` param) is fine; the trigger is specifically an *applied* parametric
type `(F A)` in scrutinee position, which is represented as `TY_APP`.

## Likely cause

`src/compiler/elab_structs.c`, the scrutinee-type check in the `match`
elaborator (~lines 2880-2907). The code unwraps the `TY_APP` chain to find the
base ADT:

    const Type *base = &scrutinee->type;
    while (base && base->kind == TY_APP && base->as.app.fn) base = base->as.app.fn;
    if (base && base->kind == TY_ADT && base->as.adt_.def) {
        /* valid TY_APP(... TY_ADT) chain -- nothing to patch */
    } else if (scrutinee->type.kind != TY_ADT || !scrutinee->type.as.adt_.def) {
        ... infer from first ctor pattern, else ERROR ...
    }

For a parameter typed `(Pair int)` the unwrapped `base` is not landing on a
resolved `TY_ADT`-with-def (the applied parametric type's head is not the
backing `AdtDef`), so the "valid chain" branch is skipped; the ctor-inference
branch then also does not patch the type, and the error fires. The fix is to
make the applied-parametric-ADT parameter type resolve its head to the backing
`AdtDef` (so the existing TP6 `TY_APP(TY_ADT)` branch accepts it), or to extend
the ctor-inference fallback to patch a `TY_APP`-headed scrutinee using the
constructor's owning ADT (the same `elab_lookup_ctor(...) -> cd->adt` already
used a few lines down) while preserving the concrete type arguments.

## Workaround

Re-ascribe the scrutinee through `::` to the same applied type inside the
`match`, or destructure via an intermediate `(Roll ...)`-style non-applied
wrapper ADT (as the Fix/`cata` pattern does), which lands a `TY_ADT` scrutinee.
