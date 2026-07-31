# Increment 4: the representation decision function (`repr-of`)

**Status:** stage 1 landed 2026-07-31; stages 2-4 proposed.
**Parent:** [representation-consolidation-meta-plan.md](representation-consolidation-meta-plan.md)
(increment 4 -- "the true consolidation; it goes last because by then the
sites agree in *behavior* and the collapse is mechanical rather than
semantic").

## Why now

Increments 0-3 made the sites agree in behavior: the container element
protocol is one predicate (`type_is_boxed_container_elem`), the fn-value
carrier<->fat seam is alias- and join-aware, method results bridge uniformly,
and the fuzzer's full crossing pool runs clean.  What remains is the
*structure* that let them disagree in the first place: the same decision
("what representation does this type have, in this position?") re-derived
independently at many sites.  Every bug in this campaign's archive is a
two-sites-disagree story.  Increment 4 replaces re-derivation with one
routine consulted everywhere.

## The axes

A representation decision has two inputs:

1. **The type** -- its TypeKind plus payload (def, width, heapness,
   signature shape).  Today this axis is answered by three hand-maintained
   switches in `src/compiler/types.c` (`type_c_name`, `append_type_mangle`,
   `type_has_concrete_codegen_layout`) plus predicates layered on top
   (`type_is_wide_byval_adt`, `type_is_boxed_container_elem`,
   `fn_param_type_is_fat_normalized`, `type_uses_carrier_abi`, ...).
2. **The position** -- param slot, result slot, let binding, container
   element, struct field, closure capture, generic (carrier) sink, inline-C
   edge.  Today this axis lives implicitly in per-site code in
   `emit_expr.c` / `elab_*.c`.

## Stages

### Stage 1 -- the simple-kind row table (LANDED 2026-07-31)

`TY_SIMPLE_REPR_ROWS` in `types.c`: one X-macro row per payload-free
TypeKind carrying all three type-axis answers (C name, mangle token,
concrete-layout).  The three switches expand the rows with their own
projection, so:

- adding a simple kind is ONE row; forgetting it is a `-Wswitch` build
  failure in all three switches at once;
- the drift class behind `map-show-keyword-key-raw-int` (a kind present in
  one switch, absent from another) is structurally closed for simple kinds;
- payload kinds (TY_PTR_VOID, TY_FN, TY_ADT, TY_APP, the ref family, ...)
  keep hand-written arms -- their three answers genuinely compute different
  things from the payload, and forcing them into a table would trade
  readable arms for opaque function-pointer soup.

`tests/check-typekind-mangle-exhaustive.sh` parses the table plus the
residual arms; its ratchet grew a sixth property (`type_c_name`
exhaustive/no-default, previously unguarded).  37 rows, 60 kinds, behavior
byte-identical (snapshots unchanged, suite green).

### Stage 2 -- name the positions (observability before commitment)

Introduce `ReprPosition` (an enum: PARAM, RESULT, LET_BIND, CONTAINER_ELEM,
STRUCT_FIELD, CLOSURE_CAPTURE, CARRIER_SINK, INLINE_C_EDGE) and a
`repr_of(const Type *, ReprPosition)` returning a small closed enum of
representation forms (SCALAR_BITS, HEAP_PTR, BYVAL_AGG, BOXED_AGG,
CARRIER_I64, FAT_HANDLE, THIN_FN, POLY_CARRIER).  Do NOT wire it into
emission yet: shadow-call it at the existing decision sites under
`--emit-abi-trace` (the increment-0 instrument) and log disagreements
between what `repr_of` says and what the site decided.  This is the same
observability-first discipline increment 0 used: the shadow log is the
blast-radius measurement for stage 3, produced at zero behavioral risk.
A Debug-build ICE on disagreement (R3-style) graduates from logging once
the corpus runs silent.

### Stage 3 -- migrate sites chokepoint by chokepoint

With the shadow log silent on the suite + fuzz corpus, flip sites to
CONSULT `repr_of` instead of re-deriving, one chokepoint at a time
(the R0-R4 routing precedent), each measured in isolation.  Candidate
order, narrowest blast radius first: container-element sites (already one
predicate -- mechanical), fn-value tail/join classification (already
alias-aware), method-result carrier production, then the long tail of
`emit_expr.c` per-arg bridges.

### Stage 4 -- registry + ratchet

Extend the CI guard family to the position axis: a registry test that
enumerates (form, position) pairs and asserts `repr_of` totality, plus a
grep-level ratchet that fails when a new site re-derives a representation
decision inline (pattern: `type_is_wide_byval_adt|type_uses_carrier_abi`
outside the blessed files) -- new sites must go through `repr_of`.

## Guardrails (inherited from the meta-plan)

- **No performance loss:** `repr_of` is a pure classification; it changes
  WHERE decisions are made, never WHAT they decide.  Any stage that would
  alter a decision (e.g. retiring the by-value fat struct in-flight form)
  is increment 5 material, behind its own probe.
- **Grounding guard:** where a site's decision cannot yet be proven equal
  to `repr_of`'s answer, the site keeps its logic and the shadow log keeps
  watching -- no un-grounded normalization.
- **Probes:** the type fuzzer's full pool is the acceptance instrument;
  a shadow-disagreement is a finding even when nothing miscompiles.
