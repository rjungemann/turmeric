# WASM REPL: collection results still print as opaque pointers (no Show wiring)

**Status:** RESOLVED 2026-07-10. Both gaps closed in `src/web/wasm_glue.c`:
`wasm_preload_stdlib` now calls `turi_env_preload_typeclasses` after
`turi_env_preload_collections`, and both eval paths (`turi_wasm_eval`,
`turi_wasm_eval_ex`) gained the fourth display tier
`turi_try_show_by_tag(g_env, result, type_tag)` between `turi_show_result`
and the `turi_value_repr` fallback -- matching the native `tur repl`
four-tier dispatch in `src/turi/repl.c`.

**Severity:** low (usability paper cut, browser REPL only; a follow-up carved
out of the `repl-show-collections` work -- see
`docs/archive/history/repl-no-show-instances-for-collections-and-structs.md`).

## Summary

The native `tur repl` now renders a `Vec` / `Set` / `Map` result through its
`Show` instance, so `(vec-of 1 2 3)` prints `[1 2 3]`, `#set{...}` prints
`#set{...}`, etc. The web/WASM REPL (`src/web/wasm_glue.c`) does **not** get
this: a collection result there still prints its raw heap pointer.

Two independent gaps, both in `src/web/wasm_glue.c`:

1. **The Show slice is never preloaded.** The WASM env setup loads macros and
   the typed collections:

   ```c
   turi_env_preload_macros(env, WASM_STDLIB_ROOT);
   turi_env_preload_collections(env, WASM_STDLIB_ROOT);   // wasm_glue.c:92-93
   ```

   but it never calls `turi_env_preload_typeclasses(env, WASM_STDLIB_ROOT)`,
   the REPL-only helper the native `tur repl` runs (`src/turi/repl.c`, right
   after `turi_env_preload_collections`). Without it there is no `Show [Vec]` /
   `Show [Set]` / `Show [Map]` instance registered, so nothing to dispatch to.

2. **The display path is still the old three tiers.** The WASM result
   formatter (`wasm_glue.c:159-170`) predates the fourth tier added for the
   native REPL:

   ```c
   /* SI4: three-tier display: turi_try_show -> turi_show_result -> repr */
   const char *show_str = turi_try_show(g_env, result);      // TURI_STRUCT only
   if (!show_str)
       show_str = turi_show_result(g_env, result, type_tag); // Pair / Cons only
   /* ... falls through to turi_value_repr -> raw pointer for Vec/Set/Map */
   ```

   It never calls `turi_try_show_by_tag(g_env, result, type_tag)`, the tier
   that renders a named ADT/struct/record heap result (Vec, Set, Map, or a
   user `defstruct`/`defgadt` with a `Show` instance) through its instance.
   `src/turi/repl.c` was updated to a four-tier dispatch; `wasm_glue.c` was
   not.

## Root cause

`src/turi/repl.c` (native REPL) and `src/web/wasm_glue.c` (browser REPL) each
carry their own env-setup and result-formatting code; the
`repl-show-collections` change only touched the native side. The WASM side
drifted on both the preload list and the display dispatch.

## Fix directions

Both are one-liners against `src/web/wasm_glue.c`, no new stdlib or compiler
work:

1. After the `turi_env_preload_collections` call (wasm_glue.c:93), add:

   ```c
   turi_env_preload_typeclasses(env, WASM_STDLIB_ROOT);
   ```

2. In the result formatter (wasm_glue.c:159-170), add the fourth tier between
   `turi_show_result` and the `turi_value_repr` fallback:

   ```c
   if (!show_str)
       show_str = turi_try_show_by_tag(g_env, result, type_tag);
   ```

## Notes

- Unlike the `:Sym`-keyed-map REPL gap (see
  `docs/reported/web-repl-repl-inline-c-native-gap.md`), the collection `Show`
  bodies should Just Work in WASM once preloaded and wired: they only need
  `show-concat`, `vec-get`/`vec-len`, and the HAMT iterator ops, all of which
  are registered by `turi_register_collection_natives` at `turi_env_new` time
  (shared by every interpreter entry point, WASM included). No `wk_register_*`
  main-only native is on the collection `Show` path.
- The `cstr`-element display limitation (int64-carrier element-type erasure,
  shared with `Eq [Vec]`/`Eq [Set]`/`Eq [Map]`) applies here too: `Vec`/`Set`
  of `cstr`, and `cstr`-keyed maps, will render their carrier pointer rather
  than the string. That is a separate, systemic limitation, not specific to
  the WASM REPL.
- Report filed 2026-07-09.
