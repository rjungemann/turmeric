---
title: Control Flow -- Pre-v1.0.0 Completeness Plan
category: Language Features
description: Phased implementation plan that closes (or explicitly gates) the pre-v1.0.0 control-flow gaps identified in the control-flow completeness audit
---

# Control Flow -- Pre-v1.0.0 Completeness Plan

> **Status:** Phases CF0-CF1 complete (CF0: disposition ratified + diagnostics
> namespace reserved + at-risk fixtures inventoried; CF1: self-tail-call -> loop
> lowering shipped); CF2 next. Companion to
> [control-flow-completeness-audit.md](control-flow-completeness-audit.md);
> every phase below maps to a numbered gap in that audit's "Pre-v1.0.0 gaps"
> section. Post-1.0 work (full CPS pass, MT scheduler bridge, trampolining)
> is explicitly out of scope here -- see the audit's "Post-v1.0.0 gaps".
>
> **Snapshot:** `0.14.6`.
>
> **Guiding principle (from the audit's bottom line):** for each gap the
> 1.0 choice is one of three -- **implement**, **lower** (TCO), or
> **gate-off-and-document**. Real continuation capture needs the full CPS
> pass, which is correctly deferred; so the call/cc-class gaps are resolved
> for 1.0 by gating, not by implementing capture.
>
> **Last updated:** 2026-05-30

---

## Motivation

The control-flow substrate (delimited continuations + effect handlers +
fiber runtime) is shipped and broadly exercised. The remaining pre-1.0 risk
is concentrated in a handful of features that are either non-functional sugar
(`call/cc`/`escape`, `compose-handlers`), missing a fundamental lowering
(self-tail-call TCO), carrying typing placeholders (`shift` result type), or
holding a soundness hole (async Send across await points). This plan turns
each audit item into an ordered phase with concrete tasks and done-criteria,
so a 1.0 tag can be reached without leaving correctness landmines on the
shipped surface.

Goals:

- Eliminate every "runs only because it is never exercised" stub from the
  1.0 surface (either implement it or gate it behind a clearly-failing
  diagnostic).
- Add self-tail-call -> loop lowering so idiomatic `(let loop ...)` does not
  overflow.
- Remove the async Send soundness hole or scope it down to a checked subset.
- Make every remaining limitation a diagnostic or a documented restriction,
  not silent wrong behavior.

Non-goals (deferred to post-1.0, tracked in the audit):

- The full whole-program CPS transformation.
- Real first-class `call/cc` capture, `yield` in arbitrary positions,
  recursive generators, and liveness-precise cloneable capture (all gated on
  CPS).
- Multi-threaded scheduler integration and general/mutual-tail-call
  trampolining.

---

## Phase ordering at a glance

| Phase | Audit item | Disposition | Why this order |
|---|---|---|---|
| CF0 | -- | Decision + tracking | Records implement/lower/gate per item before code moves |
| CF1 | 5 | Lower | Self-tail-call TCO; independent, high user value |
| CF2 | 3 | Implement | `shift`/`shift0` result typing; small, unblocks clean diagnostics |
| CF3 | 2 | Implement | `compose-handlers` semantics |
| CF4 | 1 | Gate (`-Xcallcc`) | `call/cc`/`escape` real capture is post-1.0 |
| CF5 | 4 | Gate-off-and-document | Generator `yield`-in-`match` / recursion limits |
| CF6 | 6 | Scope down | Async Send-across-await soundness |
| CF7 | 7, 8 | Implement / tighten | Cloneable deep clone + capture precision |

> **Ratified 2026-05-30 (CF0.1).** This disposition table is the single
> source of truth for the 1.0 control-flow milestone; the per-item
> dispositions above are accepted with no overrides. `compose-handlers`
> (CF3) is **implemented**, not removed; `call/cc`/`escape` (CF4) are
> **gated**, not implemented. The 1.0 milestone tracks this plan via the
> [Exit criteria for 1.0](#exit-criteria-for-10-control-flow) section below.

---

## Phase CF0 -- Disposition decision and tracking

Records, before any code changes, the per-item 1.0 decision so reviewers and
the changelog have a single source of truth.

- **CF0.1** Confirm the disposition table above with maintainers; capture any
  overrides (e.g. if `compose-handlers` is to be removed rather than
  implemented). *Done when:* the table is ratified in this doc and linked
  from the 1.0 milestone.
- **CF0.2** For every "gate-off" item, agree the exact diagnostic wording and
  error code namespace so CF4/CF5 emit consistent messages. *Done when:* a
  short "gated control-flow features" subsection lists each gated form and its
  diagnostic code.
- **CF0.3** Identify the fixtures that currently "pass" only because they never
  exercise the stub (`continuation-callcc`, `continuation-escape`, the
  `compose-handlers` fixture, if any). *Done when:* each is tagged in this doc
  as "will convert to expect-error" (CF4/CF3) so the suite stays green across
  the transition.

### CF0.1 outcome -- ratified dispositions

The disposition table under [Phase ordering at a glance](#phase-ordering-at-a-glance)
is ratified as written (2026-05-30). No maintainer overrides were taken: the two
items that carried an open implement-vs-remove question are both resolved toward
**implement/gate**, not removal --

- **CF3 (`compose-handlers`)** -- implement real composition (already recorded
  in the CF3 header as the 2026-05-30 decision).
- **CF4 (`call/cc`/`escape`)** -- gate behind `-Xcallcc`; ungated use is a hard
  compile error. Real capture stays post-1.0 (CPS).

### CF0.2 outcome -- gated control-flow features and their diagnostics

Every form that 1.0 turns off (rather than implements) gets a stable diagnostic
code in a reserved **`E07xx` "gated / unsupported control-flow"** band, so CF4
and CF5 emit consistent, greppable messages. The `E07xx` band is currently
unused (existing control-flow codes live in `E0016`-`E0019`, the `E025x`
handler band, and the `E050x` multishot band); reserving a fresh band keeps the
"this feature is gated for 1.0" class self-contained.

The experimental opt-in flag follows the existing `-X<feature>` convention in
`src/main.c` (`wk_apply_flags`), defaults **off**, and gates only CF4:

| Form | Phase | Gating | Diagnostic code | Wording (ungated) |
|---|---|---|---|---|
| `call/cc` | CF4 | `-Xcallcc` (default off) | `TUR-E0700` | `'call/cc' has no real continuation capture yet (unsound) and is gated; pass -Xcallcc to experiment. Real capture requires the post-1.0 CPS pass.` |
| `escape` | CF4 | `-Xcallcc` (default off) | `TUR-E0701` | `'escape' has no real early-exit semantics yet (unsound) and is gated; pass -Xcallcc to experiment. Real capture requires the post-1.0 CPS pass.` |
| `yield` / `yield*` inside a `match` arm | CF5 | always rejected | `TUR-E0702` | `'yield' is not supported inside a 'match' arm (1.0 limitation); this requires the post-1.0 CPS pass.` |
| `yield` / `yield*` inside a recursive generator | CF5 | always rejected | `TUR-E0703` | `'yield' is not supported inside a recursive generator (1.0 limitation); this requires the post-1.0 CPS pass.` |

`-Xcallcc` help text (CF4.1): `enable experimental call/cc / escape -- no real
capture yet (unsound); requires the post-1.0 CPS pass`.

Notes:

- CF5 diagnostics (`E0702`/`E0703`) are *always-on* rejections, not flag-gated:
  there is no experimental path for unsupported `yield` placement, since
  mis-lowering would be silently wrong.
- These four codes are reserved here so the enum additions in
  `src/compiler/diag.h` / `diag.c` (CF4, CF5) do not collide and so the
  changelog can reference them before the code lands.

### CF0.3 outcome -- at-risk fixture inventory

These fixtures currently PASS only because they never exercise the gated stub
(`call/cc`/`escape` desugar to identity / dummy `0`). Each is tagged with its
transition so the suite stays green across CF3/CF4/CF5:

| Fixture | Exercises | Transition | Phase |
|---|---|---|---|
| `tests/fixtures/continuation-callcc` | `call/cc` identity desugar | move behind `-Xcallcc`; add ungated expect-error (`TUR-E0700`) sibling | CF4.3 |
| `tests/fixtures/continuation-escape` | `escape` dummy-`0` desugar | move behind `-Xcallcc`; add ungated expect-error (`TUR-E0701`) sibling | CF4.3 |
| `tests/fixtures/continuation-escape-fn` | `escape` over a fn arg | move behind `-Xcallcc` | CF4.3 |

`compose-handlers` (CF3) needs **no** expect-error conversion: it is being
implemented, not gated. The only fixtures referencing it today are
`tests/fixtures/effect-handler-type` (uses it in a doc comment / type position,
not as the nil-returning call) and `tests/fixtures/errors/effect-handler-overlap`
(already an expect-error for `TUR-E0251` overlap). The runtime-composition
fixtures land fresh in CF3.2/CF3.3. The `tests/fixtures/effect-handler-compose`
fixture composes via **nested `handle`**, not `compose-handlers`, so it is
unaffected.

---

## Phase CF1 -- Self-tail-call optimization (audit item 5)

Lower self-recursive tail calls (the `(let loop [...] ... (loop ...))` shape
that desugars through named-let -> `letrec`) into an iterative loop so
iteration count no longer drives stack depth. Mutual/general TCO and
trampolining stay post-1.0.

- **CF1.1** Define "self-tail-call": a call to the enclosing function/loop
  binding in tail position, where tail position is computed through `if`,
  `when`, `cond`, `do`, `let`, and `match` arms. *Done when:* a written tail-
  position predicate exists and has unit coverage over each form.
- **CF1.2** Add a tail-call analysis that marks self-tail-calls during/after
  named-let -> `letrec` desugaring, without disturbing non-tail recursion.
  *Done when:* analysis flags the recursive call in a countdown loop and does
  not flag a non-tail recursive sum.
- **CF1.3** Lower marked self-tail-calls to a backedge (parameter reassignment
  + loop) in the emitter. *Done when:* generated C for a countdown loop
  contains a loop, not unbounded self-recursion.
- **CF1.4** Fixtures: a deep countdown (e.g. 10M iterations) that previously
  overflowed now completes; a non-tail recursion fixture is unchanged.
  *Done when:* both run green under `tests/run.sh` and snapshots are
  regenerated per CLAUDE.md.
- **CF1.5** Document the guarantee and its boundary (self-tail only; mutual/
  general tail calls remain post-1.0) in `generators-guide.md`/relevant guide
  and link this phase. *Done when:* the guide states the guarantee precisely.

### CF1 outcome (complete -- 2026-05-30)

Self-tail-call -> loop lowering ships in the C emitter. A self-recursive call in
tail position is rewritten to a backedge: argument values are evaluated into
temporaries, the C parameter variables are reassigned, and control jumps to a
`__tur_tailcall:` label at the top of the function body. A 10,000,000-iteration
countdown that previously overflowed the C stack now completes.

- **Tail-position predicate / analysis (CF1.1, CF1.2)** -- `tco_mark` in
  `src/compiler/emit_fns.c` walks tail positions through `if`, `do`, and
  `let`/`letrec` (`cond`/`when` macro-expand to `if` before the IR), marking the
  new `Expr.as.call_.is_tail_self_call` flag (`src/compiler/expr.h`) on direct,
  arity-matching self-calls. Self-identity is by resolved C name, so the
  named-let desugar (`(let loop ...)` -> `letrec` -> static fn) is recognized
  even though the loop binding and the `fn`'s own binding are distinct objects.
  Non-tail recursion (e.g. `(+ n (f ...))`) is never marked.
- **Lowering (CF1.3)** -- `emit_tail` emits the backedge; gated to value-
  returning, non-`main`, non-closure functions whose parameters are simple
  scalars (pass-by-pointer struct / fn-typed / poly-fn / carrier-ABI params are
  excluded so the backedge temporary's C type is unambiguous). Functions with no
  self-tail-call are emitted exactly as before (no snapshot churn).
- **Fixtures (CF1.4)** -- `tests/fixtures/tco-self-tail-deep` (10M-iteration
  countdown, asserts stdout + the lowered codegen) and
  `tests/fixtures/tco-non-tail-unchanged` (non-tail `sum-to`; snapshot has no
  `__tur_tailcall`, locking in "non-tail recursion is unchanged"). The existing
  `named-let-loop` / `named-let-shadowing` snapshots were regenerated to the
  lowered form; `letrec-self-recursive` (factorial, non-tail) is unchanged.
- **Docs (CF1.5)** -- `docs/guides/performance-guide.md` gains a "Self-tail-call
  optimization" subsection stating the guarantee and its boundary (self-tail
  only; mutual/general/`match`-arm tail calls deferred to the post-1.0 CPS
  pass), and the Fibonacci example now uses the lowered named-let form.

> **Residual (documented):** tail calls inside `match` arms and mutual/general
> tail calls are compiled as ordinary recursive calls (correct, not stack-
> optimized) -- consistent with the "self-tail only" boundary and the post-1.0
> CPS deferral.

---

## Phase CF2 -- `shift` / `shift0` result typing (audit item 3)

Replace the `body->type` placeholder result type for `shift`/`shift0` with a
properly inferred delimited-continuation result type, so these expressions
cannot silently mistype.

- **CF2.1** Characterize the intended typing rule for `shift`/`shift0` against
  the surrounding `reset` and the captured continuation's answer type.
  *Done when:* the rule is written down with the cases it must handle.
- **CF2.2** Replace the placeholder so the result type is derived from the
  rule rather than reused from the body. *Done when:* a program that relied on
  the placeholder coincidence now type-checks via the rule, and a
  deliberately mistyped `shift` is rejected with a clear error.
- **CF2.3** Add expect-error and expect-ok fixtures covering at least: matching
  answer types (ok), mismatched answer types (error), and `shift0`'s
  distinct delimiter behavior. *Done when:* fixtures pass and are snapshotted.

---

## Phase CF3 -- `compose-handlers` (audit item 2)

`compose-handlers` currently elaborates to a nil placeholder ("runtime
semantics TBD"). **Decision (2026-05-30): implement real composition for
1.0.**

- **CF3.1** Specify composition semantics: handler order, effect-row union,
  and resume/discontinue behavior of the composed handler. *Done when:* a
  one-paragraph operational spec exists with at least two worked examples.
- **CF3.2** Replace the nil placeholder with an elaboration that produces the
  composed handler per the spec. *Done when:* a fixture composing two effect
  handlers produces the spec's expected stdout.
- **CF3.3** Fixtures: compose two independent effects; compose with an
  overlapping effect (define the precedence outcome from CF3.1). *Done when:*
  both run green and are snapshotted.
- **CF3.4** Update `effects-system-guide.md` to document the composition
  semantics (no silent-nil description remains). *Done when:* the guide
  reflects the shipped behavior.

---

## Phase CF4 -- Gate `call/cc` / `escape` behind `-Xcallcc` (audit item 1)

Real first-class capture needs the CPS pass (post-1.0). **Decision
(2026-05-30): gate behind an experimental `-Xcallcc` flag.** Ungated,
`call/cc`/`escape` raise a compile error; with `-Xcallcc` the current
(no-real-capture) desugar is unlocked for experimentation and clearly
documented as unsound. This avoids shipping a form that silently hands the
user the integer `0` as a "continuation".

- **CF4.1** Add the `-Xcallcc` experimental flag (default off) and record it in
  CF0.2 with its diagnostic wording. *Done when:* the flag is recognized and
  its help text states "no real capture yet (unsound); requires the post-1.0
  CPS pass".
- **CF4.2** Make ungated `call/cc`/`escape` raise an elaboration error that
  points at the post-1.0 CPS entry; under `-Xcallcc`, retain the current
  desugar unchanged. *Done when:* a program using `call/cc` fails to compile
  without the flag and compiles (with the documented caveat) with it.
- **CF4.3** Move `continuation-callcc` / `continuation-escape` fixtures behind
  `-Xcallcc`, and add an expect-error fixture for the ungated case. *Done
  when:* suite stays green with the new expectations and snapshots are updated.
- **CF4.4** Document `call/cc`/`escape` status and the `-Xcallcc` flag in the
  relevant guide, linking the control-flow audit's post-1.0 CPS entry. *Done
  when:* docs state the flag and that capture is unsound until CPS lands.

---

## Phase CF5 -- Generator limitation diagnostics (audit item 4)

`yield` inside `match` arms and recursive generators are unsupported v1
limitations that fall out of the `may_capture`/no-full-CPS design. Make them
explicit compile-time diagnostics rather than surprises, and document them
prominently for 1.0.

- **CF5.1** Detect `yield`/`yield*` appearing in an unsupported position
  (inside a `match` arm; inside a recursive generator) during elaboration.
  *Done when:* each unsupported placement is identified by analysis.
- **CF5.2** Emit a precise diagnostic naming the limitation and pointing to the
  post-1.0 CPS plan, instead of mis-lowering. *Done when:* the two unsupported
  patterns fail with that diagnostic.
- **CF5.3** Add expect-error fixtures for `yield`-in-`match` and recursive
  generator; keep an expect-ok fixture for supported `yield`/`yield*`.
  *Done when:* all three run green and are snapshotted.
- **CF5.4** Promote the limitation note in `generators-guide.md` from a buried
  line to a prominent "Limitations (1.0)" section. *Done when:* the section
  exists and links this phase.

---

## Phase CF6 -- Async Send-across-await soundness (audit item 6)

The boundary Send check exists but does not enforce that all values *live
across* an await point are Send -- a soundness hole. For 1.0, close the hole
or scope the async surface down to the checked subset.

- **CF6.1** Define the precise obligation: a value live across an await must be
  Send; enumerate what "live across await" must include (locals held over the
  await, captured-by-continuation state). *Done when:* the obligation is
  written with the liveness inputs it needs.
- **CF6.2** Decide scope for 1.0: (a) approximate liveness conservatively
  (reject more, sound) or (b) restrict async to constructs where the existing
  boundary check is already sufficient and reject the rest. *Done when:* the
  chosen scope is recorded with its false-positive cost.
- **CF6.3** Implement the chosen check so a non-Send value held across an await
  is rejected. *Done when:* a fixture holding a non-Send value across await
  fails; a Send-only async fixture still passes.
- **CF6.4** Fixtures: non-Send-across-await (expect-error), Send-across-await
  (expect-ok), and a regression for each existing async fixture. *Done when:*
  all green and snapshotted; existing async fixtures unaffected.
- **CF6.5** Document the rule and any conservative rejections in
  `async-await-guide.md`. *Done when:* the guide states the Send-across-await
  rule.

---

## Phase CF7 -- Cloneable continuation deep clone + capture precision (audit items 7, 8)

Cloneable-continuation cloning is a v1 bitwise copy with the Drop typeclass
path unimplemented, and the cloneable-`shift` capture check is conservative
(covers every binding in scope). Tighten both to the extent possible without
the full CPS pass.

- **CF7.1** Replace the bitwise continuation clone with a field-by-field deep
  clone for captured owning/heap state. *Done when:* cloning a continuation
  that captures heap state yields independent copies (no aliasing) under a
  fixture that mutates one clone and checks the other.
- **CF7.2** Implement the Drop typeclass path for cloned continuations so owned
  resources are released once per clone. *Done when:* an ASan/LSan run over a
  clone-and-drop fixture is leak-clean (per CLAUDE.md leak policy).
- **CF7.3** Tighten the cloneable-`shift` capture check from "every binding in
  scope" toward the bindings actually referenced by the continuation body
  (best-effort without full CPS liveness). *Done when:* a previously-rejected
  valid program now type-checks, while a genuinely uncloneable capture is
  still rejected.
- **CF7.4** Fixtures: deep-clone independence, clone-and-drop leak check, and a
  capture-precision accept/reject pair. *Done when:* all green; leak fixtures
  run with detection ON per CLAUDE.md.
- **CF7.5** Note the remaining liveness imprecision (full precision is gated on
  CPS) in the backtracking / serializable-continuations guides. *Done when:*
  the residual limitation is documented and linked to the post-1.0 CPS entry.

---

## Exit criteria for 1.0 (control-flow)

- No shipped control-flow form silently returns a placeholder: `call/cc`/
  `escape` (CF4) and, if not implemented, `compose-handlers` (CF3) fail
  loudly; `shift` result typing (CF2) is real.
- Self-tail-call loops do not overflow (CF1).
- Async has no Send-across-await soundness hole within its 1.0 scope (CF6).
- Cloneable continuation cloning is deep and leak-clean; capture is no longer
  needlessly conservative (CF7).
- Every remaining limitation (generator restrictions, residual capture
  imprecision) is a diagnostic and/or a prominent doc note, not silent wrong
  behavior (CF5, CF7.5).
- `bash tests/run.sh` reports zero `FAIL` lines and all fixture snapshots are
  regenerated per CLAUDE.md.

## See also

- [control-flow-completeness-audit.md](control-flow-completeness-audit.md)
- [typing-gap-plan.md](typing-gap-plan.md)
- [effects-system-guide.md](../guides/effects-system-guide.md)
- [generators-guide.md](../guides/generators-guide.md)
- [async-await-guide.md](../guides/async-await-guide.md)
