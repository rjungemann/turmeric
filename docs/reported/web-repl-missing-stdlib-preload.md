# Web REPL: stdlib not preloaded -- `#map{}`, `#set{}`, and other reader literals fail

**Severity:** medium (web/WASM REPL is missing the stdlib bootstrap the
native `--interpret` path performs; user-visible as "unknown function or
operator" for reader-macro lowerings).

## Summary

In the browser/WASM REPL (`web/` + `src/web/wasm_glue.c`), reader-macro
data literals like `#map{...}` and `#set{...}` fail because the runtime
environment has no `hamt-of` / `set-of` / etc. binding. These are
`defmacro`s in `stdlib/map.tur` (and friends), so they only become
available once the relevant stdlib module is loaded. The WASM entry
point never loads any stdlib.

Repro (in the web REPL at `/try`):

```
> #map{}
<eval>:1:5: warning [TUR-W0040]: unknown name 'hamt-of'; will runtime-dispatch -- typo?
1 | #map{}
|     ^^
#<error: unknown function or operator 'hamt-of'>
```

The compiled path and `tur --interpret` are unaffected -- both preload
the stdlib before user code.

## Root cause

`turi_wasm_init` in `src/web/wasm_glue.c:80` builds a bare `TuriEnv`:

```c
int turi_wasm_init(void) {
    if (g_env) return 0;
    g_env = turi_env_new();
    if (!g_env) return 1;
    turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);
    turi_init(false);
    return 0;
}
```

The native `--interpret` entry point in `src/main.c:5437-5477` does
significantly more work on the same env: it `(load ...)`s
`stdlib/macros.tur` and `stdlib/contract.tur` explicitly, and later
registers native shims. The reader-macro layer lowers `#map{k v ...}`
to `(hamt-of k v ...)` unconditionally, so any environment that hasn't
loaded `stdlib/map.tur` (where `defmacro hamt-of` lives -- see
`stdlib/map.tur:890`) will fail to elaborate the call.

Same failure mode applies to any other data-literal-driven lowering
whose target lives in the stdlib but isn't force-loaded (`#set{...}` ->
`set-of`, etc.).

## Fix directions

Bring the WASM init flow into parity with `src/main.c`'s file-eval
preload:

1. In `turi_wasm_init` (`src/web/wasm_glue.c:80`), after
   `turi_env_set_diag_sink`, evaluate `(load "<stdlib>/macros.tur")`,
   `(load "<stdlib>/contract.tur")`, and `(load "<stdlib>/map.tur")`
   using the same `snprintf`+`turi_eval` shape used in
   `src/main.c:5448-5477`. Pick the stdlib path via
   `tur_stdlib_path(...)`.
2. The WASM build has no real filesystem, so `tur_stdlib_path` +
   `(load ...)` needs the stdlib bundled into the Emscripten preload
   FS (`--preload-file stdlib@/stdlib` at link time) OR the preload
   needs to be reworked to feed the source text in directly (embed the
   `.tur` sources as C string literals and call `turi_eval` on them).
   The embedded-string route is probably simpler for WASM and avoids
   FS surprises -- see how `tools/gendocs.py --emit-tur` already bakes
   `stdlib/docstrings.tur` into the build.
3. Consider factoring the preload sequence out of `src/main.c` into a
   shared helper (e.g. `turi_env_preload_stdlib(env, const char
   *stdlib_root)`) so the native `--interpret`, REPL, and WASM entry
   points can't drift.

## Related

- `stdlib/map.tur:861-890` -- `defmacro hamt-of` (the lowering target).
- `src/compiler/elab_toplevel.c:477-496` -- `#map{...}` -> `hamt-of`
  lowering.
- `src/main.c:5437-5477` -- the reference stdlib preload the WASM path
  needs to mirror.
- `src/web/wasm_glue.c:80-94` -- the WASM init that currently skips
  preload.
