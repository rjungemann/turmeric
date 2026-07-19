# Fix: effect re-opening (handler case performs an outer-handled effect)

Resolves `docs/archive/cps-handler-case-effect-reopening-needs-emission.md`.

## Summary

A handler CASE body may itself `perform` an effect handled by an ENCLOSING
handler ("effect re-opening").  Before this change `handle_case_ok` had no
`CT_PERFORM` case, so such a case evicted the whole function to the fiber
(`BODY-STRUCT-OR-TAINT`); the reported bounded probe miscompiled with
`'__kont' undeclared` because the lifted handler-case frame had no enclosing
continuation to thread the interior perform against.

## Root cause

`dk_perform` calls a case as `H->handler(H->handler_env, arg, sub)` and then
threads the case's return value via `dk_run_impl(H->next, r, false)`.  The case
function's signature is `(env, arg, subk)` -- it never received the enclosing
continuation, so an interior `perform` (which `emit_perform` lowers against
`ce->cur_k == "__kont"`) referenced a `__kont` that did not exist in the frame.
The naive "use `subk`" or "use `H->next` directly" both mis-deliver: `subk`
re-runs the inner body on resume, and `H->next` (real frames) double-runs the
enclosing frames because `dk_perform` already threads them.

## Fix

The interior perform must dispatch against the **transparent enclosing handler
markers** -- handler nodes only, `dk_done()`-terminated -- so the effect reaches
the enclosing handler while the case's own value returns to the `H->next`
boundary for `dk_perform` to thread the real enclosing frames exactly once
(handler markers are skipped by `dk_run_impl`, so no double delivery).

- **Runtime** (`src/compiler/emit_dk_runtime.c`):
  - `dk_case_enclosing(H)` -- copies the enclosing handler markers, skipping the
    whole `dk_handler` sibling group for a deep handler (matching `dk_perform`'s
    own `ge` walk) or just `H` for a shallow one.
  - `g_dk_case_reopen_hnode` -- set to `H` immediately before each
    `H->handler(...)` call, read by a re-opening case at entry (into a local,
    before any interior perform can overwrite the global) -- so no ABI change to
    `DKHandler`, and non-re-opening cases are unaffected.

- **Admission** (`emit_cps_ir.c`, `handle_case_ok`): admit a `CT_PERFORM` whose
  args are slot atoms and whose continuation is itself `handle_case_ok` (it may
  resume the case's own `k`).

- **Emission** (`emit_cps_ir.c`, `emit_lifted` `LH_HANDLER_CASE`): when the case
  body re-opens (`case_reopens`), declare
  `DK *__kont = __dk_reap_keep(dk_case_enclosing(g_dk_case_reopen_hnode));` at
  entry; `emit_perform`'s existing straight-line (`LH_PERFORM_CONT`) and
  Track-A (`LH_RESUME_CONT`) paths then thread it as `cur_k` unchanged.  A
  re-opening case binds its continuation `k` as the `int64_t` word
  (`(int64_t)(intptr_t)subk`) rather than `DK *`, so the perform-continuation
  env store matches its field type without an int-from-pointer warning
  (`emit_resume` casts `(DK *)k` at use).

## Verification

- Minimal repro (2 effects, inner Log case re-opens Write): prints `start` once,
  CPS-admitted.
- Variants: outer handler resuming a non-zero value; re-open inside an `if`
  branch; two sequential interior performs in one case -- all correct + admitted.
- `effect-reopen` fixture runs `start`/`done`/`142`.
- `bash tests/run.sh`: 2194 passed, 0 failed (DK-runtime preamble snapshots
  regenerated in the same change).

## Residual (separate report)

The `effect-reopen` *fixture* still evicts under `--enable=cps-tramp-resume`,
but for an unrelated, pre-existing reason -- a `perform` whose continuation
contains a non-tail cps->cps heap join -- filed as
`docs/reported/cps-perform-cont-heap-join-eviction.md`.  It is independent of
re-opening (it reproduces with no re-opening at all) and out of scope here.
