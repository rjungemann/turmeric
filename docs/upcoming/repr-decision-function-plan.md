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
(LANDED 2026-07-31)

`ReprPosition` (PARAM, RESULT, LET_BIND, CONTAINER_ELEM, STRUCT_FIELD,
CARRIER_SINK) and `repr_of(const Type *, ReprPosition) -> ReprForm`
(SCALAR_BITS, HEAP_PTR, BYVAL_AGG, BOXED_AGG, CARRIER_I64, FAT_HANDLE,
THIN_FN) live in `types.c`/`types.h`.  It is NOT wired into emission:
shadow checks at two decision sites (`emit_binding_repr_c_name` -> the
let-bind decl; `control_result_temp_ctype` -> the control-form merge temp)
log one `repr-shadow` stderr line per disagreement under
`--emit-abi-trace`.  `tests/run-repr-trace.sh` pins the instrument (a
mid-migration shape must fire; a consolidated shape must not).

**First corpus measurement** (1976 fixtures, 2026-07-31): the raw sweep
found 521 disagreement lines; two spec-calibration passes (existential
packages ARE heap pointers; tyvar-elemented heap apps are the erased
spelling of the same bits; `any`/union are the two-word tagged aggregate
at direct positions; the merge-temp shadow must use the walker's RECOVERED
type, not the elab-collapsed one) reduced it to **80 lines in exactly two
residual classes**, both genuine protocol gaps that today's bridges paper
over:

| class | count | meaning |
| --- | --- | --- |
| `let-bind want=heap-ptr got=carrier-i64` | 51 (+2 merge-temp) | a CONCRETE heap container (`(Vec int)`, `(MutableMap int int)`, a :heap struct) bound as `int64_t` instead of its typed pointer -- the spelling seam behind the gcc14 pointer/int straddle bridge family |
| `let-bind want=byval-agg got=carrier-i64` | 27 | a concrete by-value app (`(Schema int)`, `(BoxW int)`, phantom-param structs) bound as the carrier because the INIT produced a carrier -- the init-dependent mid-migration regime in `emit_binding_repr_c_name` |

This is stage 3's work list.  A Debug-build ICE on disagreement (R3-style)
graduates from logging once the corpus runs silent.

### Stage 3 -- migrate sites chokepoint by chokepoint
(chokepoint 1 LANDED 2026-07-31; work list below)

With the shadow log measured, flip sites to CONSULT `repr_of` instead of
re-deriving, one chokepoint at a time (the R0-R4 routing precedent), each
measured in isolation.

**Chokepoint 1 (landed): concrete heap bindings get their typed pointer.**
`emit_binding_repr_c_name` consults `repr_of(rbt, LET_BIND)`: a concrete
(tyvar-free, non-existential-elemented) heap container / :heap struct
binding is declared as its typed pointer regardless of the init's erased
spelling; the existing binding-emission arms bridge an int64 init into the
pointer decl.  Consumed 47 of the 53 heap-ptr shadow lines; one snapshot
regenerated (`make-struct-cstr-carrier-bridge` -- the diff is a REMOVED
`(int64_t)(intptr_t)` cast, the exact seam this chokepoint closes).  Suite
2449/0; fuzz seed 139, 250 cases, 0 findings.  Two spec refinements rode
along, both narrowing what counts as a disagreement: existential-elemented
heap apps stay carrier-spelled (`(Vec (exists ...))` -- the
vec-get-existential-element erasure design), and the let-bind shadow skips
bindings whose DECLARED type mentions tyvars (a spec-resolved generic body
keeps the erased spelling by design).

**Chokepoint 2 grounding (2026-07-31): no seam existed.**  The 27
`byval-agg` lines were SC7 transparent int newtypes -- a parametric
single-ctor record with one concrete-int field (`(ArrW int)`,
`(Schema int)`) lowers to its raw int64 payload with NO ctor call, so the
int64 decl IS the value.  `repr_of` classifies
`type_is_transparent_int_newtype` as SCALAR_BITS in every position, the
shadow mapper recognizes the same transparency, and the earlier deferral
rationale (base-ctor/monomorph layout proof) is moot for this class: the
probe showed the base ctor is never even called for these shapes.  The
liveness smoke re-anchored to the lens fixture accordingly.

**Corpus silence achieved for the shadowed sites (2026-07-31): sweep 7 = 0
lines.**  521 -> 80 -> 38 -> 33 -> 6 -> 0 across the calibration/migration
arc.  The final 6 (the lens family) fell to a real drift find: a :heap
record with heap-struct FIELDS (`Line {a: Point}`) fails
`adt_is_byvalue_product`, so `type_c_name` said int64 while the ctor
emitter returned the typed pointer -- the two-switches-disagree anatomy at
yet another pair of sites.  The binding chokepoint now asks the def for
the pointer spelling directly (`adt_heap_ptr_c_name`).  Getting the guard
right took two measured corrections, both now encoded in the arm: a
tyvar-DECLARED binding keeps the erased spelling even when the spec
resolves it (hoisting the arm above the carrier-ABI early return without
this churned 140 fixtures), and a BARE parametric ADT base (`Map` with
its args erased) is the erased container, not a concrete type.  One
snapshot regenerated (`van-laarhoven-lens-wide-compose`, value-preserving
`(int64_t)(intptr_t)` casts at dict-call args).  Suite 2449/0; fuzz seed
149, 250 cases, 0 findings.

The liveness smoke GRADUATED to the silence criterion: any repr-shadow
line on the former lens anchor is now a failure.  The let-bind and
merge-temp positions are consolidated; what remains of stage 3 is
EXTENDING shadow coverage to the positions not yet instrumented --
fn-value tail/join classification (already alias-aware -- mechanical),
method-result carrier production, then the long tail of `emit_expr.c`
per-arg bridges -- and then stage 4's registry ratchet.

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
