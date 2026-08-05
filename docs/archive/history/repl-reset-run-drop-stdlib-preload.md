# Fix trail: `:reset`/`:run` drop the REPL stdlib preload

Resolved report: `docs/archive/repl-reset-run-drop-stdlib-preload.md`.

## Change

`src/turi/repl.c`:

- Added `static void repl_preload_stdlib_and_natives(TuriEnv *env)` bundling the
  startup sequence: `turi_env_preload_macros` + `_preload_collections` +
  `_preload_typeclasses` + `turi_env_register_interpreter_natives` +
  `tur_ffi_register_reload_native`.
- Startup: replaced the inline preload/register block with one call to the
  helper (spice auto-discovery RP3 left in place after it).
- `:reset` handler: added the helper call after `turi_env_new()` +
  `turi_env_set_diag_sink`.
- `cmd_run` (`:run`): replaced the lone `tur_ffi_register_reload_native(env)`
  with the helper call.

Net: `+30 / -29` in `src/turi/repl.c`.

## Why a helper rather than three inline copies

The three sites had already drifted -- startup preloaded + re-registered,
`:reset` did neither, `cmd_run` registered only the reload native. One helper
is the single source of truth so a future addition to the interactive preload
cannot silently miss `:reset`/`:run` again.

## Verification

- Repro (before): `:reset` then `(load parser.tur)` -> TUR-W0040 on
  `list-head`/`list-tail`; `#map{:a 1}` -> `error: unknown function or operator
  'hamt-of'`.
- After: both clean (`=> #<fn digit>`; `#map` returns a hamt). `:run` clean.
- `bash tests/run.sh`: 2248 passed, 20 failed; the 20 are pre-existing and in
  unrelated subsystems (reactor async fibers `exit 134`, image-reload hooks,
  hrt continuations, `re-string`) -- none exercise the REPL env-reinit path,
  and they reproduce on the pre-change baseline.
