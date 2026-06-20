---
title: Phase 4.1 -- carrier-helper surface inventory (which inline-C helpers back a typeclass dispatch, and the rewrite verdict per helper)
category: Planning -- ABI / Codegen rework (M9 prerequisite)
description: The Phase 4.1 deliverable of end-to-end-monomorphization-plan.md. Enumerates every stdlib inline-C helper that operates on the int64 carrier and could back a typeclass method, identifies its consuming instance (if any), and gives the rewrite/keep verdict.
---

# Phase 4.1 -- carrier-helper surface inventory

**Snapshot:** 2026-06-18. Grep basis:
`grep -rn 'tur_is_some|tur_opt_value|vec_eq|map_eq|set_eq|list_eq|mutmap_eq' stdlib/`
plus a sweep of every `defn *-eq?` and the `Eq`/HKT `definstance` bodies.

## Headline finding

**The non-HKT `Eq` instances no longer bottom out in an inline-C carrier
helper.** `Eq [Vec]`, `Eq [Map]`, `Eq [Cons]`, `Eq [Option]`, `Eq [Result]`,
`Eq [Tuple*]`, `Eq [Pair]` were already migrated (the M4c "consult by-value
fields directly" rewrite) to read `.is-some`/`.value`/`vec-len`+`vec-get`/
`map-count`+... directly. The inline-C `*-eq?` helpers that remain are
**standalone public API** called by user code / fixtures, NOT typeclass
dispatch targets -- so they do not force a dispatch-time carrier bridge.

Consequently the **only** remaining carrier-essential *dispatch* surface is:

1. **HKT instance method bodies** (`fmap`/`ap`/`bind`/`pure`/`alt-or`/`bimap`/
   `foldl`/`foldr` for Option, Result, Either, Parser, Goal, Backtrack, Schema,
   Cons, rc, identity, pair) -- all inline-C on the raw carrier. **Gated on
   Phase 3.0** (the method body has no element-type tyvar; see the plan). This
   is the bulk of the residual bridge surface.
2. **Genuinely runtime-erased helpers** -- `set-eq?` / `set-eq-full`,
   `map-eq-raw?`, the hamt iteration helpers: they walk a heterogeneous HAMT
   via `tur_hamt_iter_*` with no element type available. **Carrier-essential**
   by nature; keep inline-C with a NOTE.

## Per-helper verdict

| Helper | Body | Backs an instance? | Verdict |
|---|---|---|---|
| `option-eq?` | pure-Turmeric (by-value `(Option A)`) | no (Eq[Option] reads fields) | ✅ done |
| `option-map` | pure-Turmeric by-value | Functor[Option] would, but the instance is still inline-C (3.0) | ✅ helper done; instance gated on 3.0 |
| `result-map` | pure-Turmeric by-value (2026-06-18) | no | ✅ done |
| `list-eq?` | pure-Turmeric | Eq[Cons] delegates to it | ✅ done (already pure) |
| `mutmap-eq?` | pure-Turmeric (delegates to `-storage?`) | Eq[MutableMap] | ✅ done |
| `map-eq?` | pure-Turmeric (delegates to `map-eq-raw?`) | no (Eq[Map] reads fields) | ✅ wrapper done; `map-eq-raw?` is carrier-essential (hamt) |
| `vec-eq?` | inline-C carrier (`{int64_t* data;...}`) | **no** (Eq[Vec] uses `vec-eq-loop`, pure) | standalone public API; `v1:int` is a No-Lazy-`:int` smell, retypeable to `[A] [(Vec A) (Vec A) ^fat (fn [A A] bool)]` but cascades to fixtures; NOT dispatch-backing |
| `slice-eq?` | inline-C carrier | no (Eq via pure loop) | same as `vec-eq?` |
| `result-eq?` | inline-C carrier | no (Eq[Result] reads fields) | standalone public API; retypeable, cascades to fixtures |
| `set-eq?` / `set-eq-full` | inline-C hamt iteration | Eq[Set] -> `set-eq-full` | **carrier-essential** (runtime-erased HAMT) -- keep, annotate |
| `map-eq-raw?` | inline-C hamt | via `map-eq?` | **carrier-essential** (HAMT) -- keep, annotate |
| `unwrap-or` | inline-C carrier | no | retypeable to by-value `[A] [(Option A) A] : A`, but its only stdlib caller is the HKT-gated kleisli `comp` (see `unwrap-or-byvalue-cascade.md`) |
| all HKT method bodies (`fmap`/`bind`/...) | inline-C carrier | **yes -- the dispatch target** | **gated on Phase 3.0** |

## What this means for Phases 4-5

- The "rewrite each carrier helper" worry (M9 blocker) is **already resolved
  for non-HKT typeclass dispatch** -- those instances read by-value fields.
- The remaining dispatch-backing carrier surface is **entirely the HKT
  instance bodies**, which are gated on the Phase 3.0 elaborator change (HKT
  methods must receive the element type). So Phase 4.2's dispatch-relevant
  subset collapses into Phase 3, and the standalone `*-eq?`/`unwrap-or`
  retypes are independent API-hygiene cleanups (No-Lazy-`:int`) that cascade
  only to their direct callers, not to dispatch.
- The **carrier-essential** set is small and well-defined: the HAMT iteration
  helpers (`set-eq-full`, `map-eq-raw?`, and the `tur_hamt_*` externs). These
  are the legitimate `;;` NOTE / "carrier-essential" entries for
  `docs/monomorphization-audit.md` (Phase 4.3).

## Recommended Phase 4/5 sequencing (revised)

1. Land Phase 3.0 (HKT method element-type threading) -- unblocks the HKT
   instance-body rewrites, which ARE the dispatch-backing carrier surface.
2. Rewrite the HKT instance bodies pure-Turmeric (Phase 4.2 HKT subset).
3. Annotate `set-eq-full` / `map-eq-raw?` / hamt externs as carrier-essential
   in `docs/monomorphization-audit.md` (Phase 4.3) -- they never become
   by-value (heterogeneous runtime data).
4. Independently (any time): retype the standalone `vec-eq?` / `slice-eq?` /
   `result-eq?` / `unwrap-or` public helpers off `:int` (API hygiene; cascades
   only to direct callers).
5. Then Phase 5 (tighten the bridge predicate to fire only at the
   carrier-essential hamt sites; delete the rest).

## Cross-references

- `docs/archive/end-to-end-monomorphization-plan-2.md` Phases 3.0, 4, 5.
- `docs/upcoming/v2/hkt-dispatch-options-tradeoff.md` (Phase 2 decision).
- `docs/archive/kleisli-k-apply-raw-B-uninferable.md`,
  `docs/archive/unwrap-or-byvalue-cascade.md`.
