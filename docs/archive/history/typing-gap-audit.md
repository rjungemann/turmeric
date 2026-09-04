---
title: Advanced Typing -- Pre-/Post-v1.0.0 Gap Audit
category: Language Features
description: Audit of Turmeric's advanced type-system features, separating gaps that should block v1.0.0 from work that is correctly deferred to a later release
---

# Advanced Typing -- Pre-/Post-v1.0.0 Gap Audit

> Snapshot: `0.14.6`. This is a point-in-time audit of the advanced type
> system. The intended v1.0.0 feature set is defined in
> [advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md);
> this document records where the *implementation* diverges from that intent.

The entire advanced-typing surface is gated behind `-X` experimental flags
(`-Xgadt`, `-Xlinear`, `-Xsubstructural`, `-Xunique-types`, `-Xunion-types`,
`-Xintersection-types`, `-Xeffect-types`, `-Xcontracts`, `-Xsessions`,
`-Xdynamic-vars`). HKT/HRT and existentials are on by default (no flag).

## What is solid (shipped and exercised end-to-end)

| Feature | Evidence |
|---|---|
| GADTs + sized types | `defgadt`, skolem index refinement, exhaustiveness; `stdlib/gadt-vec.tur`; 40+ fixtures |
| Algebraic effects + row polymorphism | `defeffect`/`perform`/`handle`, fixed-point row inference, row variables, occurs-check; ~60 `effect-*` fixtures |
| Session types (binary + multi-party) | duality, equirecursive unfold, typed timeouts, HYC projection; 24 fixtures with real `expected.stdout`, incl. `session-mp-*` / `session-project-*` |
| Union / intersection / `any` | `(A | B)`, `(A & B)`, exhaustive match, in-match narrowing; `union-types-*` fixtures run (e.g. `union-types-basic` -> `99\n99`) |
| Contracts | `{ x : T | pred }`, boundary check insertion, pre/post, debug-on / release-strip / `--keep-contracts` |
| HKT/HRT + existentials | Functor/Applicative/Monad/Foldable/Traversable/Bifunctor, Rank-2 `forall`, `exists`/`pack`/`open`, kind inference, orphan checking |

> **Note on stale comments.** `elab_sessions.c:194` still says "Codegen
> deferred to SS2", but session emission actually happens in
> `elab_forms.c` / `emit_module.c`, and the multi-party + projection
> fixtures run with real stdout. Sessions are *not* unfinished; the
> comments are historical.

## Pre-v1.0.0 gaps -- incompleteness *inside* features intended to ship at 1.0

These are correctness/completeness gaps within features the rationale doc
treats as part of the 1.0 set. They are the candidates to close (or
explicitly re-scope) before a 1.0 tag.

1. **`call/cc` / `escape` are sugar stubs, not real continuation capture.**
   `elab_effects.c:1183,1230` -- they desugar to `(let [__cc_f f] (__cc_f 0))`,
   passing the continuation as the integer `0`. Full capture needs the CPS
   pass (not implemented; see the control-flow audit). Shipping a `call/cc`
   that cannot capture is a correctness landmine.

2. **`compose-handlers` is a nil placeholder.** `elab_effects.c:982` --
   elaborates to a nil-typed value, "runtime semantics TBD." Implement or
   remove from the surface before 1.0.

3. **`shift`/`shift0` result type is a placeholder** (`body->type`),
   `elab_effects.c:61,100,202`. Full type inference deferred; can mistype
   delimited-continuation programs.

4. **`any` boxing codegen + runtime `cast`/`type-of` deferred**
   (union-intersection-types-guide.md "Deferred" table). Pointer-sized `any`
   payloads (cstr/struct/ADT) have no boxing wrapper; `(cast x : T)` and
   `(type-of x)` are not emitted for them. `any` is documented as a 1.0
   feature but is only half-codegen'd. General tagged-union C emission is
   likewise listed as deferred (the cases that work reuse ADT machinery).

5. **Lifetime inference / elision is a thin stub.** `lifetime_elision.c`
   implements only elision rule 2; rules 1 and 3 are placeholders and
   collected lifetimes are not bound to parameters. `lifetimes.c` has no
   constraint solving / cycle detection. Inter-procedural borrow checking is
   `-Xlinear`-gated and minimal (`borrow_check.c:38,50`). The
   handler-borrow-capture check does run unconditionally and is fine; the
   general lifetime story is not.

6. **Multi-capture closures in HKT contexts need a manual cast workaround**
   (`docs/archive/hkt-deferred-tasks.md` section 5); its acceptance criterion
   is still unchecked.

7. **Flow-sensitive narrowing only inside `match`.** Union narrowing in `if`
   guards (`(type-of x)` tests) does not refine the branch type.

8. **Flag-graduation decision (meta).** Everything is `-X`-experimental at
   0.14.6. A 1.0 needs an explicit, recorded decision about which flags become
   default-on/stable vs. stay experimental. No such decision is recorded.

9. **Documentation drift to fix before 1.0.** The rationale doc references
   `-Xhkt` and `-Xexistentials` flags that do not exist in `main.c` (those
   features are unconditionally on). The stale "deferred to SS2" comments in
   `elab_sessions.c` should also be cleaned.

## Post-v1.0.0 gaps -- correctly deferred future work

Out of scope for 1.0 and well-justified.

- **Refinement types (SMT-backed entailment).**
  `docs/upcoming/refinement-types-plan.md` is "Not started" (RT1--RT7:
  constraint collector, SMTLIB2 encoder, libz3 driver, predicate
  propagation, WASM z3 bridge, `stdlib/refine.tur`). Best-positioned future
  feature -- syntax / runtime-check / FFI layers already exist via contracts;
  only the entailment layer is missing.

- **Dependent / Pi types.** Deferred in `post-mvp.md` and the rationale doc;
  needs dependent unification + proof erasure. Sized types + GADTs cover the
  practical demand.

- **Typeclass-system extensions:** multi-parameter type classes, superclass
  constraints (`class Eq a => Ord a`), associated types / type families,
  automatic `deriving`, functional dependencies, constraint kinds, and kind
  variables / polymorphic kinds (ground kinds only today,
  `kind_check.c:50`). Every instance is hand-written and single-param.

- **Runtime-polymorphic dictionary passing / monomorphization (`-O`)** for
  HKT -- current dispatch is compile-time first-match
  (`hkt-deferred-tasks.md` section 1); the `-O` monomorphization flag is
  documented but unimplemented.

- **Effect-system polish:** true effect *inference* (annotations currently
  required where polymorphism is not structural), named/scoped handler
  instances, and effect-handler composition (gated on item 2 above).

- **Intersection struct-field merging** (`{x:int} & {y:bool}` -> combined
  record); deferred. Intersections are typeclass-oriented today.

## Bottom line

The 1.0-intended feature graph is largely present and tested -- the strongest
pieces are GADTs, effects/rows, and sessions. Pre-1.0 risk concentrates in
control-flow completeness (`call/cc` / `escape` / `compose-handlers` stubs),
`any` codegen, and lifetime/borrow inference depth, plus the meta-tasks of
deciding flag graduation and cleaning doc/comment drift. The post-1.0 list
(refinement, dependent, typeclass extensions) is coherent and well-reasoned.

## See also

- [typing-gap-plan.md](typing-gap-plan.md) -- phased pre-1.0 plan that closes the gaps above
- [advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md)
- [control-flow-completeness-audit.md](control-flow-completeness-audit.md)
- [refinement-types-plan.md](../refinement-types-plan.md)
