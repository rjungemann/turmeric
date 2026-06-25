---
title: Typed Eq[Map] / Eq[Set] / Eq[Cons] Consumers -- Plan (for review)
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: Create typed Map/Set/Cons consumers so the producer-typing slice has somewhere to land. The M4 series shipped per-instance specs for direct dispatch but left collection-Eq instances on the int64 carrier because their bodies bottom out in inline-C helpers (`map-eq?`, `set-eq?`, `mutmap-eq?`, `list-eq?`) that iterate opaque heap memory. The archived M4 final-state explicitly calls rewriting those helpers "a separate language-design effort, not M4 work" -- this doc tracks that effort so it doesn't fall out of sight. Direct prerequisite of `map-set-typed-pointer-producer-slice-plan.md`.
---

# Typed Eq[Map] / Eq[Set] / Eq[Cons] Consumers -- Plan

## COMPLETE -- 2026-06-25

**All eight steps are landed; every collection-Eq instance now has a typed
consumer.** `Eq[Cons]` (#553), `Eq[Map]` (#555), `Eq[Set]` + the full Set
producer slice (this branch), and `Eq[MutableMap]` all dispatch a concrete
receiver via a typed by-value per-instantiation spec over `Cons__A *` /
`Map__K__V *` / `Set__A *` / `MutableMap__K__V *`. The downstream
`map-set-typed-pointer-producer-slice-plan.md` is likewise complete.

The surrounding milestones this plan referenced as future gates have also
landed and are no longer pending:

- **M4 (per-method typeclass dict ABI):** landed -- non-HKT instance dicts hold
  per-instance concretely-typed function pointers and dispatch with no carrier
  result cast. See `docs/archive/m4-typeclass-per-method-abi-plan.md`.
- **M6/M7 (HKT class dispatch):** landed and on by default
  (`g_m7_hkt_enabled = true`, Option 1 full per-`(f, A)` monomorphization,
  2026-06-19). See `docs/archive/hkt-dispatch-options-tradeoff.md`. This
  unblocks (but does not itself deliver) HKT-class collection instances like
  `Functor[Map]` -- now a stdlib instance-body task, not an ABI gate.

The "Landing status" section below is the authoritative per-step record; this
banner only flags that the plan as a whole is closed and that its forward
references to M4/M6/M7 are now historical.

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

## Landing status (Phase TCE -- 2026-06-25)

Executed against the current tree; the empirical state diverges from the
"for review" framing above, so this section records what actually holds.

1. **Clone-name / signature divergence (step 1): already resolved.** The
   `emit_abi_clone_name` -> `type_c_name` TY_APP double-spec the archived
   probe described (`Cons__int_Cons__int` *and* a spurious
   `int64_t_int64_t`) no longer reproduces. Both the factored-helper form
   and the archive's exact inline-projection form
   (`(let [c1 (:: t1 (Cons A))] ...)` with `:int` params) emit clean code
   with no spurious int64 spec. The spec-promotion path was reworked
   between the archive note and now (see the realloc/`current_abi_specialization`
   and `match_bindings` handling in `emit_abi_intern_spec`). No compiler
   change was needed.

2. **`Eq[Cons]` Path A (step 2): LANDED.** `cons-eq-go [A] [(Eq A)]
   [c1 : (Cons A) c2 : (Cons A)]` (stdlib/list.tur) projects `.head`/`.tail`
   by value and recurses via `(:: tail (Cons A))`, mirroring `tlength`. The
   `Eq[Cons]` instance guards nil then forwards. This emits per-element
   specs -- `cons_eq_go__spec__bool_Cons__int___Cons__int__(Cons__int *, ...)`
   dispatching `__inst_Eq_eq_qu_int`, and a parallel `Cons__float` spec
   dispatching `__inst_Eq_eq_qu_float` -- with TCO'd goto loops and typed
   `(Cons__T *)(intptr_t)` consumers at the dispatch site. Verified int and
   float (`1.5`/`2.25` vs `7.1`) element dispatch is now *correct*; the prior
   `:int`-carrier sketch silently used integer equality for every element
   type (a latent bug for `cstr`/struct/NaN/-0.0 elements). Compiled-suite +
   `--interpret` parity both green.

3. **Iteration primitive (step 3): already chosen and shipped.** "Phase
   TCO-Eq-MapSet" (predating this doc) settled the load-bearing call in
   favour of a **fold-with-typed-continuation cursor**, not HAMT-node ADT
   recursion: `hamt/iter-alloc` + `hamt/iter-advance!` /
   `hamt/iter-cur-hash` / `hamt/iter-cur-key`, threaded by `set-eq-loop`
   (stdlib/set.tur) and `map-eq-loop` (stdlib/map.tur). The driver pattern
   (`*-eq-driver` allocates the iter box, runs the TCO'd loop, frees) is the
   reusable shape for future `Show[Map]` / `Hash[Map]`.

4-6. **`set-eq-full` / `map-eq?` / `mutmap-eq?` rewrites: landed**
   (the cursor-iteration bodies under TCO-Eq-MapSet; the by-value `(Set A)`
   receiver for `set-eq-full` completed this phase -- see item 7's Set bullet).
   All three instance bodies are pure Turmeric over the cursor; the inline-C
   `set-eq?` / `map-eq?` / `mutmap-eq-storage?` helpers stay only for direct /
   abstract-K-V callers (the same compromise `vec-eq?` made).

7. **Typed consumers (step 7): Cons / Set / MutableMap / Map all YES.**
   - `Eq[Cons]` -> `(Cons__int *)(intptr_t)` (this phase).
   - `Eq[Set]` -> `(Set__int *)(intptr_t)` (this phase). The TCO-Eq-MapSet
     rewrite made `set-eq-full` / `set-eq-driver` pure-Turmeric over the
     cursor but left them on the **int64 carrier** (`s1 : int s2 : int`), so
     `Eq[Set]` still dispatched via `set_hyeq_hyfull(int64_t, int64_t)` and no
     typed `(Set__A *)` consumer ever appeared (verified zero across all
     fixtures). Two sub-steps closed it: (a) a first pass made just
     `set-eq-full` / `set-eq-driver` take by-value `(Set A)` while keeping the
     public `set-count` / `set-hamt` on the carrier (ascribing `(:: s :int)`
     for those calls), because Set's producers were not yet typed -- flipping
     `set-count` alone regressed `set-basic` with TUR-E0001. (b) This phase
     then landed the **full Set producer slice** (see step 8): the producers
     return `(__TUR_RET__)` typed `(Set A)`, the accessors take by-value
     `(Set A)`, `Set` joined `type_is_heap_vec`, and the (a) ascriptions were
     removed. A concrete `(Set int)` now specializes to
     `__inst_Eq_eq_qu_Set__spec__bool_Set__int___Set__int__` dispatching
     `set_eq_full__spec__...(Set__int *, Set__int *)` over typed producers
     (`set_new__spec__Set__int__`, `set_add__spec__...`) and accessors
     (`set_count__spec__...`) -- a typed consumer end to end, matching
     `Eq[Map]`. New fixture `set-typed-consumer` guards it; 93 snapshots
     regenerated for the producer-typing flip. Compiled suite + `--interpret`
     parity green (the int64 carrier instances persist for HKT-headed /
     abstract-A receivers, so existing carrier callers are unaffected).
   - `Eq[MutableMap]` -> `(MutableMap__int__int *)(intptr_t)` in `mutmap-eq`
     (its producer `mutmap-new` already returns the typed spec, so the
     receiver local is concrete at the `.eq?` site).
   - `Eq[Map]` -> **now `(Map__K__V *)(intptr_t)` as well, resolved in #555.**
     The original blocker -- `Map` declared `(defstruct Map :heap [K V]
     (carrier :int))`, a parametric struct with a *single `:int` field* that
     `type_is_transparent_int_newtype` (src/compiler/types.c) treated as a
     **transparent int newtype** and lowered to `int64_t` in every C signature
     -- was a *representation* gap, not the `#364` multi-param resolution the
     sequencing above guessed. #555 ("Make Map a non-transparent heap struct
     with HAMT pointer") flipped the field to `(hamt :ptr<void>)` (matching
     `Set` / `MutableMap`), so `(Map K V)` now monomorphizes to a real
     `Map__K__V` struct. With that change, the seven heap-returning producers
     return through `__TUR_RET__`, `Map` joined the `type_is_heap_vec`
     allow-list, and the float/cstr carrier-forcing block was extended to
     recover the degenerate multi-param declared `(Map K V)` from the resolved
     slot type (the piece the fix directions had not anticipated -- otherwise
     `map-assoc-eq-o`'s `val :V` slot truncated `0.5 -> 0`, caught by
     `tce3-map-cstr-val`). Verified on `map-of-tvec-eq`:
     `(Map__int__Vec__int *)(intptr_t)` consumers now emit from the typed
     producers (`map_new`, `map_assoc_eq_o`, `tur_map_kcheck` specs). The
     remaining `__inst_Eq_eq_qu_Map(int64_t, int64_t)` HKT-head carrier
     instance is the same permanent-by-design floor `Eq[Vec]`
     (`__inst_Eq_eq_qu_Vec`) hits -- both carry the abstract HKT head as
     int64 while the per-instantiation specs use typed pointers. See
     `docs/archive/eq-map-typed-consumer-blocked-on-transparent-newtype.md`.

8. **Unblock producer-slice (step 8): fully, and Set's producer slice now
   landed too.** Cons / Set / MutableMap / Map all present typed consumers, so
   the producer-slice plan's typing of those producers removes real crossings.
   - **Map:** #555 folded the `(carrier :int)` -> `(hamt :ptr<void>)`
     representation change into the Map slice (mirroring how MutableMap's
     producer was typed); `map-set-typed-pointer-producer-slice-plan.md`'s Map
     slice is DONE (its Step 3 cites the archived report).
   - **Set:** this phase completed the Set producer slice. `Set` was already
     non-transparent (`(hamt :ptr<void>)`), so it needed no representation
     change -- only the producer/consumer flip in lockstep: the six
     heap-returning producers (`set-new` / `set-add` / `set-remove` /
     `set-union` / `set-intersect` / `set-diff`, plus the `set-add1` wrapper)
     now return through `(__TUR_RET__)(intptr_t)` and are `[A]`-polymorphic;
     the accessors (`set-count` / `set-member?` / `set-free` / `set-hamt`) and
     the Eq helpers take by-value `(Set A)`; `Set` joined the
     `type_is_heap_vec` allow-list. A concrete `(Set int)` monomorphizes to
     `Set__int *` end to end (typed producers + accessors + Eq spec). The
     element/hash slots stay `:int` carrier and `set-eq?` / `set-eq-cmp?` stay
     carrier inline-C (the `map-eq?` / `vec-eq?` compromise). The localized
     consumer-only rewrite the previous phase shipped (ascribing the carrier
     accessors) is superseded by this full slice -- the ascriptions are gone
     now that the accessors are by-value.

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
  collection work (`Show[Map]`, `Hash[Map]`, and -- now that HKT dispatch has
  landed, see the banner above -- `Functor[Map]`). Worth a focused RFC turn,
  not a snap decision inside the first rewrite.
- **HAMT structural recursion under the matrix.** Pure-Turmeric
  iteration of HAMT nodes intersects with `#NotImplemented` paths on
  the by-value side; verify with `tce3-map-cstr-val` (float-value
  carrier-forcing) and a `Map cstr cstr` fixture before the rewrite is
  considered shippable.
- **Cons spec-name fix scope.** The clone-name divergence may touch
  more than `TY_APP` -- audit `emit_abi_clone_name` callers for
  similar collapse-vs-name issues while the fix is live in mind.

## Out of scope

- HKT-class collection instances (`Functor[Map]`, `Foldable[Set]`) -- the M6/M7
  HKT dispatch story they waited on has since **landed** (Option 1, full
  per-`(f, A)` monomorphization, on by default `g_m7_hkt_enabled = true`,
  2026-06-19; see `docs/archive/hkt-dispatch-options-tradeoff.md`). Writing the
  actual `Functor[Map]` / `Foldable[Set]` instance bodies is now unblocked
  stdlib work (the "Phase 4.2" instance-body migration), not an ABI gate --
  it is still out of scope *for this Eq-consumer plan*, but no longer blocked.
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
