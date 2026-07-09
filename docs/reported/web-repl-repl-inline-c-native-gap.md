# WASM / `tur repl`: inline-C stdlib ops need the `wk_register_*` natives

**Severity:** low (follow-up carved out of
`docs/archive/web-repl-missing-stdlib-preload.md`; a bounded, well-understood
subset of stdlib is still unavailable in the two interpreter entry points that
do not link `src/main.c`).

## Summary

The web/WASM REPL (`src/web/wasm_glue.c`) and the interactive `tur repl`
(`src/turi/repl.c`) now preload the stdlib via the shared
`turi_env_preload_*` helpers, so macros and the typed collections resolve.
But those two entry points build on the bare `libturi`/`tur_core` env and do
**not** run the large `wk_register_*` / `native_*` block that lives in
`src/main.c` and that `cmd_eval` (the native `--interpret` path) runs after its
preload.

Any stdlib op whose Turmeric body is inline-C and is *overridden* by one of
those main-only natives therefore fails under WASM / `tur repl` with:

```
eval: inline-C not supported in interpreter mode
(function uses a native C implementation; run it with `tur build`/`tur run`
 instead of `--interpret`)
```

instead of evaluating. Known cases:

- **Keyword / `:Sym`-keyed maps** -- `#map{:a 1}` needs `Hash[Sym]` /
  `Eq[Sym]` (`stdlib/sym.tur`, inline-C) which `wk_register_sym_natives`
  overrides. Int/`cstr`/`bool`/`float`-keyed maps work (their comparators are
  registered by `turi_register_collection_natives` in the core).
- **Contract panics** -- `assert!`/`require!`/`ensure!` lower to
  `tur-contract-check`, overridden by `native_contract_check`.
- **`json` / `schema`** -- overridden by `wk_register_{json,schema}_natives`.
- **Lazy `seq`** -- overridden by `wk_register_seq_natives`.

This is a strict improvement over the pre-fix state (a hard "unknown function
or operator 'hamt-of'"), but it is not full parity with `--interpret`.

## Root cause

`turi_env_new` registers only the core collection/async/eval natives
(`src/turi/env.c:211`). The rest of the interpreter's native overrides are
~3900 lines of `static` `wk_register_*` / `native_*` functions in
`src/main.c` (roughly lines 5352-10030), which is not compiled into
`tur_core` and so is absent from both the WASM link (`WASM_SOURCES =
WASM_GLUE_SOURCES + TUR_CORE_SOURCES`) and `tur repl`.

## Fix directions

Relocate the `wk_register_*` / `native_*` block from `src/main.c` into a new
`tur_core` source (mirroring how `turi/collections_native.c` and
`turi/preload.c` were already carved out), exposing a single
`turi_env_register_interpreter_natives(env)` the three entry points call after
`turi_env_preload_*`. Watch for main-only dependencies (the fixture-runner
helpers `wk_apply_flags` / `wk_write_result` / `wk_drain_pipes` /
`wk_eval_fixture` use `fork`/`pipe` and must stay in `main.c`; the `g_*`
diagnostic-flag globals referenced inside some natives need to move or be
threaded through). The move should be validated on native first
(`bash tests/run.sh` behavior-preserving) since the WASM half cannot be
exercised without an Emscripten toolchain.

## Confirmed (2026-07-09, during repl-show-collections)

Re-verified the first bullet from the interactive `tur repl`:

- `#map{:a 1}` at the prompt -> `eval: inline-C not supported in interpreter
  mode`. `(hash :a)` / `(sym->str :a)` fail the same way, while `:type :a`
  correctly reports `: Sym`.
- The same program runs fine under `./build/tur --interpret` and
  `./build/tur run` (both print the looked-up value), which pins the gap to
  the two entry points that skip the `wk_register_*` block -- exactly this
  report. Int-keyed maps at the REPL now also exercise the new collection
  `Show` path and work, because their comparators come from
  `turi_register_collection_natives`, which the REPL does get.

## Related

- `docs/archive/web-repl-missing-stdlib-preload.md` -- the resolved parent report.
- `docs/reported/wasm-repl-no-show-collection-preload.md` -- sibling WASM-REPL
  parity gap (collection `Show` preload + `turi_try_show_by_tag` wiring); that
  one is *not* a native-registration gap and is fixable independently.
- `src/turi/preload.c` -- the shared preload helper (the load half of the fix).
- `src/turi/env.c:211` -- `turi_register_collection_natives`, the core natives
  the two entry points already get.
- `src/main.c:5648-5698` -- the `wk_register_*` sequence `cmd_eval` runs that
  WASM / `tur repl` currently do not.
