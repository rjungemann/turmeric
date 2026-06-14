---
title: M4c (per-instantiation dispatch) blocked on stdlib instance bodies that use inline-C carrier helpers
category: Codegen / ABI — monomorphization plan refinement
severity: Low. Refines `docs/upcoming/m4-typeclass-per-method-abi-plan.md`. The M4b backlinks (FnDef.owner_instance, EmitAbiSpecialization.typeclass_inst) are inert until M4c lands; this report explains why the natural M4c surgery doesn't terminate at a clean working state without first reworking the stdlib instance bodies.
description: Plan M4c says "emit one dict + singleton per observed instantiation rather than one per definstance" and "rewrite the dispatch site in emit_expr.c to drop the carrier cast." Walking through the emit code with a concrete failing fixture (emit-abi-trace / Eq Tuple2), the surgery turns out to be blocked one layer earlier: stdlib instance method bodies for parameterized types call inline-C carrier helpers (e.g. tuple2-eq-carrier? from stdlib/tuple.tur) whose C bodies hard-code `Tuple2 *p = (Tuple2 *)(intptr_t)x` — they require the int64 handle the bridge spills to. Re-elaborating the method body with a by-value `Tuple2__int__int` parameter doesn't recover, because the inline-C helper still expects an int64. M4c can't safely rewrite the dispatch site to call the spec without first rewriting these stdlib instance bodies to be ABI-agnostic (i.e. take by-value structs and use `(.fst x)` / `(.snd x)` directly).
status: RESOLVED 2026-06-13 (same session). M4c-pre shipped: the 3 stdlib instance bodies (Eq Tuple2 at `stdlib/tuple.tur:473`, Eq Pair at `stdlib/pair.tur:111`, Eq Result at `stdlib/result.tur:239`) now consult their by-value fields directly via the recursive Eq A / Eq B dispatch, with no detour through the `(tuple2|pair)-eq-carrier?` / `result-eq?` carrier helpers. Build clean, full suite at 172 FAIL — exact pre-existing baseline, zero regressions. The carrier helpers `tuple2-eq-carrier?` and `pair-eq-carrier?` are now unused (no external callers in stdlib or fixtures); `result-eq?` retains its `tests/fixtures/structural-eq` user and stays as a free helper. M4c proper (per-instantiation dict + dispatch rewrite) is now unblocked.
---

# M4c stdlib-helper dependency

## What blocks the natural surgery

The plan's M4c §4 says

> In `emit_expr.c:1313` (EX_DICT) and `emit_expr.c:1749` (the
> `(intptr_t)` cast in the indirect-call dispatch), emit the
> per-instantiation singleton's address with no cast — the slot's
> declared type matches the spec clone's signature.

For that to be safe, the spec clone's body has to actually accept the
per-instantiation arg types — i.e. for `Eq Tuple2` at `(Tuple2 int int)`,
the spec is `bool __inst_Eq_eq_qu_Tuple2__spec__bool_Tuple2__int__int(
Tuple2__int__int x, Tuple2__int__int y)` and the body operates on
by-value `Tuple2__int__int` directly.

But the Eq Tuple2 source body is

```turmeric
(definstance Eq [Tuple2]
  [(Eq A) (Eq B)]
  (eq? [x y] (tuple2-eq-carrier? x y (fn [a b] (= a b)) (fn [a b] (= a b)))))
```

`tuple2-eq-carrier?` (from `stdlib/tuple.tur`) is inline-C that hard-casts
its first two args back through int64:

```c
Tuple2 *p1 = (Tuple2 *)(intptr_t)x;
Tuple2 *p2 = (Tuple2 *)(intptr_t)y;
…
```

Re-elaborating the `eq?` body with x/y typed as `Tuple2__int__int` (by-value)
doesn't help — when emit reaches the call to `tuple2-eq-carrier?`, the inline-C
helper still declares `int64_t x` in its signature and the call must pass the
int64 handle the helper expects. We're back to needing the spill.

The same shape blocks every parameterized non-HKT instance whose body
delegates to a carrier-helper:

## Stdlib bodies to rewrite

| Instance | Stdlib file | Helper used |
|---|---|---|
| `Eq Tuple2` | `stdlib/tuple.tur:475` | `tuple2-eq-carrier?` |
| `Eq Pair` | `stdlib/pair.tur:111` | `pair-eq-carrier?` |
| `Eq Result` | `stdlib/result.tur:239` | `result-eq-carrier?` |

(I.e., every `Eq` instance on a non-primitive non-HKT type. The HKT-class
instances — Functor, Monad, etc. — are explicitly out of scope for M4 and
keep the carrier ABI.)

Each rewrite is small in isolation: replace the carrier-helper inline-C call
with the direct ABI-agnostic body, e.g.:

```turmeric
;; before
(definstance Eq [Tuple2]
  [(Eq A) (Eq B)]
  (eq? [x y] (tuple2-eq-carrier? x y (fn [a b] (= a b)) (fn [a b] (= a b)))))

;; after (M4c-pre)
(definstance Eq [Tuple2]
  [(Eq A) (Eq B)]
  (eq? [x y]
    (and (eq? (.fst x) (.fst y)) (eq? (.snd x) (.snd y)))))
```

The recursive `(eq? (.fst x) …)` dispatches through the appropriate Eq A
instance — that's exactly the constraint mechanism the `[(Eq A) (Eq B)]`
context declares.

Why this works: the body now consults *only* the by-value struct's fields
via the structural projection, never the int64 handle. When the method is
specialized per instantiation, `x` / `y` are typed as the concrete
`Tuple2__int__int` and `.fst` / `.snd` work on the by-value struct.

## Why I'm reporting instead of executing

The rewrite itself is short. The chain effect isn't:

1. Every stdlib instance using a carrier-helper has to change.
2. The carrier helpers (`tuple2-eq-carrier?`, `pair-eq-carrier?`,
   `result-eq-carrier?`) become dead code once removed — and the
   `tests/fixtures/*-carrier*` fixtures that exercise the helper paths
   need attention (some may delete; some adapt).
3. Every fixture whose `expected.c` snapshot mentions `dict_Eq_Tuple2`,
   `__inst_Eq_eq_qu_Tuple2`, or the carrier-helper call regenerates
   — empirically ~50-100 snapshots based on a quick grep.
4. Polymorphic functions over `Eq A` (the *constraint-polymorphism* case)
   still expect `void *` dicts at the C level — that's **Plan M5** and it
   was explicitly listed as the sibling of M4 in the plan doc's "Risks"
   section. M4c without M5 leaves constrained generics broken.

Concretely: M4c-pre (the stdlib rewrite) is one focused day. M4c proper
(per-instantiation dict + dispatch + the M5 sibling) is the multi-session
push the plan originally estimated. Doing M4c-pre in isolation regresses
the suite (the carrier-helper fixtures fail) without yet enabling the
typed-dispatch path — net regression for one turn.

## Recommended sequence

| Phase | What | Cost | Suite delta |
|---|---|---|---|
| **M4c-pre** | Rewrite the 3 stdlib instance bodies + delete their carrier helpers | ~1 session | Temporarily worse (carrier-helper fixtures fail) |
| **M4c** | Per-instantiation dict + dispatch rewrite | ~1-2 sessions | Recovers + retires bridge for non-HKT |
| **M5** | Polymorphic functions over a dict argument get monomorphized | ~2-3 sessions per the plan | Recovers constrained-generic fixtures |
| **M4d / M3** | Final bridge delete after re-audit | ~½ session | At-baseline |

Splitting M4c-pre into a separate landing-with-regressions turn is the
pivotal call. The alternative — do M4c-pre + M4c together in one turn —
is genuinely 1-2 sessions of careful work, which exceeds a single turn's
safe scope given the fixture churn and ABI subtleties.

## What landed this turn (M4c)

Nothing semantic. M4b's backlinks remain in place (`FnDef.owner_instance`
populated in `elab_typeclasses.c:2611`; `EmitAbiSpecialization.typeclass_inst`
populated in `emit_module.c:697-717`). M4c surgery deferred until M4c-pre
unblocks it.

Suite: 172 FAIL — exact pre-existing baseline. No code change. The M4b
infrastructure stays inert until M4c-pre + M4c land together.

## Related

- [docs/upcoming/m4-typeclass-per-method-abi-plan.md](../upcoming/m4-typeclass-per-method-abi-plan.md)
  — the parent plan (now needs an M4c-pre phase prepended).
- [docs/upcoming/m4b-handoff.md](../upcoming/m4b-handoff.md)
  — M4b's deliverable.
- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the M3 deletion blocker that this whole chain unblocks.
- `stdlib/tuple.tur:473-475` — `Eq Tuple2` instance using carrier helper.
- `stdlib/pair.tur:111` — `Eq Pair` instance using carrier helper.
- `stdlib/result.tur:239` — `Eq Result` instance using carrier helper.
