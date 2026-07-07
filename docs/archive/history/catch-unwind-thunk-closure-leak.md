# Fix paper trail -- catch-unwind thunk fat-closure + let-bound box leaks

Resolved 2026-07-07. Report: `docs/archive/catch-unwind-thunk-closure-leak.md`.
Parent (result-box/payload half): `docs/archive/catch-unwind-result-box-leak.md`.

## What leaked

1. Every `(catch-unwind (fn [] ...))` materialized a 2-slot fat-closure thunk
   (`malloc(2 * sizeof(int64_t))`, 16 B) that was never freed -- `definitely
   lost` per catch site, unbounded in a loop.
2. A let-bound caught Result that was used (`ok?`/`ok-val`) then dropped without
   escaping was not freed -- the 24 B `tur_result_box_t` (plus 32 B payload on
   an err box) leaked once its last use was past.

## Fix

### Thunk (`src/compiler/emit_expr.c`)

- `catch_thunk_owns_fat_box(const Expr *)` -- true for `EX_CLOSURE` /
  `EX_FN_TO_FAT` / `EX_POLY_TO_FAT` (a fresh box this call site owns); false for
  a bare variable (may alias a caller-owned closure).
- `EX_CATCH_UNWIND`: emit `free((void *)(intptr_t)<thunk>)` after the box call
  when the thunk is owned.
- `EX_CATCH_PANIC_OF`: same free, emitted *before* `emit_panic_signal_return`
  so it runs on both the caught and the re-raise paths.
- Statement position is unchanged code but inherits the free: `emit_stmt.c`
  routes `EX_CATCH_UNWIND`/`EX_CATCH_PANIC_OF` through `emit_value`, which
  dispatches into the `emit_expr.c` arms above (then separately frees the
  discarded box, as the parent fix already did).

### Let-bound box (`src/compiler/emit_core.c`, `src/compiler/emit_expr.c`)

- `binding_escapes_impl(e, b, allow_box_accessors)` -- `closure_binding_escapes`
  refactored into a shared walker. With `allow_box_accessors` on, a use of `b`
  as the sole argument of a direct `ok?` / `err?` / `ok-val` call is NOT an
  escape (those read the box without retaining it; `ok-val` copies out
  `box->ok_val`, which `tur_result_box_free` never frees). `err-val` is NOT
  whitelisted -- it returns the payload pointer the box free reclaims.
- `closure_binding_escapes` / `catch_box_binding_escapes` are thin wrappers
  (flag off / on). Declared in `emit_internal.h`.
- `let_binding_box_freeable(e, idx)` -- true when binding `idx`'s init is
  `EX_CATCH_UNWIND`/`EX_CATCH_PANIC_OF` and `b` does not escape the body or any
  sibling init under `catch_box_binding_escapes`.
- `emit_let_value` collects such bindings (clean, non-return/throw path only)
  and emits `tur_result_box_free((int64_t)(intptr_t)<name>)` at scope exit,
  beside the existing fat-closure-env scoped frees.

Soundness posture matches the fat-closure-env analysis: the checks only ever
greenlight a free, so a false negative preserves the status-quo leak and never
frees a live box.

## Verification

- valgrind on the report repros: discard case and
  `(let [r (catch-unwind (fn [] 5))] (if (ok? r) (ok-val r) 0))` both go to
  `0 bytes in use at exit`; caught-panic discard and the err-box `ok?`/`ok-val`
  let both clean; no `Invalid read/write`.
- Escape guards confirmed: a box returned from its defining function
  (`(let [r ...] r)`) emits no free; an `err-val`-inspected box emits no free.
- `bash tests/run.sh`: 1975 passed, 0 failed. Two catch-unwind fixture codegen
  snapshots (`panic-catch-unwind-signal`, `stackless-catch-unwind-outer-catch`)
  regenerated to show the new frees; runtime stdout unchanged.

## Deliberately left

- `err-val`-inspected let-bound boxes are not freed (payload-dangling hazard).
- Stackless-lowering aggregate-box leak: `docs/upcoming/catch-unwind-aggregate-followups-plan.md` (Part B).
- Orthogonal pre-existing bug (not this report): returning a `catch-unwind`
  Result directly as a function's by-value `(Result ...)` result miscompiles
  (`incompatible types when returning int64_t but tur_adt_Result... expected`).
  See `docs/reported/catch-unwind-byvalue-result-return-mismatch.md`.
