---
title: "fn-type Colon Codemod -- Sweet-Exp Typeclass Heads -- Plan"
category: Tooling (Plan)
description: Extend tools/rewrite_fn_type_colons.py so its sweet-exp implicit-sequence walk also reaches definstance/defclass/defprotocol method signatures, closing the one head family deliberately left out of the original sweet-exp coverage work.
---

# Plan: fn-type Colon Codemod -- Sweet-Exp Typeclass Heads

This is a **follow-up** to
[fn-type-colons-sweet-exp-plan.md](fn-type-colons-sweet-exp-plan.md),
which gave `tools/rewrite_fn_type_colons.py` an implicit-sequence walk so
sweet-exp top-level forms (`.tur.sweet` files and ` ```sweet-exp ` doc
blocks) get their `(fn ...)` inner-type colons rewritten. That work
**deliberately scoped the flat scan to `DEFN_HEADS`, `STRUCT_HEADS`,
`LET_HEADS`, and `ALIAS_HEADS`** (see the predecessor plan's Design
section, "Add `_walk_implicit_seq`"). The typeclass heads --
`definstance`, `defclass`, `defprotocol` (`INSTANCE_HEADS`) -- were left
out.

This plan closes that remaining head family **only if a real source needs
it**. As of the predecessor's landing the gap is purely a coverage hole
under the "missed, never corrupted" guarantee, so this is a *completeness*
task with no correctness urgency.

---

## Motivation

### The gap

In traditional s-expressions, `walk()` dispatches `INSTANCE_HEADS` to
`_process_instance`, which treats each inner paren child as a method
signature `(name [params] #{eff}? :ret body?)` and runs
`_process_signature(child, name_slots=1)` over it. So fn types in method
return/parameter slots are rewritten today:

```turmeric
(defclass HasArr [a]
  (arr-of [self n : int] : (fn [int] int)))   ;; <- already handled
```

In sweet-exp, both the **outer** `defclass`/`definstance`/`defprotocol`
head **and** each **method signature** drop their parens and lay out by
indentation:

```
#lang sweet-exp

defclass HasArr [a]
  arr-of [self n :int] :(fn [:int] :int)      ;; <- MISSED
```

Here nothing is paren-wrapped at the declaration level:

- The top-level run is `defclass`, `HasArr`, `[a]`, then the method
  forms. `_walk_implicit_seq` scans for head symbols but `definstance`/
  `defclass`/`defprotocol` are **not** in any of the four head sets it
  checks, so it skips straight past.
- Each method `arr-of [self n :int] :(fn [:int] :int)` is itself a flat
  `name [params] :ret` run -- a "defn signature without the `defn`
  keyword". The normal paren walk never sees a `(name [params] :ret)`
  paren form to hand to `_process_instance`, because there is no paren.

Net effect: the method's `(fn [:int] :int)` return type keeps its legacy
inner colons. **Missed, never corrupted** -- the `(fn ...)` form is still
paren-delimited, and when the paren walk reaches it standalone it matches
`fn` in `DEFN_HEADS` and treats it as a lambda *value* (whose fused `:int`
slots are correctly left untouched).

### Why it was deferred

The predecessor plan's "What stays out of scope" reasoning applies: the
inner rewriter is unchanged and the legacy spelling still parses (Phase 1
of the parent "Drop leading colons inside `(fn ...)` types" work landed
the lenient path). The typeclass-head shape is also **materially harder**
than the four heads already handled -- see below -- so it was correctly
left until a real source exercises it.

---

## Current State

- `tools/rewrite_fn_type_colons.py`:
  - `_walk_implicit_seq(nodes, stats)` scans a flat sibling sequence for
    `DEFN_HEADS` / `STRUCT_HEADS` / `LET_HEADS` / `ALIAS_HEADS` and runs
    the shared slot processors (`_process_signature_items`,
    `_process_struct_field_vec`, `_process_let_binding_vec`,
    `process_type_slot`). It has **no** `INSTANCE_HEADS` branch.
  - `_process_instance(form, stats)` (the paren-walk handler) iterates the
    form's paren children and calls `_process_signature(child,
    name_slots=1)` on each. It assumes paren-delimited method signatures.
- The sweet-exp corpus under `tests/codemod/fn-type-colons/`
  (`sweet-basic`, `sweet-effects`, `sweet-lambda-untouched`, `sweet-md`)
  has **no** typeclass case.

### Why typeclass heads are harder than defn/struct/let

The four currently-handled heads each have a **fixed, single** slot shape
the flat scan consumes once and then stops:

- `defn NAME [vec] :ret` -- one param vec + one return slot.
- `defstruct NAME [fields]` -- one field vec.
- `let [bindings]` -- one binding vec.
- `defalias NAME TYPE` -- one type slot.

`definstance`/`defclass`/`defprotocol` instead introduce a **variable-
length list of method signatures** as indented body siblings, each of
which is itself a `name [params] #{eff}? :ret` signature. The flat scan
must therefore:

1. Recognize the typeclass head and consume its `CLASS`/`TYPE`/superclass
   preamble (which differs across the three heads), then
2. Iterate the **following** sibling forms, and for each one that looks
   like a method signature (a bare **symbol** immediately followed by a
   `[...]` bracket), run `_process_signature_items` over it -- while
   **not** mis-consuming a non-method body form or the next top-level
   declaration.

Boundary detection ("is this sibling a method head or the start of
something else?") is the crux, and is exactly why this was split out.

---

## Design

### Reuse, don't duplicate

`_process_signature_items(code, start, stats)` already exists and is the
right primitive -- it rewrites a param vec + return-type slot given a
`(orig_index, node)` code list and a start index, independent of `Form`.
A method signature is just a signature with `name_slots = 1` (the method
name, no leading `defn` keyword). So the new logic is **head recognition +
method-boundary iteration**, not new slot code.

### Method-boundary heuristic

Within the body of a sweet-exp typeclass form, treat a sibling as a method
signature **iff** it is a bare `SYMBOL` immediately followed (in the
code-item sequence) by a `[...]` bracket `Form`. Run
`_process_signature_items(code, k+1, stats)` starting just past the method
name. Anything else (a paren call, a non-bracket-led form) is a body
expression and is left to the normal paren walk.

This mirrors how `_process_instance` already trusts the
`name [params] :ret` shape, lifted to the flat sequence.

### Preamble consumption per head

- `definstance CLASS TYPE ...methods` -- skip two leading code items
  (class symbol + type form) before iterating methods.
- `defclass NAME [tyvars] ...methods` -- skip the name and the optional
  `[tyvars]` bracket.
- `defprotocol NAME [tyvars]? ...methods` -- same shape as `defclass`.

A superclass/constraint preamble (e.g. `defclass (Ord a) => ...`) is rare
in sweet-exp; if present it appears as a paren form the heuristic simply
won't treat as a method (no bare-symbol-then-bracket), so it is skipped
safely -- document this as "missed, never corrupted" rather than
special-casing it in v1.

### Where the scan stops

Because the flat scan has no indentation information, it cannot know where
the typeclass body ends. Rely on the **method-shape heuristic** as the
boundary: keep consuming siblings that match `symbol + [bracket]` as
methods; the first sibling that does not match ends the method run, and
the outer `while` resumes normal head scanning from there. The next
top-level `defn`/`defstruct`/etc. is then picked up as usual. Cover the
"body form that is not a method" and "two adjacent typeclass decls" cases
in the corpus to lock the boundary down.

### Out of scope (unchanged from predecessor)

- Indentation-nested type forms deeper than the flat scan reaches stay
  "missed, never corrupted."
- Neoteric / curly-infix never carry type annotations.

---

## Phases

### Phase T1 -- Implicit-seq `INSTANCE_HEADS` branch

- [ ] Add an `INSTANCE_HEADS` branch to `_walk_implicit_seq`: consume the
      per-head preamble, then iterate method-shaped siblings calling
      `_process_signature_items(code, name_idx + 1, stats)`.
- [ ] No change to `_process_instance` (paren-walk handler) or the inner
      rewriter.

### Phase T2 -- Corpus

- [ ] Add sweet-exp corpus cases under `tests/codemod/fn-type-colons/`:
  - `sweet-instance` -- `definstance` with a method whose return is
    `:(fn [...] ...)`; assert the inner colons are stripped.
  - `sweet-defclass` -- `defclass`/`defprotocol` with a method fn type in
    both a parameter slot and the return slot.
  - `sweet-instance-body-untouched` -- a typeclass body containing a
    non-method body form (and an immediately following top-level `defn`);
    assert the boundary is respected and nothing spurious is rewritten.
- [ ] Confirm idempotency (`--check` clean on every `after.*`) via the
      existing `run-fn-type-colons.sh` (already pairs by extension and
      asserts idempotency).

### Phase T3 -- Re-sweep + doc sync

- [ ] Re-run `--check` over `docs/guides/`, `stdlib/`, `examples/`, and
      (when present) `../turmeric-spices/`; rewrite any now-reachable
      sweet-exp typeclass method fn types in the **same** PR (fixture/guide
      churn is not a deferral reason).
- [ ] `python3 tools/check-guide-pairs.py --tur build/tur docs/guides/`
      -- 0 pairs failed.
- [ ] Update the Phase 2 note in
      [`../archive/still-in-flight-plan.md`](../archive/still-in-flight-plan.md)
      and the predecessor plan's "out of scope" caveat to record that
      typeclass heads are now covered.
- [ ] Drop the module docstring caveat in `rewrite_fn_type_colons.py` that
      calls typeclass heads out, if one was added.

---

## Validation

- `bash tests/codemod/run-fn-type-colons.sh` -- all corpus cases (s-expr +
  sweet, including the new typeclass cases) pass; tool is idempotent.
- `bash tests/run.sh` -- still `0 failed` (annotation-spelling only;
  codegen snapshots byte-identical).
- `tools/check-guide-pairs.py` -- 0 pairs failed before and after.
- Grep gate: after the sweep, no ` :(fn \[` / `: (fn \[` ... `:`-inner
  occurrences remain in swept sweet-exp typeclass bodies (excluding
  `docs/archive/` and intentionally-legacy `tests/fixtures/` sources).

## Risks and edge cases

- **Method-boundary over-reach.** The `symbol + [bracket]` heuristic must
  not swallow a body expression or the next top-level declaration. The
  `sweet-instance-body-untouched` corpus case is the guard; mirror the
  conservative "stop at first non-match" boundary.
- **Mixed paren/sweet typeclass bodies.** A sweet-exp typeclass may still
  use explicit-paren method signatures for some methods. Those are handled
  by the paren walk's `_process_instance`/generic recursion; running both
  walks must not double-rewrite (the `rewritten` flag on `Tok` already
  guards keyword stripping and the bare-`:`-deletion path).
- **Superclass/constraint preambles.** Left as "missed, never corrupted"
  in v1 (see Design); revisit only if a real source needs it.

## References

- [fn-type-colons-sweet-exp-plan.md](fn-type-colons-sweet-exp-plan.md) --
  the predecessor that added `_walk_implicit_seq` and scoped out
  `INSTANCE_HEADS`.
- `tools/rewrite_fn_type_colons.py` -- `_walk_implicit_seq`,
  `_process_instance`, `_process_signature_items`.
- `tests/codemod/run-fn-type-colons.sh` + `fn-type-colons/` corpus.
- [`../archive/still-in-flight-plan.md`](../archive/still-in-flight-plan.md)
  -- "Drop leading colons inside `(fn ...)` types".
