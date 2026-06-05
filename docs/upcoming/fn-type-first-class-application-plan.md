---
title: First-Class `:fn` Closure Values -- Application and Coercion
category: Planning
description: Make the `:fn` closure type (the typeclass-method continuation carrier, `tur_poly_fn_t`) a first-class callable value. Today a `:fn`-typed value can be received and forwarded but cannot be applied with `(g x)`, constructed from a lambda, or coerced to a `^fat` sink outside the typeclass-dispatch machinery -- so "is this value callable?" depends on which of four closure representations it happens to have. This plan collapses that to one predictable rule: a `:fn` value is callable, constructible, and coercible everywhere, with no representation-dependent pitfalls.
---

# First-Class `:fn` Closure Values -- Plan

> **Status:** Draft
> **Type:** compiler -- closure ABI / type system
> **Builds on (all COMPLETE):**
> - [closure-first-class-type-plan.md](../archive/closure-first-class-type-plan.md)
>   -- boxed `TY_FN` first-class closure type (Option B); made a bare `^fat g`
>   directly callable.
> - [poly-to-fat-typed-shim-plan.md](../archive/poly-to-fat-typed-shim-plan.md)
>   and #252 -- N-ary `__tur_poly_to_fat<N>` carrier; routes a typeclass-method
>   poly closure into a `^fat` sink.
> **Reports / gaps this resolves:**
> - The `:fn` direct-application gap surfaced finishing
>   `stdlib-hkt-consolidation` (parser/backtrack/logic instances) -- the
>   instance bodies still had to delegate through `^fat`-sink workers because a
>   `:fn` value is not itself callable.
> - Supersedes the still-open residue of
>   [fat-fn-param-capturing-closure-gap.md](../archive/fat-fn-param-capturing-closure-gap.md)
>   for the `:fn` spelling specifically.

---

## Problem

Turmeric has **four** runtime representations of "a thing you can call", and
whether `(g x)` type-checks depends on which one `g` happens to be:

| Representation | Spelling | Runtime value | `(g x)` works? |
|---|---|---|---|
| bare function | captureless `(fn ...)`, C fn ref | `R (*)(A...)` pointer | yes (`TY_FN`) |
| boxed closure | capturing `(fn ...)`, `^fat g` | fat box `{thunk, caps...}` ptr | yes (first-class closure work) |
| raw pointer | `:ptr<void>` | bare pointer | no -- hard error (by design) |
| **poly closure** | **`:fn`** | **`tur_poly_fn_t {env, fn}`** | **no** |

The fourth -- `:fn`, the typeclass-method continuation carrier -- is the odd one
out. It is **second-class**: the type system models it as a *def-less*
`TY_STRUCT` (it prints as `<struct>`; see `types.c:1636`), and the only code
that can construct, call, or coerce one is the typeclass-dispatch machinery
itself. Everywhere else it is an opaque blob.

### Observed behaviour (current build)

Probed directly; each row is a minimal `tur check`:

| # | Program | Result |
|---|---|---|
| P1 | `(defn applyf [g : fn x : int] : int (g x))` | **error**: `'g' is not a function or continuation` |
| P2 | `:fn` param forwarded to a `^fat` sink: `(sink g 5)` | **error**: `expected ptr<void>, got <struct>` |
| P3 | `:fn` param forwarded to another `:fn` param | ok (struct copy) |
| P4 | bare lambda passed where `:fn` is expected | **error**: `expected <struct>, got (fn [int] : int)` |
| P5 | capturing closure (`:ptr<void>`) passed where `:fn` expected | **error**: `expected <struct>, got ptr<void>` |

So a `:fn` parameter can be *declared*, *forwarded to another `:fn`*, and *read
in inline-C* (`fn.fn`, `fn.env`) -- and nothing else. You cannot **call** it
(P1), **hand it to a `^fat` worker** (P2), **build one from a lambda** (P4), or
**build one from a closure value** (P5). It only "works" inside a typeclass
instance because `definstance` + the `EX_POLY_TO_FAT` bridge special-case the
whole path end-to-end.

### Why this is a "hard-to-reason-about pitfall"

1. **Same spelling, two meanings.** A typeclass-method `:fn` param is built as
   `TYPE_PTR_VOID` + `param_is_fn` (`elab_typeclasses.c:622,678`), but a
   regular-`defn` `:fn` param is a def-less `TY_STRUCT`. The keyword means
   different things in the two contexts.
2. **Callability is non-local.** Whether `(g x)` compiles depends on how `g`
   was *declared* three lines up, not on what it *is* (a function). The author
   of `stdlib-hkt-consolidation` had to know that the only escape hatch is "wrap
   a `^fat`-sink worker and forward the `:fn` into it" -- which is exactly the
   workaround the instance bodies still carry.
3. **The machinery already exists, ungeneralised.** `elab_poly_call`
   (`elab_call.c:2950`) already emits `g.fn(g.env, args...)` for a
   `tur_poly_fn_t` -- but only when the binding is `is_poly_fn` (a *rank-2
   forall* parameter), not when it is the CCL `:fn` flavour. The call path for
   a `:fn` value is one `if` away from existing.

## Goal -- one predictable rule

> A value whose type is "a function" -- bare `TY_FN`, boxed closure, or `:fn`
> poly closure -- is **callable** with `(g x)`, **constructible** from a `(fn
> ...)` literal or a closure value, and **coercible** into any function-shaped
> parameter (`:fn`, `^fat`, or a matching `(fn [...] :R)` annotation). Only a
> raw `:ptr<void>` is non-callable, and that stays a hard error by design.

Concretely, every cell P1, P2, P4, P5 must compile and run with the obvious
semantics, and the round-trip must hold for non-`int` argument/return types
(`:float`, `:cstr`, `:ptr<T>`), not just the `int64` carrier.

## Background -- what the four-way split already cost

This is the *fourth* closure-ABI cleanup; the prior three (all landed) tell us
the shape of the fix:

- **Typed Closure Invocation ABI** -- threaded declared arg/return types to the
  C call site (retired the `int64`-only `TUR_APPLYn`).
- **Closure Representation Unification / First-Class Closure Type (Option B)**
  -- gave capturing closures one representation (boxed `TY_FN`) and made a bare
  `^fat g` directly callable. It explicitly carved out `:ptr<void>` as
  "raw pointer only".
- **N-ary poly-to-fat (#252)** -- generalised the `tur_poly_fn_t -> ^fat` box
  to any arity.

The `:fn` carrier was never folded into that first-class model because, at the
time, it only ever appeared as a typeclass-method parameter that the dispatch
machinery fully controlled. `stdlib-hkt-consolidation` is the first place a
human writes `:fn`-shaped delegation by hand, which is why the gap surfaced now.

## Root cause

Two coupled facts:

1. **`:fn` is modelled as an opaque struct, not as a function type.** A def-less
   `TY_STRUCT` (regular defn) / `TYPE_PTR_VOID + param_is_fn` (typeclass method).
   Neither carries an arrow signature, so:
   - the callee-resolution check `fn_type.kind != TY_FN && != TY_CONT`
     (`elab_call.c:1801`) rejects it (P1);
   - subtype/coercion checks see a struct, so no lambda (P4), closure (P5), or
     `^fat` target (P2) unifies with it.
2. **The poly-fn call/box paths are gated on `is_poly_fn` (rank-2), not on the
   `:fn` flavour.** `elab_poly_call` and the `EX_POLY_TO_FAT` arg-coercion both
   key off `is_poly_fn`/rank-2 detection, so they never fire for a hand-written
   `:fn` binding outside `definstance`.

## Design

### The unifying decision: give `:fn` a real arrow signature and one carrier

Make `:fn` a *spelling* of the first-class closure type rather than a distinct
opaque struct. A `:fn` value is a `tur_poly_fn_t` carrier (keep the runtime
representation -- it is already what typeclass dispatch and #252 rely on), but
its **type** becomes a `TY_FN` flavoured as a poly/boxed closure with a
signature, so the existing function-type code paths apply to it.

Two sub-decisions:

- **Signature inference.** Bare `:fn` (no arity) defaults to the unary
  `int64`-carrier signature it has today, so existing typeclass stubs are
  churn-free. Allow an explicit signature spelling -- `:fn` extends naturally to
  `(fn [A...] :R)` -- so a hand-written `:fn` param can declare real arg/return
  types and round-trip non-`int` values (reusing the Typed Closure Invocation
  ABI thunks).
- **One carrier, bidirectional coercions.** Keep `tur_poly_fn_t` as the `:fn`
  carrier but add the missing coercions so it interoperates with the boxed
  closure and `^fat` representations:
  - lambda / boxed closure / `^fat` handle **into** a `:fn` slot
    (`EX_*_TO_POLY` -- box a thunk+env into `{env, fn}`);
  - `:fn` value **into** a `^fat` sink (generalise `EX_POLY_TO_FAT` off
    `is_poly_fn` to the `:fn` flavour -- #252 already supplies the N-ary shim);
  - `:fn` value **applied** `(g x)` (reuse `elab_poly_call`'s
    `g.fn(g.env, args...)` emit).

> **Alternative considered -- retire `tur_poly_fn_t`, make `:fn` == boxed
> `TY_FN`.** Cleaner long-term (three representations collapse to two: boxed
> closure + raw pointer), but it rewrites the typeclass-method ABI and #252's
> N-ary carrier, and churns every `definstance` method's generated C. Deferred:
> this plan keeps the carrier and adds first-class behaviour around it, leaving
> the deeper merge as a possible follow-up once `:fn` is first-class and its
> uses are visible.

### The single rule, restated for the implementation

`elab_call_fn`'s callee check and the argument-coercion path each grow **one**
branch for the `:fn` flavour, mirroring what already exists for boxed `TY_FN`
and rank-2 `is_poly_fn`:

- **callee**: if the binding is a `:fn` closure, elaborate as
  `g.fn(g.env, args...)` (the `elab_poly_call` emit), typed by the declared (or
  defaulted) signature.
- **argument**: a `:fn` source coerces to a `^fat`/`(fn ...)`/`:fn` sink, and a
  lambda / boxed-closure / `^fat` source coerces *to* a `:fn` sink, via the
  box/unbox shims.

## Phasing (each phase ends suite-green)

1. **F1 -- unify the type.** Represent `:fn` as a `TY_FN` flavoured
   "poly/boxed closure, unary `int64` default" instead of a def-less
   `TY_STRUCT`, in **both** the regular-defn and typeclass-method parse paths
   (`elab_fns.c`, `elab_typeclasses.c:622,678`). No behaviour change intended;
   the point is that `:fn` now carries an arrow signature. Snapshot churn is the
   risk -- regenerate fixtures. *Exit:* P3 still works; suite green.
2. **F2 -- direct application (P1).** In `elab_call_fn`, route a call through a
   `:fn`-flavoured binding into the `elab_poly_call` emit
   (`g.fn(g.env, args...)`). *Exit:* P1 prints the expected value for `:int`.
3. **F3 -- `:fn` into a `^fat` sink (P2).** Generalise the `EX_POLY_TO_FAT`
   argument coercion to fire for the `:fn` flavour, not only rank-2
   `is_poly_fn`. The N-ary shim from #252 is reused unchanged. *Exit:* P2
   compiles/runs; the `stdlib-hkt-consolidation` `^fat`-sink wrappers become
   removable (follow-up).
4. **F4 -- construct a `:fn` from a lambda / closure (P4, P5).** Add the inverse
   coercion `EX_FN_TO_POLY` / `EX_FAT_TO_POLY`: at a `:fn` parameter, box a
   `(fn ...)` literal's thunk+env, or an existing boxed-closure/`^fat` handle,
   into `{env, fn}`. *Exit:* P4 and P5 compile/run.
5. **F5 -- typed signatures + non-int round-trip.** Allow `:fn` to carry an
   explicit `(fn [A...] :R)` signature and thread arg/return types through the
   poly carrier using the Typed Closure Invocation ABI thunks, so `:float` /
   `:cstr` / `:ptr<T>` arguments and returns survive. *Exit:* round-trip
   fixtures for `:float`/`:cstr` pass.
6. **F6 -- de-workaround the consumer.** Drop the now-unnecessary `^fat`-sink
   wrappers (`bind-parser-fat`, `mbind-fat`, `bind-goal-raw`'s `^fat` param,
   etc.) where a direct `:fn` application is now legal, validating the cleanup
   end-to-end against the existing `hkt-stdlib-*-instances` fixtures.
   **Split out into its own plan** (it hinges on a distinct capability --
   captured-and-deferred `:fn` application -- not exercised by F1-F4):
   [fn-first-class-stdlib-deworkaround-plan.md](fn-first-class-stdlib-deworkaround-plan.md).

## Risks

- **Snapshot churn (F1).** Changing how `:fn` types/prints will move some
  `expected.c` (and possibly diagnostic) snapshots. Follow the `CLAUDE.md`
  regeneration recipe; the change must be behaviour-preserving in F1.
- **Silent miscompile if a coercion picks the wrong shim arity.** The #252
  family is arity-tagged; F3/F4 must select the shim from the *declared*
  signature, never a defaulted unary one, when a typed `:fn` is in play. Guard
  with multi-arg round-trip fixtures (mirror `poly-to-fat-multiarg-roundtrip`).
- **Overlap with rank-2 `is_poly_fn`.** Both flavours share the
  `tur_poly_fn_t` carrier and the `elab_poly_call` emit; keep them distinguished
  at the type level so a rank-2 forall parameter does not silently accept a
  monomorphic `:fn` (or vice-versa) without the intended polymorphism check.
- **Linearity / effect rows.** A `:fn` that closes over a linear handle must not
  launder it; the coercions should preserve whatever the boxed-closure path
  already enforces, not bypass it.

## Validation

- A `tests/fixtures/fn-first-class-application/` fixture covering P1-P5 with
  `int`, plus a `*-typed` variant covering `:float`/`:cstr` round-trips (F5).
- A negative fixture asserting a raw `:ptr<void>` is **still** not callable (the
  invariant carve-out must survive).
- The `hkt-stdlib-{parser,backtrack,logic}-instances` fixtures keep passing
  after F6 removes the `^fat`-sink wrappers (behaviour identical; the instance
  bodies just call `:fn` continuations directly).
- `bash tests/run.sh` green at every phase boundary.

## Out of scope

- Retiring `tur_poly_fn_t` in favour of the boxed `TY_FN` carrier (the
  "alternative considered" above) -- a possible follow-up once `:fn` is
  first-class.
- Currying / partial application of `:fn` values beyond what the existing
  closure machinery already provides.
- Variadic `:fn` (`& rest`) continuations.

## Cross-references

- [closure-first-class-type-plan.md](../archive/closure-first-class-type-plan.md)
  -- the boxed-`TY_FN` model this extends to the `:fn` flavour.
- [poly-to-fat-typed-shim-plan.md](../archive/poly-to-fat-typed-shim-plan.md)
  + #252 -- the N-ary `__tur_poly_to_fat<N>` carrier F3 reuses.
- [stdlib-hkt-consolidation-plan.md](stdlib-hkt-consolidation-plan.md) -- the
  consumer whose instance bodies motivated this; F6 closes the loop.
