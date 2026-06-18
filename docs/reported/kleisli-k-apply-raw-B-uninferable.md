---
title: `k-apply-raw [A B] [k : int x : A] : (Option B)` leaves `B` uninferable -- prerequisite 1 of the Kleisli by-value retype fails
category: Stdlib / Type inference -- Option none-as-NULL retirement (Track A, step 5)
severity: Medium. Blocks the Kleisli arrow by-value `(Option B)` retype (Phase
  1.4 / step 5 of end-to-end-monomorphization-plan). The retype's own
  "prerequisite 1" minimal repro -- pinned in
  `docs/reported/kleisli-byvalue-option-cascade.md` -- fails: the return tyvar
  `B` has no argument witness, so `.value`/`.is-some` on the result resolves to
  a bare tyvar.
status: OPEN, retype reverted to keep the suite green (1683/0). `k-apply-raw` /
  `k-apply` / `Category [Kleisli]` / `ArrowZero [Kleisli]` remain on the carrier
  ABI in `stdlib/kleisli.tur`, with the `(:: r (Option int))` bridge in `comp`
  (the PR #426 interim patch) still in place. No new ascription patches added.
---

# `k-apply-raw`'s `B` is uninferable from a `:int` carrier argument

## Repro (the cascade spec's prerequisite-1 gate)

Apply the target-shape retype from
`docs/reported/kleisli-byvalue-option-cascade.md`:

```turmeric
(defn k-apply-raw [A B] [k : int x : A] : (Option B)
  ```c return TUR_APPLY1((int64_t)k, (int64_t)x); ```)
```

Then the spec's own minimal repro fails to typecheck:

```turmeric
(let [f (fn [x : int] : (Option int) (some (* x 2)))]
  (println (.value (k-apply-raw (:: f :int) 7))))   ;; want 14
```

```
error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s),
                   first arg type tyvar
```

`(.value (k-apply-raw (:: f :int) 7))` has type `B`, a free type variable.

## Root cause

`k-apply-raw`'s only inputs are `k : int` (the erased fat-closure carrier) and
`x : A`. The return tyvar `B` -- the closure's *result* element -- appears
**only in the return position**, with no argument to witness it. `(:: f :int)`
deliberately erases the closure's type so type-erased instance-method params can
call `k-apply-raw`, which is exactly what destroys the `B` witness. This is the
same shape as the original `ne-from?` inference gap (return tyvar with no
argument witness), but here there is no typed-witness argument to add without
defeating the carrier-erased call convention that `k-apply-raw` exists to serve.

The spec doc's target shape (`k : int`, `: (Option B)`) is internally
inconsistent on inferability -- which is precisely why it pins a prerequisite-1
gate that says "if this fails, file a fresh report and pause this plan."

## Why the obvious fixes don't apply cleanly

- **Typed closure parameter** (`k : (fn [A] (Option B))`): would witness `B`,
  but instance methods (`comp`) receive their arrow params type-erased and pass
  `(:: f :int)`. A typed-closure `k-apply-raw` could not accept the erased
  carrier without re-introducing an ascription at every call -- the No-Lazy-`:int`
  defect the retype is meant to remove.
- **Witness `B` via the Kleisli handle** (`k : (Kleisli A B)`): `k-apply-raw`
  takes the raw `:int` precisely so the type-erased instance-method carrier can
  reach it; `k-apply` already wraps the typed `(Kleisli A B)` entry. Threading
  `(Kleisli A B)` into `k-apply-raw` would require the instance methods to
  reconstruct the typed handle from the erased carrier -- again an ascription.

## Proposed fix directions

1. **Recover `B` through the typed `Kleisli A B` handle end-to-end.** Make the
   instance methods carry the typed `(Kleisli A B)` (not the erased `:int`) so
   `k-apply` (typed) is the only entry and `k-apply-raw` is dropped or made
   private with `B` supplied by the caller's spec. Requires the typeclass
   instance machinery to preserve the arrow's element types through `comp`/
   `ident` rather than erasing to `:int`.
2. **A `Monad [Option]` `>>=` surface** (named as the natural successor in the
   cascade spec's "out of scope"): express `comp` as `\x -> f x >>= g` over a
   by-value `(Option B)` bind whose signature witnesses `B` from the `(fn [A]
   (Option B))` continuation, sidestepping the bare `k-apply-raw` entirely.

Either is a larger change than the "single PR, ~90 lines" the cascade spec
estimated; the estimate assumed prerequisite 1 held.

## Validation for a fix

- The prerequisite-1 repro above prints `14`.
- `stdlib/kleisli.tur` `comp` body uses `.is-some`/`.value` with no
  `(:: r (Option int))` ascription; `ident`/`zero-arrow` construct
  `(some x)`/`(none)` directly.
- A new `tests/fixtures/kleisli-non-int-element/` round-trips a `(Kleisli int
  float)` payload without width mis-cast.
- Full suite green; step 5 struck from `option-consumer-retype-byvalue.md`.

## Related

- `docs/reported/kleisli-byvalue-option-cascade.md` (the retype plan; this
  report is its prerequisite-1 failure).
- `docs/reported/option-consumer-retype-byvalue.md` step 5.
- `docs/archive/ne-from-byvalue-option-nonempty-element-type-uninferable.md`
  (the sibling return-tyvar inference gap, closed with a typed-witness argument
  that is NOT available here).
- `stdlib/kleisli.tur` (`k-apply-raw` / `k-apply` / `Category`/`ArrowZero`).
