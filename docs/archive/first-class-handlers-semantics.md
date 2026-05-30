---
title: First-Class Effect Handlers -- Operational Semantics (FH0)
category: Language Features
description: Operational specification for first-class effect handler values -- creation, application, composition precedence, overlap, effect-row typing, and continuation discipline
---

# First-Class Effect Handlers -- Operational Semantics (FH0)

> **Status:** Specification (Phase FH0 of
> [first-class-handlers-plan.md](first-class-handlers-plan.md)).
> Written before the implementation phases (FH1+) so the runtime behaviour
> of `compose-handlers` and handler application is unambiguous.
>
> **Snapshot:** `0.14.6`.
>
> **Last updated:** 2026-05-30

This document is the FH0 deliverable: it pins down the operational meaning of a
first-class handler *value* so that the codegen phases (FH1-FH6) implement one
agreed semantics. Each numbered section corresponds to an FH0 sub-item in the
plan.

---

## Vocabulary

- A **handler value** `hv` is a runtime value of type `(handler R V T)` (after
  FH4: `R` is the *handled effect row*, `V` an effect value summary, `T` the
  answer type). It carries an effect-keyed **dispatch table**: one entry per
  handled effect, each holding the effect name, a generated case function, its
  captured environment, and the continuation discipline (`cont_kind`).
- **Creation** is the handler literal form (FH2):
  `(handler (E [args] k) body ...)`.
- **Application** is `(with-handler hv body)` (FH3): run `body` with `hv`'s
  table installed around it via the existing fiber dispatcher.
- **Composition** is `(compose-handlers h1 h2)` (FH5): a new handler value whose
  table is the concatenation of `h1`'s and `h2`'s tables.

---

## FH0.1 -- Composition precedence

**Rule.** For any body `b`,

```
(with-handler (compose-handlers h1 h2) b)
   ==  (with-handler h1 (with-handler h2 b))
```

That is, **`h1` is the *outer* handler and `h2` is the *inner* handler.** The
composed dispatch table is `table(h1) ++ table(h2)` (h1's entries first), and
when the dispatcher scans the table on a performed effect it uses the first
matching entry. Because the overlap rule (FH0.2) guarantees `h1` and `h2`
handle disjoint effect sets, the scan order is observable only for effects that
*neither* handles (they bubble out unchanged) -- there is never an effect that
both could claim, so "h1 is outer" and "first match wins" agree.

**Consequences.**

- An effect performed in `b` and declared by `h1` is routed to `h1`'s case.
- An effect performed in `b` and declared by `h2` is routed to `h2`'s case.
- An effect declared by neither bubbles past both, exactly as it would past two
  nested `handle` forms, and surfaces as a leftover effect in the residual row
  (FH4.2 / `TUR-E0253`).

**Worked two-effect trace.** Let `h1` handle `Ask` (returns a constant) and
`h2` handle `Tell` (prints, then resumes):

```
hv = (compose-handlers h1 h2)        ; table = [Ask->c1 ; Tell->c2]
(with-handler hv
  (do
    (perform Tell "log line")        ; (1)
    (+ (perform Ask nil) 1)))        ; (2)
```

1. `perform Tell "log line"` -- the fiber yields; the dispatcher scans
   `[Ask, Tell]`, matches `Tell` at index 1, runs `h2`'s case (prints
   `log line`), which resumes `k` with `nil`. Control returns after `(1)`.
2. `perform Ask nil` -- the fiber yields; the dispatcher scans `[Ask, Tell]`,
   matches `Ask` at index 0, runs `h1`'s case, which resumes `k` with the
   constant. The body computes `constant + 1` and that becomes the answer.

The observable behaviour is identical to
`(with-handler h1 (with-handler h2 (do ...)))`: `Tell` is intercepted by the
inner-or-outer handler that declares it (here `h2`), `Ask` by `h1`, and the
answer type is `T`.

---

## FH0.2 -- Overlap

**Rule (hard error, retained).** It is a compile-time error
(`TUR-E0251`) for `h1` and `h2` to handle the same effect. Composition is
**defined only for disjoint effect sets** in v1: `R1 ∩ R2 = {}`.

This is enforced before any composition codegen runs (FH5.1 preserves the
existing `TUR-E0251` check in `elab_compose_handlers`).

**Future relaxation (recorded, not adopted).** A "leftmost wins" relaxation --
where overlapping effects would resolve to `h1` (the outer/leftmost handler) --
is *deliberately not* adopted in v1. Rationale: silent shadowing of an effect
handler is a frequent source of bugs, and the disjoint-set rule keeps
composition associative and commutative *up to the answer-type side condition*.
If a future phase wants it, it should be opt-in (e.g. a `compose-handlers/left`
variant) rather than a change to the default. **Default: no relaxation.**

---

## FH0.3 -- Effect-row / result typing

**Judgment.**

```
   h1 : (handler R1 _ T)      h2 : (handler R2 _ T)      R1 ∩ R2 = {}
   ---------------------------------------------------------------------
        (compose-handlers h1 h2) : (handler (R1 ∪ R2) _ T)
```

**Side conditions.**

1. **Disjointness:** `R1 ∩ R2 = {}` (FH0.2 / `TUR-E0251`).
2. **Answer-type agreement (strict equality, v1):** both handlers must produce
   the *same* answer type `T`. The composed handler also answers `T`. No
   coercion is performed in v1 -- a mismatch is a type error. (The simplest
   sound rule; a coercion story is left to a future phase, see Risks in the
   plan.)
3. **Value summary `_`:** the middle `V` parameter of `TY_HANDLER` is the
   single-effect value summary retained for source compatibility. Under a row
   it is not meaningful across multiple effects and is left unconstrained
   (printed as `_`); composition does not require the two `V`s to agree.

**Application and the residual row.** Applying a handler discharges its handled
row from the body's effect row:

```
   body : T ! Rb        hv : (handler Rh _ T)
   -----------------------------------------------------
        (with-handler hv body) : T ! (Rb \ Rh)
```

where `\` is `effect_row_remove` applied for each effect in `Rh`. Any effect in
`Rb` but not in `Rh` remains in the residual row and, if it reaches a context
that does not handle it, is reported as a leftover effect (`TUR-E0253`).

---

## FH0.4 -- Continuation discipline

**Rule.** Each handler case carries its own continuation discipline
`cont_kind ∈ { CK_UNIQUE (default affine), CK_LINEAR, CK_MULTISHOT,
CK_COPY (deprecated) }`. This discipline is a property of the *case*, fixed at
the handler literal (FH2), and **is preserved unchanged** when the case lives in
a handler value and when two handlers compose:

- A case's `cont_kind` is stored in its dispatch-table entry (FH1) and is read
  back when the case runs, regardless of whether it was reached through
  `(with-handler hv ...)` or through a composed table.
- **Composition does not blend disciplines.** In `(compose-handlers h1 h2)`,
  each entry keeps the `cont_kind` it had in its source handler. Because the
  effect sets are disjoint (FH0.2), no effect's discipline is ambiguous: the
  single entry that handles a given effect dictates the discipline for that
  effect's continuation. There is no "join" of `CK_LINEAR` and `CK_MULTISHOT`.

**Per-case discipline is independent across composition.** Misusing a
continuation (e.g. resuming a `CK_LINEAR` k twice, or a `CK_UNIQUE` k after it
was moved) is rejected with the same diagnostics whether the case was written
inline in a `handle`, bound in a handler literal, or composed:

| Discipline | Misuse | Diagnostic |
|---|---|---|
| `CK_LINEAR` | resumed/discontinued more than once, or zero times | `TUR-E0101` |
| `CK_UNIQUE` | resumed/discontinued after move | `TUR-E0201` |
| `CK_MULTISHOT` | resumed under `atomically` (STM retry hazard) | `TUR-E0500`-series |

FH6 verifies these fire through a handler value identically to the inline path.

---

## Invariants the implementation must uphold

1. **Single dispatch path.** `handle` (inline) and `with-handler hv` share one
   fiber dispatch loop; the only difference is whether the dispatcher scans
   compile-time inline cases or a runtime table (FH3.2).
2. **Table = concat.** `compose-handlers` is exactly table concatenation plus
   `effect_row_union`; it introduces no new dispatch mechanism (FH5.1).
3. **Nested == composed.** `(with-handler (compose-handlers h1 h2) b)` must
   produce byte-identical observable output to
   `(with-handler h1 (with-handler h2 b))` for disjoint, terminating handlers
   (FH5.2 fixture).
4. **No leaks.** Handler values may escape their defining scope; their captured
   environments are heap-managed with a drop path that is ASan/LSan-clean
   (FH1.2).

---

## See also

- [first-class-handlers-plan.md](first-class-handlers-plan.md) (the phased plan)
- [control-flow-completeness-plan.md](archive/control-flow-completeness-plan.md) (Phase CF3, the `TUR-E0704` gate)
- [effects-system-guide.md](guides/effects-system-guide.md)
