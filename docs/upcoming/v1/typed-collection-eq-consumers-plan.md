---
title: Typed Eq[Map] / Eq[Set] / Eq[Cons] Consumers -- Plan (for review)
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: Create typed Map/Set/Cons consumers so the producer-typing slice has somewhere to land. The M4 series shipped per-instance specs for direct dispatch but left collection-Eq instances on the int64 carrier because their bodies bottom out in inline-C helpers (`map-eq?`, `set-eq?`, `mutmap-eq?`, `list-eq?`) that iterate opaque heap memory. The archived M4 final-state explicitly calls rewriting those helpers "a separate language-design effort, not M4 work" -- this doc tracks that effort so it doesn't fall out of sight. Direct prerequisite of `map-set-typed-pointer-producer-slice-plan.md`.
---

# Typed Eq[Map] / Eq[Set] / Eq[Cons] Consumers -- Plan

## Why this exists

The Vec slice of end-to-end monomorphization (commit `600e859`,
`6f381cc4`, #400) is complete: `Eq[Vec]` dispatches via a per-instantiation
spec, producers return typed pointers, and the audit floor (34/10) is all
permanent by-design boundaries.

`Eq[Map]` (`stdlib/map.tur:805`) and `Eq[Set]` (`stdlib/set.tur:392`)
stayed on the int64 carrier because their bodies call inline-C helpers
(`map-eq?`, `set-eq?`, `mutmap-eq?`) that iterate opaque heap memory via
an `int64_t (...)` signature. `Eq[Cons]` is in the same shape (see
`docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md`
"Probed alternative: stdlib Eq Cons rewrite is blocked on a language
gap").

**This is the gating condition for
[map-set-typed-pointer-producer-slice-plan.md](map-set-typed-pointer-producer-slice-plan.md).**
That plan defers its work because Map/Set have zero `(Map__* *)(intptr_t)`
/ `(Set__* *)(intptr_t)` consumers in any fixture (verified 2026-06-25),
so typing the producers buys nothing. Typed consumers only appear once
`Eq[Map]` / `Eq[Set]` are rewritten to project receivers by value.

## What "typed consumer" means here

The Path-A pattern that worked for `Eq[Option]`, `Eq[Tuple2]`, `Eq[Pair]`,
`Eq[Result]` (commit `aca5bdea` and the M4c cascade) -- a pure-Turmeric
instance body that projects fields directly:

```turmeric
;; what landed for Option:
(definstance Eq [Option] [(Eq A)]
  (eq? [x y] (and (= (.is-some x) (.is-some y))
                  (or (not (.is-some x))
                      (eq? (.value x) (.value y))))))
```

The instance method receives by-value `Option__T`, projects `.is-some` /
`.value` directly, and the per-instantiation spec emits a clean
`bool eq_qu__spec__Option__T__(Option__T x, Option__T y)`. No carrier
spill, no inline-C helper.

For Map/Set/Cons the receivers are heap-allocated and iterated -- there's
no direct projection that walks N buckets. Path A's mechanism applies,
but the body needs an ABI-agnostic iteration primitive the inline-C
helpers currently substitute for.

## The two real blockers

### 1. Stdlib collection-helper rewrite

`map-eq?` / `set-eq?` / `mutmap-eq?` / `list-eq?` currently look like:

```turmeric
(defn map-eq? [a : int b : int] : bool
  ```c
  /* walks the HAMT, dereferencing data : ptr<void>, returns 0/1 */
  ```)
```

They have to become Turmeric bodies that iterate the receiver as a
by-value `(Map K V)` / `(Set A)` / `(Cons A)` and bottom out in already-typed
accessors (`map-get-eq-o`, `set-contains?-o`, `cons-head`, `cons-tail`).
The accessors return `:V` / `:A` / `:bool` and ride the carrier-forcing
slot rule the producer-slice plan documents, so they compose with Path
A's per-instance spec emit without rebuilding ABI scaffolding.

Open design question: do we expose an iterator/`fold` primitive that
walks the HAMT in pure Turmeric (likely an inline-C primitive that yields
one `(K, V)` per step under a typed continuation), or do we recurse on
the HAMT's internal node ADT directly? The first is less intrusive; the
second is closer to the language's structural-recursion north star.

### 2. `Eq[Cons]` clone-name / signature consistency bug

Probed in archived
[m4-final-state-bridge-still-essential-for-collection-eq.md](../../archive/m4-final-state-bridge-still-essential-for-collection-eq.md)
("Probed Eq Cons rewrite end-to-end"): with the ascribe-bridge widening
already shipped, an `Eq[Cons]` body using `(:: t (Cons int))` to recover
a typed view of the `:int` tail emits **both** a correct
`Cons__int_Cons__int` spec **and** a spurious `int64_t_int64_t` spec
whose signature and body disagree. Root cause is
`emit_abi_clone_name` -> `type_c_name` on `TY_APP` returning different
strings on different paths for the same `arg_types`.

This has to be fixed before `Eq[Cons]` can rewrite, and it likely affects
any future `Eq[<heap-tail-struct>]` shape (e.g. internal HAMT-node ADT
recursion in approach 2 above). Standalone -- doesn't depend on the
helper rewrite, can land first.

## Sequencing

1. **Fix the `emit_abi_clone_name` / `type_c_name` divergence on TY_APP**
   (the Cons blocker). Smallest piece, unblocks `Eq[Cons]` independent
   of the collection work, and prevents the same issue from biting any
   subsequent collection-Eq rewrite.
2. **Land Path A for `Eq[Cons]`** as the smallest validating rewrite:
   the tail is a single `:int` carrier handle, not a HAMT. Confirms the
   pattern under the new spec-name fix.
3. **Pick the iteration primitive** (fold-with-typed-continuation vs.
   exposed HAMT-node ADT) and write a small RFC in this doc once the
   choice is made. This is the load-bearing language-design call.
4. **Rewrite `set-eq-full`** (set is simpler than map -- single element
   type) as the first collection.
5. **Rewrite `map-eq?`** (two type parameters, see the
   `#364`-multi-param-instance caveat in the producer-slice plan).
6. **Rewrite `mutmap-eq?`** (effectively the same shape as map but the
   `mutmap-multi-param-producer-typing-blocked.md` archive notes a
   resolution that should make it straightforward).
7. **Verify typed consumers appear**: `grep -rE '\(Map__[A-Za-z0-9_]+ \*\)\(intptr_t\)' tests/fixtures/`
   becomes non-zero on fixtures like `map-of-tvec-eq`, `set-of-tvec-eq`.
8. **Unblock `map-set-typed-pointer-producer-slice-plan.md`**: with typed
   consumers present, the producer-typing slice now removes real
   crossings instead of being pure regen churn.

## Validation harness

- `bash tests/run.sh` -- gate on zero new FAIL. **Coordinate the regen
  window with the producer-slice landing**, since both will move
  Map/Set-touching snapshots; doing them in two separate windows
  doubles the churn for downstream branches.
- `TUR_M3_AUDIT=1` sweep before/after each step: target is the
  collection-Eq fixtures (`map-of-tvec-eq`, `set-of-tvec-eq`,
  `vec-eq-ascribed-multi`, `result-of-typed-eq`) dropping their
  carrier-crossing counts to permanent-by-design floor.
- `bash tests/run-turi.sh` parity: the inline-C helpers stay registered
  as `native_map_eq` / `native_set_eq` / `native_mutmap_eq` for the
  interpreter; rewriting the *Turmeric* body doesn't touch the
  interpreter override path.

## Risks

- **Iteration primitive design lock-in.** The choice between
  fold-with-continuation and ADT-node-recursion shapes future
  collection work (`Show[Map]`, `Hash[Map]`, `Functor[Map]`-once-HKT-lands).
  Worth a focused RFC turn, not a snap decision inside the first rewrite.
- **HAMT structural recursion under the matrix.** Pure-Turmeric
  iteration of HAMT nodes intersects with `#NotImplemented` paths on
  the by-value side; verify with `tce3-map-cstr-val` (float-value
  carrier-forcing) and a `Map cstr cstr` fixture before the rewrite is
  considered shippable.
- **Cons spec-name fix scope.** The clone-name divergence may touch
  more than `TY_APP` -- audit `emit_abi_clone_name` callers for
  similar collapse-vs-name issues while the fix is live in mind.

## Out of scope

- HKT-class collection instances (`Functor[Map]`, `Foldable[Set]`) --
  those wait on M6/M7's HKT dispatch story per the archived
  `m4-final-state` doc's "What's worth doing next".
- M5 constrained-polymorphic dict typing -- adjacent but independent.
- `vec-eq?` (done, see #400 / `6f381cc4`).

## Related

- [map-set-typed-pointer-producer-slice-plan.md](map-set-typed-pointer-producer-slice-plan.md)
  -- the downstream plan this unblocks.
- [docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md](../../archive/m4-final-state-bridge-still-essential-for-collection-eq.md)
  -- the honest read of what M4 actually shipped vs. what remained.
- [docs/archive/m4d-typed-dict-vec-execution-plan.md](../../archive/m4d-typed-dict-vec-execution-plan.md)
  -- the Vec equivalent of this work (done).
- [docs/archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  -- original M3 deletion gate; archived but referenced by the
  producer-slice plan.
- [docs/upcoming/end-to-end-monomorphization-plan.md](../end-to-end-monomorphization-plan.md)
  -- the parent north-star doc.
