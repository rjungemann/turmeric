/* preload.h -- shared stdlib preload sequence for interpreter entry points.
 *
 * The tree-walking interpreter starts from a bare `TuriEnv` that only knows the
 * elaborator builtins (`+`, `let`, `if`, `fn`, ...).  Every real program needs
 * the core macros (`when`/`cond`/`for`/`and`/`or`) and the typed-collection
 * stdlib (Map/Set/Vec/Option/Result/...) loaded on top of that env before user
 * code runs.  `src/main.c`'s `--interpret` entry point (`cmd_eval`) has always
 * done this inline; the WASM/browser REPL (`src/web/wasm_glue.c`) skipped it,
 * so reader-macro data literals such as `#map{...}` (which lower to the
 * `hamt-of` macro defined in `stdlib/map.tur`) failed with "unknown function or
 * operator 'hamt-of'".  See docs/reported/web-repl-missing-stdlib-preload.md.
 *
 * These two helpers factor that load sequence out of `main.c` so the native
 * interpreter and the WASM REPL share one copy and cannot drift.  They only
 * emit `(load "<root>/<file>")` forms; the underlying Vec/Set/Map/HAMT runtime
 * ops are already registered as natives by `turi_env_new`
 * (`turi_register_collection_natives`), so no extra native wiring is required
 * for the collection literals to evaluate.
 */
#ifndef TURI_PRELOAD_H
#define TURI_PRELOAD_H

#include "turi/env.h"

/* Load `macros.tur` then `contract.tur`, each in its own `turi_eval` call so
 * each file gets a distinct file_id (the per-file "one defmodule" reset fires)
 * and both land at the front of the accumulated source, where the Phase M7
 * macro-promotion makes their `tur/`-prefixed macros globally visible without
 * an explicit import.  `stdlib_root` is prepended to each basename (e.g. an
 * absolute path from `tur_stdlib_path`, or the FS-relative "stdlib" the WASM
 * MEMFS embed uses); NULL/"" defaults to "stdlib". */
void turi_env_preload_macros(TuriEnv *env, const char *stdlib_root);

/* Load the typeclass-stub + typed-collection stdlib set (safe, typeclass-*,
 * hamt/set/map, vec/slice, option/result, pair/tuple/list, grid/zipper, mutmap,
 * unique, sym) as one batched `(load A)(load B)...` form so the whole prelude
 * elaborates in a single `turi_eval` with deps resolved in order.  Mirrors the
 * array `cmd_eval` loads; keep the two in sync.  `stdlib_root` as above. */
void turi_env_preload_collections(TuriEnv *env, const char *stdlib_root);

#endif /* TURI_PRELOAD_H */
