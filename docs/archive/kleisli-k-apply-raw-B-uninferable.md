---
title: `k-apply-raw [A B] [k : int x : A] : (Option B)` leaves `B` uninferable -- prerequisite 1 of the Kleisli by-value retype fails
category: Stdlib / Type inference -- Option none-as-NULL retirement (Track A, step 5)
severity: Medium. Blocks the Kleisli arrow by-value `(Option B)` retype (Phase
  1.4 / step 5 of end-to-end-monomorphization-plan). The retype's own
  "prerequisite 1" minimal repro -- pinned in
  `docs/reported/kleisli-byvalue-option-cascade.md` -- fails: the return tyvar
  `B` has no argument witness, so `.value`/`.is-some` on the result resolves to
  a bare tyvar.
status: RESOLVED 2026-06-19 -- end-to-end monomorphization is complete; the small
  residual ABI bridge is intentional and necessary. The Kleisli arrow remaining on
  the carrier ABI behind that bridge is accepted, so this prerequisite-1
  inference gap is no longer blocking work to be done -- closed with the
  step-5 retype it gated. Archived to docs/archive/. Prior status follows.
  OPEN, retype reverted to keep the suite green (1683/0). `k-apply-raw` /
  `k-apply` / `Category [Kleisli]` / `ArrowZero [Kleisli]` remain on the carrier
  ABI in `stdlib/kleisli.tur`, with the `(:: r (Option int))` bridge in `comp`
  (the PR #426 interim patch) still in place. No new ascription patches added.
---

# `k-apply-raw`'s `B` is uninferable from a `:int` carrier argument

## Resolution (2026-06-19)

End-to-end monomorphization is complete and the small residual ABI bridge is
intentional and necessary. This report was the prerequisite-1 gate for the
Kleisli by-value retype (step 5); with that retype closed (the carrier arrow
behind the bridge is accepted, not a defect), this inference gap is no longer
outstanding work. Report archived.

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

## Root cause (definitive)

Two layers, the second fatal:

1. **`k-apply-raw` arg witness.** Its inputs are `k : int` (the erased
   fat-closure carrier) and `x : A`; the return tyvar `B` appears only in the
   return, with no argument to witness it. A *typed* closure parameter
   (`k : (fn [A] (Option B))`) DOES let `B` infer at elab -- verified -- but
   then hits an inline-C-returns-carrier vs. declared-by-value codegen
   mismatch (the `return TUR_APPLY1(...)` body yields the int64 carrier while
   the declared `(Option B)` wants the struct), and instance methods cannot
   pass a typed closure anyway (see layer 2).

2. **`Category` is a kind-`*` class, so Kleisli's `A`/`B` are phantom at the
   class level.** `stdlib/arrow.tur:303` declares
   `(defclass Category [arr] (ident [] : arr) (comp [f g] : arr))`. `arr` has
   kind `*`. The Kleisli instance is `Category [Kleisli]` with
   `arr = Kleisli` -- the bare `(defopaque Kleisli [A B] :int)` carrier. So
   `comp`'s params `f`, `g` and result are all just `arr` (= the int64
   Kleisli carrier); the arrow's element types `A`/`B`/`C` are **phantom**
   and entirely absent from `comp`'s type. There is nowhere in `comp` for
   `B` to come from -- it is not a free tyvar of the method, it is erased by
   the class abstraction itself.

Recovering `B` in `comp` therefore requires the arrow category to be an **HKT
class** (a `(* -> * -> *)` `arr`, dispatched per element type) -- i.e. the M6
design pass + M7 HKT class dispatch implementation of
`docs/upcoming/end-to-end-monomorphization-plan.md` Phases 2-3. It is NOT a
local `kleisli.tur` fix and NOT independent of those phases, contrary to the
"self-contained one-PR retype" estimate in
`kleisli-byvalue-option-cascade.md`. The spec doc's prerequisite-1 gate ("if
this fails, file a report and pause") anticipated exactly this.

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

1. **HKT arrow category (the real fix).** Promote `Category`/`Arrow` to an HKT
   class whose `arr` has kind `(* -> * -> *)`, so the Kleisli instance tracks
   `A`/`B`/`C` and `comp` can witness `B`. This is exactly the M6 design pass +
   M7 HKT class dispatch implementation (Phases 2-3 of the umbrella plan); it is
   not a local `kleisli.tur` change. Until that lands, `comp` cannot thread a
   by-value `(Option B)` and the `(:: r (Option int))` interim bridge (PR #426)
   must stay.
2. **Sidestep via a `Monad [Option]` `>>=`** with a `(fn [A] (Option B))`
   continuation that witnesses `B` -- still blocked by layer 2 (the kind-`*`
   `Category` erases the element types before `>>=` is ever reached in `comp`).

Both are larger than the "single PR, ~90 lines" the cascade spec estimated; the
estimate assumed prerequisite 1 held and that `Category` carried the element
types, neither of which holds.

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
