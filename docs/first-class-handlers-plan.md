---
title: First-Class Effect Handlers -- Implementation Plan
category: Language Features
description: Phased plan to give effect handlers a first-class runtime representation so handler values can be created, passed, applied, and composed (unblocking compose-handlers)
---

# First-Class Effect Handlers -- Implementation Plan

> **Status:** **Implemented** (FH0-FH7, including FH4.1). Handler values have a
> runtime representation (an effect-keyed dispatch table); they can be created
> (`(handler ...)`), applied (`(with-handler hv body)`), and composed
> (`(compose-handlers h1 h2)`), and the `TY_HANDLER` type now carries the
> handled effect *set* (single- or multi-effect via `#{...}`). The CF3
> `TUR-E0704` gate has been removed.
>
> Follow-up to
> [control-flow-completeness-plan.md](archive/control-flow-completeness-plan.md)
> Phase CF3, which originally **gated** `compose-handlers` (diagnostic
> `TUR-E0704`) because handler *values* had no runtime representation.
>
> **Snapshot:** `0.14.6`.
>
> **Last updated:** 2026-05-30

---

## Problem statement

Today the effect-handler surface is split into two disconnected halves:

1. **Inline handling works.** `(handle body (Effect [args] k) case-body ...)`
   elaborates (`elab_handle`, `src/compiler/elab_effects.c`) and lowers to a
   fiber-based dispatcher (`emit_effects_handle`, `src/compiler/emit_effects.c`):
   the body runs in a fiber, `perform` yields to a dispatch function that
   routes on the effect name to a generated `__effect_handler_<id>` case
   function, binds the continuation `k`, and resumes. The runtime handler shape
   is `typedef struct { void *env; int64_t (*fn)(int64_t *, int, int64_t, void *); } tur_handler_t;`.

2. **Handler *values* are type-only.** `(handler E V R)` is a type annotation
   producing `TY_HANDLER` (`elab_types.c`; carries `effect_name`,
   `value_kind`, `result_kind`, `cont_kind`). A parameter can be typed
   `:(handler Write cstr nil)`, but **there is no form that creates a handler
   value, and no form that applies one**. `with-handler` is merely sugar for
   the inline `handle`.

Because of (2), `compose-handlers h1 h2` -- which takes two `TY_HANDLER`
*values* -- has nothing real to return; it type-checked its arguments, rejected
same-effect overlap (`TUR-E0251`), and then returned a nil placeholder. CF3
replaced that silent nil with a loud gate (`TUR-E0704`). This plan closes the
gap by making handlers first-class.

### Goal

A handler is a value. You can:

- **create** one (a handler literal carrying its cases, detached from a body),
- **pass** it (already typed `TY_HANDLER`),
- **apply** it to a body (run the body with that handler installed), and
- **compose** two of them into one that dispatches across both effect sets.

with the existing continuation discipline (`^linear` / `^multishot` / default
affine `k`) preserved per case.

---

## Design overview

The key move is to **detach a handler's cases from a body** and give them a
runtime representation, then re-express both `handle` and `compose-handlers` on
top of that representation.

- **Handler value (runtime):** an array of per-effect entries, each carrying
  the effect name, the generated case function pointer, its captured-env
  pointer, the continuation discipline, and the value/result kinds. This is a
  generalization of the existing `tur_handler_t` from one function to an
  effect-keyed table. A single-effect handler literal yields a one-entry table;
  `compose-handlers` concatenates two tables.

- **Handler value (type):** `TY_HANDLER` already exists for the single-effect
  case. Composition needs a handled-effect *set*, so either (a) generalize
  `TY_HANDLER` to carry an effect *row* (preferred -- reuses `EffectRow`,
  `effect_row_union`, `effect_row_merge` from `src/passes/effect.{h,c}`), or
  (b) introduce `TY_HANDLER_SET`. The effect row also drives which effects the
  applied body is allowed to leave unhandled.

- **Application:** a `with-handler`-style form (or repurposing the existing
  `with-handler` alias) installs a handler value's dispatch table around a body
  using the same fiber machinery `emit_effects_handle` already emits -- the
  dispatcher routes on `__dcap->eff_name` against the table instead of against
  inline cases.

- **Composition = table concat + row union:** `compose-handlers h1 h2`
  produces the concatenation of the two dispatch tables with the unioned effect
  row, still rejecting same-effect overlap (`TUR-E0251`). Applying the composed
  handler is exactly nested application of `h1` outside `h2` (precedence fixed
  below).

---

## Phase ordering at a glance

| Phase | Deliverable | Notes |
|---|---|---|
| FH0 | Semantics spec + decisions | Precedence, overlap, effect-row typing, k discipline |
| FH1 | Handler value runtime representation | Effect-keyed dispatch table; generalizes `tur_handler_t` |
| FH2 | Handler literal form (creation) | `(handler (E [args] k) body ...)` -> `TY_HANDLER` value |
| FH3 | Handler application form | `(with-handler hv body)` installs the table via the fiber dispatcher |
| FH4 | `TY_HANDLER` effect-row generalization | Carry an `EffectRow`; type application + leftover effects |
| FH5 | `compose-handlers` real elaboration | Table concat + row union; replace the `TUR-E0704` gate |
| FH6 | Continuation discipline across composition | `^linear`/`^multishot`/affine per case under composition |
| FH7 | Fixtures + docs | Independent compose, overlap reject, runtime stdout, guide update |

---

## Phase FH0 -- Semantics specification

> **Status: DONE.** The operational spec lives in
> [first-class-handlers-semantics.md](first-class-handlers-semantics.md).
> FH0.1-FH0.4 are written there (precedence + worked trace, overlap rule,
> typing judgment, continuation discipline).

Write the operational spec before code so FH5's behavior is unambiguous.

- **FH0.1** Composition precedence. Define `(compose-handlers h1 h2)` applied to
  `body` as equivalent to `(with-handler h1 (with-handler h2 body))` -- i.e.
  `h1` is the *outer* handler. State the consequence: an effect handled by
  neither bubbles out; an effect performed in `body` is routed to whichever
  handler declares it (they are disjoint by the overlap rule). *Done when:* the
  equivalence and a worked two-effect trace are written down.
- **FH0.2** Overlap. Keep `TUR-E0251` (same effect handled twice) as a hard
  error -- composition is only defined for disjoint effect sets in v1. Record
  whether a later "leftmost wins" relaxation is desired (default: no). *Done
  when:* the rule is stated.
- **FH0.3** Effect-row / result typing. `compose-handlers : handler<R1, _, T>
  -> handler<R2, _, T> -> handler<R1 ∪ R2, _, T>` with `R1 ∩ R2 = {}`. Define
  the result-type relationship between the two handlers (must agree on the
  answer type `T`, or define a coercion). *Done when:* the typing judgment is
  written with its side conditions.
- **FH0.4** Continuation discipline. Define how each case's `cont_kind`
  (`CK_UNIQUE` / `CK_LINEAR` / `CK_MULTISHOT`) is preserved when the case lives
  in a handler value and when two handlers compose. *Done when:* the rule is
  stated (per-case discipline is independent across composition).

---

## Phase FH1 -- Handler value runtime representation

- **FH1.1** Define the dispatch-table struct: an array of
  `{ const char *eff_name; int64_t (*fn)(int64_t *args, int n, int64_t k, void *env); void *env; uint8_t cont_kind; }`
  entries, plus the handled-effect count. Generalizes the current one-function
  `tur_handler_t`. *Done when:* the C struct is emitted in the runtime preamble
  (`emit_module.c`) and documented.
- **FH1.2** Decide ownership/lifetime of the env pointers (the existing handle
  path stack-allocates env then runs the fiber synchronously; a *value* may
  outlive its definition site). Specify heap allocation + a drop path, or
  restrict handler-value escape. *Done when:* the lifetime rule is recorded and
  is ASan/LSan-clean per CLAUDE.md.

---

## Phase FH2 -- Handler literal (creation)

- **FH2.1** Add a value form that produces a single-effect handler value, e.g.
  `(handler (Write [s] k) (do (println s) (resume k nil)))`. Reuse
  `HandleCase` elaboration (effect name, params, `k`, `cont_kind`, body) but
  emit a one-entry dispatch table instead of wiring it to a body. *Done when:*
  the form elaborates to a `TY_HANDLER` value and a fixture binds it to a
  `:(handler Write cstr nil)` parameter.
- **FH2.2** Reuse the existing `__effect_handler_<id>` case-function emission so
  literal and inline handlers share one codegen path. *Done when:* a handler
  literal and the equivalent inline `handle` emit the same case function shape.

---

## Phase FH3 -- Handler application

- **FH3.1** Add `(with-handler hv body)` (or a new keyword) that installs `hv`'s
  dispatch table around `body` using the fiber dispatcher. Today `with-handler`
  aliases `handle`; either repurpose it (value argument vs inline cases,
  disambiguated by syntax) or add `handle-with`. *Done when:* a body that
  performs the handler's effect runs through the value's case and resumes.
- **FH3.2** Generalize `emit_effects_handle`'s dispatch so it can route against
  a runtime table (handler value) as well as compile-time inline cases. *Done
  when:* inline `handle` and `with-handler hv` share the dispatch loop and a
  runtime fixture prints the expected output.

---

## Phase FH4 -- `TY_HANDLER` effect-row generalization

> **Status:** FH4.1 and FH4.2 **done**.
>
> FH4.1: `TY_HANDLER` now carries a `handled_row` (an `EffectRow` stored as an
> `ERK_UNRESOLVED` name-set, built at parse time -- no `Effect*` resolution
> needed). `(handler E V R)` accepts either a single effect symbol or a
> `#{E1 E2 ...}` effect set; `effect_row_collect_names` /
> `effect_row_name_set_eq` / `effect_row_format_names` (in `effect.c`) back the
> updated `type_eq`, `type_name` ("handler<A | B, V, R>"), and subtyping in
> `types.c` (`TY_UNKNOWN` value/result kinds act as wildcards so a composed
> handler round-trips through a typed parameter). `compose-handlers` sets the
> unioned `handled_row` and rejects overlap by name-set intersection.
>
> FH4.2: `effect_check`'s `collect_effects_in_expr` treats
> `(with-handler hv body)` like `(handle ...)` -- it discharges the handler's
> effect(s) from the body's row (`remove_handler_effects`, which recurses
> structurally into handler literals and compositions) and propagates leftover
> body effects plus effects re-opened by the case bodies. A leftover effect is
> reported through the same diagnostics as an inline `handle` (`TUR-E0009` when
> it violates a declared row, `TUR-W0030` when a function has no annotation) --
> the plan's reference to `TUR-E0253` predates the unified row-mismatch
> reporting that `handle` itself uses.
>
> *Note:* one call-site arg-checker path compares handler arguments by kind
> only (it falls back to `type_from_kind` when a function's full parameter types
> aren't threaded through), so it does not yet exercise the row-precise
> `type_eq`; that is pre-existing plumbing, independent of the type-level FH4.1
> operations, which are correct. This caveat, together with the related
> mixed-ownership `type_name` diagnostic leak, is tracked in
> [handler-typecheck-and-typename-followups-plan.md](handler-typecheck-and-typename-followups-plan.md).

- **FH4.1** Extend `TY_HANDLER` to carry an `EffectRow` (handled set) rather
  than a single `effect_name`, keeping the single-effect constructor as a
  one-element row for source compatibility. *Done when:* `type_eq`,
  `type_name`, subtyping (`types.c`), and `(handler ...)` parsing handle the
  row form.
- **FH4.2** Thread the handled row into application so the body's residual
  effect row = body effects minus handled row (reuse `effect_row_remove`).
  *Done when:* applying a handler discharges its effects from the row and a
  leftover effect is still reported (`TUR-E0253`).

---

## Phase FH5 -- `compose-handlers` real elaboration

> **Status: DONE.** `elab_compose_handlers` now produces an
> `EX_COMPOSE_HANDLERS` value (the `TUR-E0704` gate is removed; `TUR-E0251`
> overlap rejection retained). `emit_effects_compose_handlers` lowers it to
> `tur_handler_table_concat` (h1 outer, per FH0.1). Applying a composed handler
> is byte-identical to nested `handle` (fixture `fh-compose-handlers`). The
> CF-plan CF3 outcome and gated-diagnostic list are updated; the
> `compose-handlers-gated` fixture is removed.

- **FH5.1** Replace the `TUR-E0704` gate (in `elab_compose_handlers`) with an
  elaboration that concatenates the two dispatch tables and unions the effect
  rows (`effect_row_union`), keeping the `TUR-E0251` overlap rejection. *Done
  when:* `(compose-handlers h1 h2)` yields a `TY_HANDLER` value over `R1 ∪ R2`.
- **FH5.2** Make application of a composed handler equal to nested application
  per FH0.1. *Done when:* a fixture composing two independent effects produces
  the FH0.1 expected stdout, identical to the hand-written nested `handle`.
- **FH5.3** Remove `TUR-E0704` from the gated list in
  [control-flow-completeness-plan.md](archive/control-flow-completeness-plan.md) CF0.2
  and update CF3's outcome. *Done when:* the gate is gone and the plan reflects
  the implemented state.

---

## Phase FH6 -- Continuation discipline across composition

> **Status: DONE.** `elab_handler_lit` wires `cont_kind` into the `k` binding
> exactly like `elab_handle` (linear `is_linear`/`is_relevant`, default affine
> move, multishot snapshot), and now also carries the MS2 multishot-capture
> check. Each case keeps its own discipline in its dispatch-table entry, so
> composition does not blend disciplines. Expect-error fixtures confirm the same
> diagnostics fire through a handler value: `TUR-E0101` (linear resumed twice),
> `TUR-E0100` (linear dropped), `TUR-E0201` (affine resumed twice), `TUR-E0500`
> (multishot captures a unique). A positive fixture shows a `^multishot` handler
> value resumed twice matches the inline `handle` result.

- **FH6.1** Verify `^linear` / `^multishot` / default-affine `k` discipline is
  enforced per case when the case lives in a handler value and across
  composition (each handler keeps its own discipline). *Done when:* expect-error
  fixtures for misused `k` fire the same diagnostics (`TUR-E0101` linear,
  `TUR-E0201` unique, `TUR-E0500`-series multishot) through a handler value.

---

## Phase FH7 -- Fixtures and documentation

> **Status: DONE.** Fixtures (FH7.1): `fh-handler-value` (literal + applied,
> runtime stdout); `fh-compose-handlers` (two independent effects, equal to
> nested `handle`); `errors/fh-compose-overlap` (`TUR-E0251`);
> `errors/fh-leftover-effect` + `fh-discharge-row` (FH4.2 discharge/leftover);
> `errors/fh-{linear-twice,linear-dropped,unique-twice,multishot-capture}` and
> `fh-multishot-value` (FH6 discipline). FH7.2: `effects-system-guide.md` now
> documents the shipped creation/application/composition semantics and the CF3
> gate note is removed.

- **FH7.1** Fixtures: (a) handler literal bound + applied (runtime stdout);
  (b) compose two independent effects, run, compare to nested `handle`;
  (c) compose-overlap still `TUR-E0251`; (d) leftover-effect still `TUR-E0253`;
  (e) k-discipline negatives. *Done when:* all green and snapshotted per
  CLAUDE.md.
- **FH7.2** Update `effects-system-guide.md` from "compose-handlers is gated"
  to the implemented semantics (creation, application, composition, precedence).
  *Done when:* the guide documents the shipped behavior and the CF3 gate note is
  removed.

---

## Risks and open questions

- **Escape / lifetime.** Inline `handle` runs its fiber synchronously with a
  stack-allocated env; a handler *value* can escape its defining scope. FH1.2
  must pin down heap allocation + drop without leaking (ASan/LSan ON per
  CLAUDE.md). This is the main implementation risk.
- **Answer-type agreement.** FH0.3 must decide whether composed handlers must
  share the answer type `T` or whether a coercion is allowed; the simplest
  sound v1 is strict equality.
- **Multishot under composition.** Snapshot-based multishot (`MS1`) interacts
  with a table-driven dispatcher; FH6 must confirm clone semantics still hold
  when the case comes from a value.
- **CPS adjacency.** None of the above requires the full CPS pass (the fiber
  runtime already provides capture/resume); first-class handlers are an
  orthogonal, fiber-level feature. This is why it is tractable independently of
  the post-1.0 CPS work.

## See also

- [control-flow-completeness-plan.md](archive/control-flow-completeness-plan.md) (Phase CF3)
- [control-flow-completeness-audit.md](archive/control-flow-completeness-audit.md) (audit item 2)
- [effects-system-guide.md](guides/effects-system-guide.md)
