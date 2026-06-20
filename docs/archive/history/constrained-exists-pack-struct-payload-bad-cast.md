# Paper trail: constrained-exists-pack-struct-payload-bad-cast

Branch: `claude/spice-reported-issues-cs1utr`

## Origin

`turmeric-spices` Track C / `plot` P3 investigation
(`plot-renderer-typeclass-plan.md`). Boxing mixed renderer structs into a
constrained existential `AnyRenderer` hit a codegen miscompile when the
payload was a by-value `defstruct`. The spice plan's Risk 1 said to file a
report rather than force the change; the report
(`docs/reported/constrained-exists-pack-struct-payload-bad-cast.md`) was
already on file and is confirmed accurate.

## Investigation

- Reproduced the filed defect on a fresh Debug build:
  `__t43->value = (int64_t)((LinesR){.v = INT64_C(5)});` ->
  `error: aggregate value used where an integer was expected`.
- Located the divergence: `EX_EXISTS_PACK` in `src/compiler/emit_expr.c`
  heap-boxes a by-value aggregate only on the unconstrained path
  (`exists_payload_is_byval_aggregate`); the constrained path (`n_witnesses >
  0`) stores `value = (int64_t)(payload)` unconditionally.
- Pinned the deeper layer the report alluded to: `EX_EXISTS_DISPATCH` flows
  the receiver through the int64 carrier and erases class-var params to
  `int64_t`, but a by-value struct instance method is
  `int64_t __inst_Rdr_rbound_LinesR(LinesR)`. So heap-boxing alone would
  compile but leave dispatch a runtime-ABI trap. A *constrained* existential
  payload is abstract (only usable via dispatch), so a heap-box-only fix has
  no real use case -- the bounded, safe resolution is a diagnostic.
- Confirmed the working shape: `defopaque T :int` carrier payloads dispatch
  end to end (`tests/fixtures/exists-open-witness-dispatch` -> `5,107,3,109`).
- Surveyed fixtures: every existing constrained-existential `pack` uses a
  scalar `int`, a `defopaque ...:int`, or an RC pointer -- none pack a
  concrete by-value struct, so the new diagnostic regresses nothing.

## Change

- `src/compiler/elab_types.c` (`elab_pack`): after witness resolution, reject
  a by-value `defstruct` payload (non-opaque, `n_type_params == 0`, not a
  transparent int newtype) in a constraint-carrying existential with an
  actionable diagnostic naming the `defopaque T :int` / `:ptr<T>` workaround.
- `tests/fixtures/errors/exists-pack-byval-struct-payload/`: new error fixture.
- Report moved `docs/reported/` -> `docs/archive/` with a resolution note and
  the remaining-enhancement spec (heap-box + carrier-adapter thunk + unbox).

## Verification

- `bash tests/run.sh` (10-min timeout): `summary: 1704 passed, 0 failed`.
- Diagnostic fires for the report's 1-field repro and the plot 3-field
  `PointsR`; the opaque-over-int dispatch fixture still builds and runs.

## Superseded: heap-box support replaces the diagnostic

Branch: `claude/constrained-exists-pack-struct-cast-a7o5jb`

The `elab_pack` diagnostic above was a *reject*-the-construct resolution. It
was subsequently replaced by end-to-end **heap-box support** for the by-value
struct payload -- the construct now compiles and the pack/open record
round-trips instead of being forbidden:

- `src/compiler/emit_expr.c` (`EX_EXISTS_PACK` / `EX_EXISTS_OPEN`): on the
  constrained (`n_witnesses > 0`) path, heap-box a by-value aggregate payload
  (`exists_payload_is_byval_aggregate`) and carry the box pointer in the
  record's `value` slot; open reads it back through the record -> box
  indirection (rc-managed and `:linear` paths), freeing the box on the
  `:linear` path.
- `src/compiler/emit_module.c`: new `tur_existential_drop_byval` rc drop hook
  frees the box when the rc-managed record is reclaimed (block stays
  `RCEXP_OPAQUE`, so the cycle walker never follows the plain box).
- `src/compiler/elab_types.c` (`elab_pack`): the reject diagnostic was removed.
- `tests/fixtures/errors/exists-pack-byval-struct-payload/`: removed (the
  construct is no longer an error).
- `tests/fixtures/exists-pack-constrained-byval-struct/`: new pass fixture for
  the report's exact repro.

The dispatch-ABI layer the diagnostic cited is *not* solved by this change --
witness dispatch on a by-value-struct receiver still assumes the int64 carrier
ABI and reads garbage. It is now tracked as a focused open report,
`docs/reported/constrained-exists-open-dispatch-byval-struct-receiver.md`,
rather than forbidding the whole construct.

Verification: `bash tests/run.sh` (10-min timeout) -- `1704 passed, 0 failed`.
