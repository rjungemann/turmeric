---
title: M5 scope audit -- what already landed, what genuinely remains
category: Planning -- ABI / Codegen rework
description: M5 of `end-to-end-monomorphization-plan.md` ("polymorphic functions over a dict argument get monomorphized") was scoped on 2026-06-13. Since then, M4c Path A landed (PRs #399-#416) and substantively delivered M5's core behaviors. This audit pins the empirical state, identifies the remaining gap, and proposes a precise re-scope.
---

# M5 scope audit -- 2026-06-18

## TL;DR

Per-call-site monomorphization of **constrained-polymorphic defns** is
already in place after M4c landed. Specs are emitted with natural C arg
types and inline-dispatch the right typeclass instance per `A`. The
plan-doc M5 framing ("receives the dict as `void *` and casts it") no
longer matches reality -- there is no dict passed at all in the emitted
spec bodies; instead the instance method is hardwired.

The one genuine remaining M5-class gap is reported separately under
[`poly-hof-constrained-arg-baked-carrier`](../reported/poly-hof-constrained-arg-baked-carrier.md):
a polymorphic HOF that takes a constrained-poly-as-value does not
monomorphize per-`A` at its call sites; it stays in the carrier ABI and
emits type-mismatched C.

## What already landed (M4c-as-M5)

Built and verified with `build/tur` at `claude/sleepy-hamilton-mnvm6b`
(post-PR #421, 2026-06-18).

### Direct constrained-poly defn, concrete-A call site

`tests/fixtures/cgi-constrained-generic-dispatch/input.tur` exercises
`(defn geq [^Eq K a : K b : K] : bool (eq? a b))` called with
`int`, `bool`, `cstr`. Emit:

```c
static bool geq(int64_t a, int64_t b)               { return __inst_Eq_eq_qu_int(a, b); }
static bool geq__spec__bool_const_char___const_char__(const char *a, const char *b)
                                                     { return __inst_Eq_eq_qu_cstr(a, b); }
```

The base clone covers `K=int` (representative); per-`K` specs are
emitted for the other concrete sites. **No dict is passed -- the
instance method is hardwired in each clone.**

### Cascading constrained-poly (outer calls inner with same tyvar)

```turmeric
(defn inner [^Eq A] [a : A b : A] : bool (eq? a b))
(defn outer [^Eq A] [x : A y : A] : bool (inner x y))
```

Emit:

```c
static bool outer__spec__bool_const_char___const_char__(const char *x, const char *y) {
    return inner__spec__bool_const_char___const_char__(x, y);
}
static bool inner__spec__bool_const_char___const_char__(const char *a, const char *b) {
    return __inst_Eq_eq_qu_cstr(a, b);
}
```

Constraint propagation through nested specs works.

### Constrained-poly value-aliased to a let

`tests/fixtures/gde6-generic-dict-alias-call/input.tur` and probe2
(local repro):

```turmeric
(let [g count-it] (g (make-struct Box 0)))   ; -> 7  (Size [Box] dispatch)
```

PR #386's `elab_call.c` change ("constrained-generic-as-value" handling)
already routes immutable-let aliases of a global through their global
binding so per-call-site spec lookup fires.

### Constrained-poly as HOF arg, HOF non-polymorphic

`(defn apply-it [f : (fn [Box] int) b : Box] : int (f b))` called with
`apply-it count-it (Box 0)` returns 7 -- the spec for
`count-it@Box` is interned, and the HOF's concrete `(fn [Box] int)`
parameter slot accepts it directly.

### Parameterized non-HKT typeclass instances

Per `docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md`:
direct dispatch on `Eq Tuple2`, `Eq Result`, etc. emits per-instantiation
specs with by-value param types and concrete return types -- zero
crossings under `TUR_M3_AUDIT=1` for the canonical fixtures.

## What genuinely remains

### Gap 1 (proven): polymorphic HOF + constrained-poly-as-value

When the HOF is itself polymorphic over `A`
(`(defn apply-it [A] [f : (fn [A] int) a : A] ...)`), the HOF does NOT
get per-`A` specs. The emit produces one carrier-typed clone
`int64_t apply_hyit(tur_poly_fn_t, int64_t)`, and concrete-A call
sites passing `Box` / `int` mismatch the signature -- the resulting C
does not compile.

See [`docs/reported/poly-hof-constrained-arg-baked-carrier.md`](../reported/poly-hof-constrained-arg-baked-carrier.md)
for the repro, expected emit, and proposed fix direction.

### Gap 2 (not yet probed): runtime-erased `tur_poly_fn_t` consumers

The carrier ABI for `tur_poly_fn_t` legitimately exists for genuinely
type-erased first-class polymorphic values (e.g. a fn stored in a
heterogeneous map, or returned from a runtime dispatch). M8 of the
master plan handles those. **Out of M5 scope** -- M5 covers only the
sites where the consumer is statically reachable from a concrete-A
call.

### Gap 3 (deferred): HKT classes (Functor / Monad)

Explicit M6/M7 territory per the master plan. M5 does not touch
kind-`[*->*]` constraints.

## Proposed M5 re-scope

Narrow M5's deliverable to **gap 1 alone**:

> Extend the per-call-site spec-intern machinery
> (`emit_abi_intern_spec`, `find_matched_abi_spec`) so that a
> polymorphic HOF's `A`-parameterized argument and parameter slots
> participate in spec lookup when reached through a `(fn [A] ...)`-typed
> parameter. When all call sites resolve `A` to a concrete type, emit
> one HOF clone per `A` and route concrete-A constrained-poly-as-value
> arguments to their matching specs.

Estimated effort: 1-2 sessions (down from the plan-doc's 2-3 once gap 1
is the only target).

Validation:

1. New fixture `tests/fixtures/poly-hof-constrained-arg-spec/` mirroring
   the report's repro; passes end-to-end and pins the emitted spec
   names.
2. `bash tests/run.sh`: zero new `FAIL`s (audit floor preserved).
3. `TUR_M3_AUDIT=1` on the audit-floor fixtures: no regressions in the
   bridge crossing count.

## Where the plan-doc M5 framing went stale

The plan doc, written 2026-06-13, described M5 as:

> A function like `(defn fold-eq [A] [^&: Eq A] [xs : (Vec A) y : A] ...)`
> currently receives the Eq dict as `void *` and casts it. After M4, the
> dict has a per-instance C shape, so `void *` doesn't work. The fix is
> to monomorphize `fold-eq` per A at each call site.

Empirically, after M4c:

1. The constrained-poly defn does not receive a dict at all in the
   emitted code -- the instance method is hardwired into each spec.
2. The `void *` cast that does still exist in `emit_expr.c:2815` is for
   `^fat` closure-arg passing, not for typeclass-dict args (the dict
   isn't an arg in any sense).
3. Per-call-site monomorphization of `fold-eq` per `A` already fires
   for direct calls; it just doesn't fire through one layer of
   polymorphic-HOF nesting (gap 1).

The plan's framing predicted a different intermediate state than the
one M4c actually delivered.

## Recommendation

Update `parallel-tracks.md` to point Track A's M5 entry at this audit
doc and the linked report. Then either:

- (preferred) take gap 1 as the M5 deliverable -- one fixture + the
  spec-intern extension; or
- close M5 as substantively-delivered-by-M4c and treat gap 1 as a
  standalone fix tracked under its report's slug.

Either way, M6/M7 (HKT classes) becomes the next non-trivial
monomorphization milestone, gated on the HKT design pass.
