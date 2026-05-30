---
title: Advanced Typing -- Pre-v1.0.0 Gap-Closure Plan
category: Language Features
description: Phased implementation plan that closes (or explicitly re-scopes) the pre-v1.0.0 advanced-typing gaps identified in the typing-gap audit
---

# Advanced Typing -- Pre-v1.0.0 Gap-Closure Plan

> **Status:** Not started. Companion to
> [typing-gap-audit.md](typing-gap-audit.md); every phase maps to a numbered
> item in that audit's "Pre-v1.0.0 gaps" section. Post-1.0 work (refinement
> types, dependent/Pi types, typeclass-system extensions, `-O`
> monomorphization, intersection field-merging) is out of scope here -- see
> the audit's "Post-v1.0.0 gaps".
>
> **Snapshot:** `0.14.6`.
>
> **Shared items.** Audit items 1--3 (`call/cc`/`escape`, `compose-handlers`,
> `shift`/`shift0` result typing) are continuation-flavored and are owned by
> the [control-flow-completeness-plan.md](control-flow-completeness-plan.md)
> (phases CF4, CF3, CF2). This plan references them rather than duplicating
> them -- see Phase TY6.
>
> **Last updated:** 2026-05-30

---

## Motivation

The 1.0-intended advanced-typing graph (GADTs, effects/rows, sessions,
union/intersection, contracts, HKT/HRT, existentials) is largely present and
tested. Pre-1.0 risk is concentrated in: `any` that is only half-codegen'd,
lifetime/borrow inference that is a thin stub, narrowing that only works
inside `match`, and two meta-tasks -- recording a flag-graduation decision and
cleaning documentation/comment drift. This plan sequences those into phases
with done-criteria so a 1.0 tag reflects what is actually implemented.

Goals:

- Make every feature the rationale doc lists as "1.0" either fully work or be
  honestly documented as restricted.
- Record an explicit, reviewable decision about which `-X` flags graduate to
  default-on for 1.0.
- Remove documentation/comment drift that misdescribes shipped behavior.

Non-goals (deferred, tracked in the audit):

- Refinement types (SMT entailment), dependent/Pi types.
- Multi-parameter type classes, superclasses, associated types, deriving,
  functional dependencies, kind polymorphism.
- `-O` monomorphization / runtime dictionary passing for HKT.
- Effect inference, named/scoped handler instances.
- Intersection struct-field merging.

---

## Phase ordering at a glance

| Phase | Audit item | Disposition | Why this order |
|---|---|---|---|
| TY0 | 9 | Cleanup | Doc/comment drift; cheap, unblocks honest flag decision |
| TY1 | 8 | Decision | Flag-graduation matrix; gates what "1.0 typing" means |
| TY2 | 4 | Implement | `any` boxing codegen + `cast`/`type-of` |
| TY3 | 7 | Implement | Flow-sensitive narrowing in `if` guards |
| TY4 | 5 | Implement/scope | Lifetime inference / elision depth |
| TY5 | 6 | Implement | Multi-capture closures in HKT (remove manual cast) |
| TY6 | 1,2,3 | Cross-ref | Shared continuation-typing items (see control-flow plan) |

---

## Phase TY0 -- Documentation and comment drift (audit item 9)

Cheap, no semantics change; do first so the flag-graduation decision (TY1) is
made against accurate docs.

- **TY0.1** Remove references to non-existent `-Xhkt` / `-Xexistentials` flags
  from `advanced-type-system-rationale.md` (those features are
  unconditionally on). *Done when:* a search for `-Xhkt`/`-Xexistentials`
  returns only intentional "not a flag" notes, if any.
- **TY0.2** Clean the stale "Codegen deferred to SS2" comments in
  `elab_sessions.c` (sessions actually emit and run with real stdout).
  *Done when:* the stale comments are gone or corrected to reflect that
  emission happens in the forms/module emitter.
- **TY0.3** Sweep for other historical "deferred"/"TBD" comments in the
  advanced-typing path that contradict shipped, tested behavior; correct or
  remove. *Done when:* no comment in the touched files claims a shipped
  feature is unimplemented.
- **TY0.4** No fixture snapshots should change (comment/doc only); confirm
  `tests/run.sh` is still green. *Done when:* zero `FAIL` lines.

---

## Phase TY1 -- Flag-graduation decision (audit item 8)

Everything advanced is `-X`-experimental at 0.14.6. A 1.0 needs an explicit,
recorded decision per flag: default-on/stable vs. stays experimental.

- **TY1.1** Enumerate every advanced-typing flag (`-Xgadt`, `-Xlinear`,
  `-Xsubstructural`, `-Xunique-types`, `-Xunion-types`,
  `-Xintersection-types`, `-Xeffect-types`, `-Xcontracts`, `-Xsessions`,
  `-Xdynamic-vars`) plus the already-default HKT/HRT/existentials. *Done when:*
  a table lists each with its current state and fixture coverage.
- **TY1.2** For each flag, record a 1.0 disposition (graduate / stay
  experimental) with a one-line rationale grounded in this audit (e.g. a flag
  whose feature has an open pre-1.0 gap should not graduate until the gap
  closes). *Done when:* the table has a disposition column with rationale.
- **TY1.3** Identify cross-dependencies (a flag that, if graduated, implies a
  pre-1.0 gap must close first -- e.g. `-Xlinear` vs. TY4 lifetimes). *Done
  when:* dependencies are noted so sequencing is explicit.
- **TY1.4** Get the matrix ratified and link it from the 1.0 milestone; this
  doc becomes the source of truth. *Done when:* maintainers sign off and the
  table is referenced from the milestone.

> Note: TY1 produces a *decision*, not code. Any actual default-on flips that
> follow are tracked as their own follow-ups once the gating gaps close.

---

## Phase TY2 -- `any` boxing codegen + `cast` / `type-of` (audit item 4)

`any` is documented as a 1.0 feature but only half-codegen'd: pointer-sized
payloads (cstr/struct/ADT) have no boxing wrapper, and `(cast x : T)` /
`(type-of x)` are not emitted for them.

- **TY2.1** Specify the `any` boxing representation for pointer-sized payloads
  (tag + payload) and how it coexists with the cases that currently reuse ADT
  machinery. *Done when:* the representation is written down with the tag
  scheme.
- **TY2.2** Emit the boxing wrapper when a cstr/struct/ADT value is widened to
  `any`. *Done when:* widening such a value compiles and round-trips.
- **TY2.3** Emit `(cast x : T)` as a checked downcast against the box tag
  (with the agreed failure behavior on tag mismatch). *Done when:* a correct
  cast returns the value and a wrong cast fails per the agreed behavior.
- **TY2.4** Emit `(type-of x)` for boxed `any`. *Done when:* `type-of` returns
  the stored tag's type for each supported payload kind.
- **TY2.5** Decide 1.0 scope for general tagged-union C emission: implement, or
  document the supported subset and reject the rest with a clear diagnostic.
  *Done when:* the scope is recorded and unsupported cases fail loudly rather
  than mis-emitting.
- **TY2.6** Fixtures: box/unbox round-trip per payload kind (cstr/struct/ADT),
  correct cast, failing cast, `type-of` on each. *Done when:* all green and
  snapshotted; the union-intersection guide's "Deferred" table is updated to
  match what now ships.

---

## Phase TY3 -- Flow-sensitive narrowing in `if` guards (audit item 7)

Union narrowing works inside `match` but not in `if` guards: a `(type-of x)`
test in an `if` condition does not refine the branch type.

- **TY3.1** Define the narrowing rule for `if`: a `type-of`/type-test guard in
  the condition refines `x` to the tested type in the then-branch (and to the
  complement, where representable, in the else-branch). *Done when:* the rule
  and its supported guard shapes are written down.
- **TY3.2** Implement the refinement in the `if` typing path, reusing the
  in-`match` narrowing machinery where possible. *Done when:* a value used at
  the narrowed type inside the then-branch type-checks without an explicit
  cast.
- **TY3.3** Decide and document the boundary: which guard shapes narrow (direct
  `type-of` test) vs. which do not (negation, conjunction) for 1.0. *Done
  when:* unsupported shapes are documented and do not silently mis-narrow.
- **TY3.4** Fixtures: narrowing then-branch (ok), narrowed else-branch where
  applicable, and an unsupported guard shape (no narrowing, explicit cast
  still required). *Done when:* all green and snapshotted; the union guide
  documents `if`-guard narrowing.

---

## Phase TY4 -- Lifetime inference / elision depth (audit item 5)

Lifetime elision implements only rule 2; rules 1 and 3 are placeholders, and
collected lifetimes are not bound to parameters. There is no constraint
solving / cycle detection, and inter-procedural borrow checking
(`-Xlinear`-gated) is minimal. The handler-borrow-capture check is fine and
stays.

- **TY4.1** Decide the 1.0 ambition honestly: full elision rules 1/2/3 +
  binding + constraint solving, OR a clearly-scoped subset with the rest
  documented as post-1.0. (Feeds TY1's `-Xlinear` graduation decision.)
  *Done when:* the scope is recorded.
- **TY4.2 (if implementing)** Implement elision rules 1 and 3 and bind
  collected lifetimes to their parameters. *Done when:* functions covered by
  rules 1/3 elaborate with parameter-bound lifetimes instead of placeholders.
- **TY4.3 (if implementing)** Add lifetime constraint solving with cycle
  detection so conflicting/cyclic lifetimes are rejected. *Done when:* a
  program with a cyclic lifetime is rejected with a clear error.
- **TY4.4** Either deepen inter-procedural borrow checking to the agreed scope
  or document its limits and ensure it does not give false "ok" on unsound
  programs within that scope. *Done when:* the borrow checker's guarantees
  match its documentation.
- **TY4.5** Fixtures matching the chosen scope: elision rule 1/3 acceptance,
  cyclic-lifetime rejection (if implemented), and inter-procedural
  borrow accept/reject pairs. *Done when:* all green and snapshotted.
- **TY4.6** Update `uniqueness-types-guide.md` / `substructural-types-guide.md`
  to state exactly which lifetime machinery is active at 1.0. *Done when:* the
  guides match the implementation.

---

## Phase TY5 -- Multi-capture closures in HKT (audit item 6)

Multi-capture closures in HKT contexts currently require a manual cast
workaround; the acceptance criterion in `archive/hkt-deferred-tasks.md`
section 5 is still unchecked.

- **TY5.1** Reproduce the workaround case as a failing-without-cast fixture and
  pin down why the manual cast is currently required. *Done when:* a minimal
  repro exists that needs the cast today.
- **TY5.2** Implement handling so a multi-capture closure in an HKT context
  type-checks and codegens without the manual cast. *Done when:* the repro
  compiles and runs correctly with the cast removed.
- **TY5.3** Check off the acceptance criterion in
  `archive/hkt-deferred-tasks.md` section 5 (or migrate it into this plan as
  resolved). *Done when:* the criterion is marked done with a pointer to the
  fixture.
- **TY5.4** Fixtures: the de-casted multi-capture HKT case, plus a regression
  ensuring single-capture HKT cases are unaffected. *Done when:* both green
  and snapshotted.

---

## Phase TY6 -- Shared continuation-typing items (audit items 1, 2, 3)

These are owned by the control-flow plan; tracked here only for completeness
of the typing audit.

- **TY6.1** `call/cc`/`escape` sugar stubs -> gated for 1.0. *Owner:*
  control-flow plan **CF4**.
- **TY6.2** `compose-handlers` nil placeholder -> implement-or-remove. *Owner:*
  control-flow plan **CF3**.
- **TY6.3** `shift`/`shift0` result-type placeholder -> real inference.
  *Owner:* control-flow plan **CF2**.
- **TY6.4** Confirm, at 1.0 sign-off, that CF2/CF3/CF4 have landed so the
  typing audit's items 1--3 are closed. *Done when:* this plan's exit criteria
  reference the merged CF phases.

---

## Exit criteria for 1.0 (advanced typing)

- No doc or in-tree comment misdescribes a shipped advanced-typing feature
  (TY0).
- A ratified flag-graduation matrix exists and is the source of truth for what
  "1.0 typing" includes (TY1).
- `any` round-trips for cstr/struct/ADT payloads with working `cast`/`type-of`,
  or any unsupported case fails loudly and is documented (TY2).
- `if`-guard narrowing works for the documented guard shapes; unsupported
  shapes do not silently mis-narrow (TY3).
- Lifetime/borrow machinery's guarantees match its documentation; nothing
  claims more than it enforces (TY4).
- Multi-capture HKT closures need no manual cast workaround (TY5).
- The shared continuation-typing items are closed via the control-flow plan
  (TY6).
- `bash tests/run.sh` reports zero `FAIL` lines and all fixture snapshots are
  regenerated per CLAUDE.md.

## See also

- [typing-gap-audit.md](typing-gap-audit.md)
- [control-flow-completeness-plan.md](control-flow-completeness-plan.md)
- [advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md)
- [refinement-types-plan.md](upcoming/refinement-types-plan.md)
