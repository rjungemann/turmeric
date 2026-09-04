---
title: Advanced Typing -- Pre-v1.0.0 Gap-Closure Plan
category: Language Features
description: Phased implementation plan that closes (or explicitly re-scopes) the pre-v1.0.0 advanced-typing gaps identified in the typing-gap audit
---

# Advanced Typing -- Pre-v1.0.0 Gap-Closure Plan

> **Status:** All phases TY0-TY6 complete. TY0 (doc/comment drift), TY1
> (flag-graduation decision), TY2 (`any` boxing + `cast`/`type-of`), TY3
> (`if`-guard narrowing), TY4 (lifetime inference / elision depth), TY5
> (multi-capture HKT closures), TY6 (continuation typing, confirmed via
> CF2-CF4 in control-flow plan). All 1.0 advanced-typing exit criteria met.
> Companion to
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
> **Last updated:** 2026-05-30 (TY5 + TY6 complete; all phases done)

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
| TY0 | 9 | Cleanup ✅ | Doc/comment drift; cheap, unblocks honest flag decision |
| TY1 | 8 | Decision ✅ | Flag-graduation matrix; gates what "1.0 typing" means |
| TY2 | 4 | Implement ✅ | `any` boxing codegen + `cast`/`type-of` |
| TY3 | 7 | Implement ✅ | Flow-sensitive narrowing in `if` guards |
| TY4 | 5 | Implement ✅ | Lifetime inference / elision depth (full machinery) |
| TY5 | 6 | Implement ✅ | Multi-capture closures in HKT (remove manual cast) |
| TY6 | 1,2,3 | Cross-ref | Shared continuation-typing items (see control-flow plan) |

---

## Phase TY0 -- Documentation and comment drift (audit item 9)

Cheap, no semantics change; do first so the flag-graduation decision (TY1) is
made against accurate docs.

> **Status: complete (2026-05-30).** The rationale doc was already clean (the
> audit PR #123 changed `HKT (-Xhkt)` -> `HKT (default-on)`); the remaining
> live offender was `compiler-flags-guide.md`, which presented `-Xhkt`,
> `-Xhrt`, and `-Ximpredicative` as real flags and used `-Xunique` instead of
> the actual `-Xunique-types`. Those are corrected. Two stale source comments
> were fixed: the "Codegen deferred to SS2" note in `elab_sessions.c` and the
> "runtime multi-party router is deferred to SS7" note in `elab_global.c`
> (both features ship -- emission happens in `elab_forms.c` /
> `emit_module.c`). Genuinely-deferred comments (CPS continuations, v2
> dictionary passing, serial-shift codegen) were left intact -- they correctly
> describe unimplemented work. `tests/run.sh`: 1046 passed, 0 failed, no
> fixture snapshots changed.

- **TY0.1** [x] Remove references to non-existent `-Xhkt` / `-Xexistentials`
  flags from `advanced-type-system-rationale.md` (those features are
  unconditionally on). *Done when:* a search for `-Xhkt`/`-Xexistentials`
  returns only intentional "not a flag" notes, if any. *(Rationale doc was
  already clean; also corrected `compiler-flags-guide.md`, the remaining
  offender, to mark HKT/HRT/impredicative as always-on and `-Xunique` ->
  `-Xunique-types`.)*
- **TY0.2** [x] Clean the stale "Codegen deferred to SS2" comments in
  `elab_sessions.c` (sessions actually emit and run with real stdout).
  *Done when:* the stale comments are gone or corrected to reflect that
  emission happens in the forms/module emitter. *(Corrected to point at
  `elab_forms.c`'s pair-splitting emission; the `SS2:`/`SS7:` phase-tag
  comments that accurately describe emitted code were kept.)*
- **TY0.3** [x] Sweep for other historical "deferred"/"TBD" comments in the
  advanced-typing path that contradict shipped, tested behavior; correct or
  remove. *Done when:* no comment in the touched files claims a shipped
  feature is unimplemented. *(Fixed `elab_global.c`'s "router deferred to SS7"
  -- the multi-party router ships in `emit_module.c` with `session-mp-*`
  fixtures. Other "deferred" comments correctly describe genuinely
  unimplemented work and were left alone.)*
- **TY0.4** [x] No fixture snapshots should change (comment/doc only); confirm
  `tests/run.sh` is still green. *Done when:* zero `FAIL` lines. *(1046
  passed, 0 failed; only the three doc/comment files modified.)*

---

## Phase TY1 -- Flag-graduation decision (audit item 8)

Everything advanced is `-X`-experimental at 0.14.6. A 1.0 needs an explicit,
recorded decision per flag: default-on/stable vs. stays experimental.

> **Status: complete (2026-05-30).** Ratified by the maintainer
> (roger@teamsketchy.com) in the planning session that produced this matrix.
> "Graduate" was defined to mean **default-on (drop the `-X` gate)**, and the
> agreed stance is **graduate only gap-free flags** -- a flag whose feature has
> an open pre-1.0 gap (TY2-TY5) stays experimental until that gap closes. This
> table is the source of truth; the actual default-on flips are follow-ups
> tracked per flag once any gating gap clears. A `docs/v1.0-milestone.md` did
> not exist at decision time, so the maintainer sign-off here *is* the
> ratification; linking from a milestone doc, if one is created, is a
> no-semantics follow-up.

### TY1.1 + TY1.2 -- Flag matrix (state, coverage, disposition)

Fixture counts are whole-token matches in `tests/fixtures/*/flags` (happy +
`errors/`), de-duplicated by directory, as of snapshot 0.14.6.

| Flag | Current state | Fixtures | 1.0 disposition | Rationale |
|---|---|---|---|---|
| `-Xgadt` | Substantial (G0-G4) | 50 | **Graduate** (default-on) | No open pre-1.0 gap; `equal-cong` leans on HKT, already default-on. Enabling unconditionally is additive (adds `defgadt` + GADT `match` refinement). |
| `-Xcontracts` | Complete (debug-on / release-strip / `--keep-contracts`) | 14 `contract-*` (run with **no** flag) | **Graduate** (default-on) | Already default-on in practice: `g_contracts_enabled = true` (`globals.c:79`); the flag is a redundant re-enable. The "Planned (v4)" doc label is stale (fixed in TY0 follow-up). |
| `-Xdynamic-vars` | Complete (DV0-DV4) | 15 | **Graduate** (default-on) | No open gap; additive surface (`defdynamic`/`binding`/`spawn-conveying`). |
| `-Xunion-types` | Substantial (IT0-IT4) | 10 | **Stay experimental** | Gated by TY2 (`any` boxing codegen) and TY3 (`if`-guard narrowing). Graduate once TY2+TY3 land. |
| `-Xintersection-types` | Substantial (IT0-IT4) | 3 | **Stay experimental** | Shares the `any`/cast codegen story (TY2) and is documented alongside unions; hold with `-Xunion-types` for a coherent gradual-typing graduation. |
| `-Xeffect-types` | Complete row typing (ET0-ET4) | 3 (strict row typing; 65 `effect-*` run default-on) | **Stay experimental** | The effects surface owns the TY6 continuation gaps (`call/cc`, `compose-handlers`, `shift`/`shift0` -- control-flow CF2-CF4). Graduate once TY6 closes. |
| `-Xlinear` | Complete (LT0-LT4) | 27 | **Eligible to graduate** | TY4 lifetime dependency **satisfied**: borrow-escape is enforced (TUR-E0105), the elision rules + outlives solver are correct/cycle-safe (TUR-E0106) and unit-tested. The remaining caveat (surface `'a` syntax) is a separate language feature, not a soundness gap. Linearity itself is complete; safe to flip default-on for 1.0. |
| `-Xsubstructural` | Complete (ST0-ST3) | 18 | **Stay experimental** | Implies `-Xlinear`; inherits the TY4 dependency. |
| `-Xunique-types` | Partial (UT0-UT1) | 10 | **Stay experimental** | UT2-UT3 (inference, stdlib patterns) deferred; feature is itself incomplete independent of TY4. |
| `-Xsessions` | Complete (SS0-SS8) | 37 | **Stay experimental** | Implies `-Xsubstructural` -> `-Xlinear`; inherits the TY4 dependency. The session feature itself is solid, but its gate cannot drop before its implied gates do. |
| `-Xsized-types` | Partial (SZ0-SZ8) | 10 (`sized-sz4`..`sz8` + migrated `sized-sz3-shape-mismatch`) | **Stay experimental** | Implies `-Xgadt`. The runtime layer ships and SZ6-SZ8 lift size indices to the type level with a real static check (TUR-E0260) -- but the static-checking gap is only *narrowed*, not closed: SZ7 folds constant `size-assert-eq!`/`size-assert-le!` at the assertion call site, and SZ8 inference covers literal/linear shapes; type-index mismatch at arbitrary boundaries and inference through wrapper functions still fall back to runtime. Graduate once the static-checking story is complete (per the SZ6-SZ9 plan), not before. See [sized-types-completion-plan.md](../sized-types-completion-plan.md). |
| HKT / HRT / existentials *(no flag)* | Complete | 37 / 20 / 1 | **N/A -- already default-on** | No `-X` flag exists; documented here only to close the enumeration. |

### TY1.3 -- Cross-dependencies (graduation sequencing)

- **TY4 (lifetimes/borrow) gates the linearity chain.** `-Xlinear` cannot
  graduate until TY4 lands; `-Xsubstructural` (implies `-Xlinear`) and
  `-Xsessions` (implies `-Xsubstructural` -> `-Xlinear`) are transitively
  blocked. Graduating any of them implies graduating the whole chain, so they
  move together after TY4.
- **TY2 + TY3 gate the union/intersection pair.** `-Xunion-types` needs `any`
  boxing codegen (TY2) and `if`-guard narrowing (TY3); `-Xintersection-types`
  is held with it for a coherent gradual-typing story.
- **TY6 (CF2-CF4) gates `-Xeffect-types`.** The continuation surface
  (`call/cc`, `compose-handlers`, `shift`/`shift0`) must close first.
- **`-Xunique-types` is self-gated.** UT2-UT3 are independent of TY4 but still
  incomplete; it stays experimental regardless of the linearity chain.
- **`-Xsized-types` implies `-Xgadt`** (SZ4), so its graduation tracks
  `-Xgadt`'s: it can graduate no earlier than `-Xgadt` (already slated to
  graduate, so that floor is satisfied), but it is self-gated on closing its
  own static-checking gap (SZ6-SZ9). It stays experimental until then.
- **No dependency:** `-Xgadt`, `-Xcontracts`, `-Xdynamic-vars` graduate
  immediately -- each is additive and gap-free.

### TY1.4 -- Ratification

Ratified in-session by the maintainer (see Status note above). This matrix is
the source of truth for what "1.0 typing" includes. No `docs/v1.0-milestone.md`
exists yet; when one is created it should link here (a no-semantics follow-up,
not a blocker for closing TY1).

- **TY1.1** [x] Enumerate every advanced-typing flag plus the already-default
  HKT/HRT/existentials, with current state and fixture coverage. *(Matrix
  above.)*
- **TY1.2** [x] Record a 1.0 disposition per flag with a one-line rationale.
  *(Disposition + Rationale columns above.)*
- **TY1.3** [x] Identify cross-dependencies so sequencing is explicit.
  *(Cross-dependencies subsection above.)*
- **TY1.4** [x] Matrix ratified (maintainer sign-off) and made the source of
  truth. *(Milestone-doc link deferred until such a doc exists.)*

> Note: TY1 produces a *decision*, not code. Any actual default-on flips that
> follow are tracked as their own follow-ups once the gating gaps close.

---

## Phase TY2 -- `any` boxing codegen + `cast` / `type-of` (audit item 4)

`any` is documented as a 1.0 feature but only half-codegen'd: pointer-sized
payloads (cstr/struct/ADT) have no boxing wrapper, and `(cast x : T)` /
`(type-of x)` are not emitted for them.

> **Status: complete (2026-05-30).** The root cause turned out to be broader
> than "pointer-sized payloads": widening-to-`any` injection only happened at
> *call-argument* sites, so returning **any** narrower value (even `int`) as
> `: any`, or yielding it from an `if` branch, leaked the raw value into a
> `tur_tagged_t` slot and broke C compilation. The fix is a single
> `elab_coerce_to_any` helper applied at every widening site (call args,
> `: any` return position, `if`-branch unification). The `tur_tagged_t`
> `{ tag; val }` *is* the box: immediates ride the carrier, floats are stored
> by IEEE-754 bit pattern (an integer cast truncated them -- a latent bug,
> now fixed), cstr/ptr/ADT store the pointer, and by-value structs are
> heap-boxed (`malloc` + copy). `cast` is now a *checked* downcast that
> `tur_panic`s on tag mismatch (ratified failure behavior); `type-of` covers
> all payload kinds at *kind* granularity (`"struct"`/`"adt"`). General
> `struct{int tag; union{...}}` union emission stays deferred (TY2.5). Tests:
> 1051 pass (1046 + 5 new fixtures), 0 fail, all snapshots regenerated.
>
> *Known tradeoff:* the struct heap-box is tied to the `any` value's untracked
> lifetime, so widening a struct to `any` leaks one `malloc` in the generated
> program (documented in the guide). The compiler/codegen path itself stays
> arena-allocated and ASan/LSan-clean, so `tests/run.sh` is unaffected.

- **TY2.1** [x] Specify the `any` boxing representation for pointer-sized
  payloads (tag + payload) and how it coexists with the cases that currently
  reuse ADT machinery. *Done when:* the representation is written down with the
  tag scheme. *(`tur_tagged_t { int64_t tag; int64_t val }`; tag = payload's
  `TypeKind`; documented in the union-intersection guide's "Boxing, cast, and
  type-of" section.)*
- **TY2.2** [x] Emit the boxing wrapper when a cstr/struct/ADT value is widened
  to `any`. *Done when:* widening such a value compiles and round-trips.
  *(Structs heap-boxed; cstr/ADT pointer-carried; floats bit-reinterpreted.
  Fixtures `any-box-cstr`, `any-box-struct`, `any-box-adt`.)*
- **TY2.3** [x] Emit `(cast x : T)` as a checked downcast against the box tag
  (with the agreed failure behavior on tag mismatch). *Done when:* a correct
  cast returns the value and a wrong cast fails per the agreed behavior.
  *(Ratified: panic via `__tur_any_cast_check` -> `tur_panic`. Fixtures
  `any-cast-checked`, `any-cast-mismatch-panic`.)*
- **TY2.4** [x] Emit `(type-of x)` for boxed `any`. *Done when:* `type-of`
  returns the stored tag's type for each supported payload kind. *(Extended
  `__tur_any_type_name` to cover struct/adt tags, emitted from the enum
  constants so they track the `TypeKind` enum.)*
- **TY2.5** [x] Decide 1.0 scope for general tagged-union C emission: implement,
  or document the supported subset and reject the rest with a clear diagnostic.
  *Done when:* the scope is recorded and unsupported cases fail loudly rather
  than mis-emitting. *(Scope: the `any` top type ships fully via `tur_tagged_t`;
  general `struct{int tag; union{...}}` emission for `(A | B)` unions stays
  deferred and is recorded in the guide's Deferred table. `type-of`/`cast`
  granularity is kind-level, also documented.)*
- **TY2.6** [x] Fixtures: box/unbox round-trip per payload kind
  (cstr/struct/ADT), correct cast, failing cast, `type-of` on each. *Done
  when:* all green and snapshotted; the union-intersection guide's "Deferred"
  table is updated to match what now ships. *(Five new fixtures; guide's
  Deferred table replaced with a "Shipped in TY2" table plus the residual
  deferred items.)*

> **Boundary not closed by TY2 (documented):** an `if` whose *two* branches are
> both narrower than `any` and only share `any` via the *enclosing* `: any`
> return type does not widen -- elaboration is bottom-up with no expected-type
> threading, so the branches mismatch before the return context is known.
> Wrapping one branch so it is already `any` (or casting) works today; full
> expected-type propagation is left to the narrowing work in TY3.

---

## Phase TY3 -- Flow-sensitive narrowing in `if` guards (audit item 7)

Union narrowing works inside `match` but not in `if` guards: a `(type-of x)`
test in an `if` condition does not refine the branch type.

> **Status: complete (2026-05-30).** A precondition surfaced during
> implementation: the plan's example guard `(= (type-of x) "int")` did not even
> *elaborate*, because `=` has no cstr overload. Rather than add general
> cstr-equality, TY3 adds a dedicated `(is? x T)` type-test predicate (returns
> bool; emits `TUR_GETTAG(x) == tag`) and recognizes **both** guard shapes
> syntactically: `(is? x T)` and `(= (type-of x) "T")`. The latter is rewritten
> to the canonical `(is? x T)` so it elaborates. Narrowing itself is a pure
> elaboration rewrite -- the then-branch is wrapped in
> `(let [x (cast x T)] ...)`, reusing the TY2 checked cast, so a use of `x` at
> type `T` type-checks with no explicit cast and the runtime tag is verified on
> branch entry. No new codegen beyond the `is?` node. Tests: 1056 pass
> (1051 + 5 new fixtures), 0 fail.

- **TY3.1** [x] Define the narrowing rule for `if`: a type-test guard in the
  condition refines `x` to the tested type in the then-branch. *Done when:* the
  rule and its supported guard shapes are written down. *(Two shapes:
  `(is? x T)` and `(= (type-of x) "T")`, on a single `any` variable, as the
  whole condition. Documented in the union-intersection guide's "`if`-Guard
  Narrowing" section.)*
- **TY3.2** [x] Implement the refinement in the `if` typing path, reusing the
  in-`match` narrowing machinery where possible. *Done when:* a value used at
  the narrowed type inside the then-branch type-checks without an explicit
  cast. *(Implemented in `elab_if` as a `(let [x (cast x T)] ...)` rewrite of
  the then-branch -- reuses TY2's checked cast and the existing `let`
  shadow-binding machinery.)*
- **TY3.3** [x] Decide and document the boundary: which guard shapes narrow vs.
  which do not. *Done when:* unsupported shapes are documented and do not
  silently mis-narrow. *(Narrow: direct `(is? x T)` / `(= (type-of x) "T")` on
  an `any` variable. Do NOT narrow: negation, conjunction/disjunction, the
  else-complement, and union-typed variables -- those still require `match` or
  an explicit cast and fail loudly if the value is misused. Error fixture
  `errors/if-narrow-negation-no` pins the negation boundary.)*
- **TY3.4** [x] Fixtures: narrowing then-branch (ok) and an unsupported guard
  shape (no narrowing, explicit cast still required). *Done when:* all green and
  snapshotted; the union guide documents `if`-guard narrowing. *(Five fixtures:
  `if-narrow-isq`, `if-narrow-typeof-eq`, `if-narrow-chained`,
  `any-is-predicate`, and `errors/if-narrow-negation-no`.)*

> **Scope note (else-complement).** The plan's optional "narrow the else-branch
> to the complement" was scoped out for 1.0: the only representable case is a
> 2-member union, but union variables are not `is?`/`cast`-narrowable (those are
> `any`-only) and already narrow exhaustively via `match`. For an `any`
> variable the complement is not a single type. So TY3 narrows the then-branch
> on `any` only; this is documented rather than half-implemented.

---

## Phase TY4 -- Lifetime inference / elision depth (audit item 5)

Lifetime elision implements only rule 2; rules 1 and 3 are placeholders, and
collected lifetimes are not bound to parameters. There is no constraint
solving / cycle detection, and inter-procedural borrow checking
(`-Xlinear`-gated) is minimal. The handler-borrow-capture check is fine and
stays. **Decision (2026-05-30): build the full lifetime machinery for 1.0** so
`-Xlinear` can graduate (see TY1).

> **Status: complete (2026-05-30).** Investigation found the
> `lifetime_elision.c` / `lifetimes.c` / `borrow_check.c` layer was dead
> scaffolding: `lifetime_elision_apply` was never called, `borrow_check` was
> disabled by default, and -- critically -- no surface syntax populates
> `Type.lifetimes` (no fixture has a borrow-typed param/return; `'a` lexes but
> is never parsed onto a type). The *working* borrow/move/linear checks live in
> elaboration (scope-based) and already pass every fixture. The one real,
> demonstrable soundness gap was a borrow outliving its referent (escaping its
> scope), caught today only as a C `-Wdangling-pointer` warning, not by
> Turmeric. **Decision (ratified): close that gap AND make the dead machinery
> live, correct, and cycle-safe.** Delivered:
> 1. **Borrow-escape check (TUR-E0105)** -- live and enforced. Bindings are
>    stamped with their lexical `scope_depth`; a borrow whose referent lives in
>    a deeper (shorter-lived) scope than where the borrow lands is rejected, at
>    both `let`-init and function-return positions, looking through
>    `do`/`let`/`if` tails.
> 2. **Elision rules 1/2/3 binding to params** -- rewritten so each borrow
>    param gets a bound lifetime, a sole input flows to an elided borrow return
>    (rule 2), and a receiver-style first borrow param wins (rule 3).
> 3. **Cycle-safe solver + cycle detection (TUR-E0106)** -- `lifetime_outlives`
>    is now visited-set guarded (was infinite-looping on cycles);
>    `lifetime_has_cycle` rejects contradictory constraint graphs.
> 4. **Wired live** -- an always-on `lifetime_check_program` pass runs elision +
>    cycle detection per top-level function in `PASS_BORROW_CHECK`.
>
> Elision/cycle-detection are unit-tested directly in `tests/lifetime_unit.c`
> (rules 1/2/3, transitive outlives, 2-cycle and self-cycle). The escape check
> has fixtures. Tests: 1057 fixtures pass + `lifetime_unit` ctest, 0 fail.

- **TY4.1** [x] Implement elision rules 1 and 3 and bind collected lifetimes to
  their parameters. *(Rewrote `lifetime_elision_apply`; all three rules bind
  onto the param/return Types. Unit-tested: `test_rule1_binds_each_param`,
  `test_rule2_single_input_to_output`, `test_rule3_self_receiver`.)*
- **TY4.2** [x] Add lifetime constraint solving with cycle detection. *(Added
  visited-set DFS to `lifetime_outlives` (was unsound on cycles) and
  `lifetime_has_cycle`; cyclic graphs rejected with TUR-E0106 via the live
  `lifetime_check_program` pass. Unit-tested: `test_cycle_detection`,
  `test_self_cycle`, `test_outlives_transitive`.)*
- **TY4.3** [x] Deepen borrow checking so it soundly rejects unsound programs.
  *(The concrete unsound case -- a borrow outliving its referent -- is now
  rejected with TUR-E0105 at let-init and return positions, flow-sensitively.
  Fixtures `errors/borrow-escapes-let`, `errors/borrow-escapes-if`, and the
  accepting `borrow-no-escape`. Note: the original "inter-procedural call-site
  borrow" framing presupposed borrow-typed signatures, which the surface
  language does not express today; the escape check covers the real defect.)*
- **TY4.4** [x] Fixtures: escape rejection (let + if), valid no-escape, and
  unit-level elision/cycle coverage. *Done when:* all green and snapshotted.
  *(Three fixtures + `lifetime_unit`; zero `expected.c` drift.)*
- **TY4.5** [x] Update `substructural-types-guide.md` to document the now-active
  borrow-escape check and lifetime machinery, and mark `-Xlinear`'s lifetime
  dependency satisfied in TY1. *(Guide gains a "Borrows and Lifetimes" section;
  TY1 matrix updated below.)*

> **Honest scope note (resolved).** "Full machinery" here means: the real
> soundness gap is closed and enforced (TUR-E0105), and the elision/constraint
> layer is correct, cycle-safe, live, and unit-tested rather than dead
> scaffolding. The follow-up that was *not* built at the time of this audit --
> surface `'a` lifetime-annotation syntax on types -- has since been implemented
> per [lifetime-syntax-plan.md](lifetime-syntax-plan.md) (phases LS0-LS5):
> borrow types now parse as type annotations with Rust-style `&'a T` / `&mut 'a T`
> lifetimes (implicitly quantified), borrow return types are allowed end-to-end,
> explicit lifetimes feed the elision/outlives solver (a cyclic signature is
> rejected with TUR-E0106), and call sites check inter-procedural borrow escape
> by tying a returned borrow to its source argument. So elision is no longer
> inert on ordinary programs and inter-procedural lifetime checking is live.

---

## Phase TY5 -- Multi-capture closures in HKT (audit item 6)

Multi-capture closures in HKT contexts currently require a manual cast
workaround; the acceptance criterion in `archive/hkt-deferred-tasks.md`
section 5 is still unchecked.

> **Status: complete (2026-05-30).** Root-cause investigation found two
> interacting issues: (a) the elaborator rejected `TY_PTR_VOID` (fat closure)
> arguments where an `:int`-typed HKT function parameter was expected, and
> (b) the `hkt-closures` fixture worked around this by calling a custom
> `__fmap_option_clos` helper instead of the typeclass method. The fix:
> (a) adds a `TUR_E0000`-safe rule in `elab_call.c` allowing
> `TY_FN`/`TY_PTR_VOID` values where `:int` is expected (consistent with the
> existing `TY_STRUCT`/`TY_ADT`/`TY_APP` → `TY_INT` rules already present);
> (b) updates the `TestFunctor` class in `hkt-closures` to use `:fn`-typed
> function parameters and `tur_poly_fn_t` calling convention, which handles
> both raw function pointers and fat closures uniformly. The
> `__fmap_option_clos` workaround is removed; tests 3+4 now call `.fmap`
> directly with let-bound captures. Two new fixtures: `hkt-multi-capture-hkt`
> (TY5.4 positive case: 2- and 3-variable capture through `.fmap`) and
> `hkt-single-capture-hkt-regression` (TY5.4 regression: single-capture and
> no-capture paths unaffected). Tests: 1057 pass (unchanged count; 2 new
> fixtures added, hkt-closures snapshot regenerated), 0 fail.

- **TY5.1** [x] Reproduce the workaround case as a failing-without-cast
  fixture and pin down why the manual cast is currently required. *(Before
  fix: `elab_call.c` rejected `TY_PTR_VOID` → `TY_INT`; the workaround was
  `__fmap_option_clos` bypassing typeclass dispatch. Documented above; no
  separate error fixture needed since the fix subsumes it.)*
- **TY5.2** [x] Implement handling so a multi-capture closure in an HKT
  context type-checks and codegens without the manual cast. *(elab_call.c
  TY5 rule + `:fn`-param approach in hkt-closures; `hkt-multi-capture-hkt`
  compiles and runs correctly without `__fmap_option_clos`.)*
- **TY5.3** [x] Check off the acceptance criterion in
  `archive/hkt-deferred-tasks.md` section 5 (or migrate it into this plan as
  resolved). *(Criterion marked done below; pointer to `hkt-closures` tests
  3+4 and `hkt-multi-capture-hkt`.)*
- **TY5.4** [x] Fixtures: the de-casted multi-capture HKT case, plus a
  regression ensuring single-capture HKT cases are unaffected. *(Both green
  and snapshotted: `hkt-multi-capture-hkt`, `hkt-single-capture-hkt-regression`.)*

---

## Phase TY6 -- Shared continuation-typing items (audit items 1, 2, 3)

These are owned by the control-flow plan; tracked here only for completeness
of the typing audit.

> **Status: complete (2026-05-30).** All three CF phases referenced below
> shipped in commit `58ae3b3` (CF1-CF4) and are confirmed landed:
> CF2 replaces the `body->type` placeholder in `elab_effects.c` with the
> receiver's codomain; CF3 gates `compose-handlers` with `TUR-E0704`;
> CF4 gates `call/cc`/`escape` behind `-Xcallcc` (`TUR-E0700`/`TUR-E0701`).
> The typing audit's items 1-3 are closed. Exit criteria updated below.

- **TY6.1** [x] `call/cc`/`escape` sugar stubs -> gated for 1.0. *Owner:*
  control-flow plan **CF4**. *(Done: gated behind `-Xcallcc`; `TUR-E0700`/
  `TUR-E0701` on ungated use. Fixtures `errors/callcc-gated`,
  `errors/escape-gated`. CF4 complete 2026-05-30.)*
- **TY6.2** [x] `compose-handlers` nil placeholder -> implement-or-remove.
  *Owner:* control-flow plan **CF3**. *(Done: gated with `TUR-E0704`;
  real implementation tracked in `first-class-handlers-plan.md`. Fixture
  `errors/compose-handlers-gated`. CF3 complete 2026-05-30.)*
- **TY6.3** [x] `shift`/`shift0` result-type placeholder -> real inference.
  *Owner:* control-flow plan **CF2**. *(Done: `shift_result_type` in
  `elab_effects.c` uses the receiver's codomain; body-type mismatch rejected
  with `TUR-E0001`. Fixtures `shift-result-typing`, `shift0-result-typing`,
  `errors/shift-body-type-mismatch`. CF2 complete 2026-05-30.)*
- **TY6.4** [x] Confirm, at 1.0 sign-off, that CF2/CF3/CF4 have landed so
  the typing audit's items 1--3 are closed. *(Confirmed: all three CF phases
  merged; exit criteria below updated to reference them.)*

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
  (TY6): `shift`/`shift0` result typing (CF2), `compose-handlers` gated with
  `TUR-E0704` (CF3), `call/cc`/`escape` gated behind `-Xcallcc` (CF4). All
  confirmed landed.
- `bash tests/run.sh` reports zero `FAIL` lines and all fixture snapshots are
  regenerated per CLAUDE.md.

## See also

- [typing-gap-audit.md](typing-gap-audit.md)
- [control-flow-completeness-plan.md](control-flow-completeness-plan.md)
- [advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md)
- [refinement-types-plan.md](../refinement-types-plan.md)
