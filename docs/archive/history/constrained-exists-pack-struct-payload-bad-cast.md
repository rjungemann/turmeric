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
