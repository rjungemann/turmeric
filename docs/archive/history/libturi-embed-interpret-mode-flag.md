# libturi embedders must manually flip `g_interpret_mode` to call registered natives

> **Status:** Resolved 2026-06-24 -- fix #1 applied: `turi_env_new()` now sets
> `g_interpret_mode = true` (src/turi/env.c). Anyone calling `turi_env_new` is
> by definition an interpreter embedder; the compiler does not call this path.
> The `turmeric-godot` workaround was removed in the same change.
> **Severity:** High (embed-API correctness)
> **Found by:** Wiring `(godot-println "hi")` from the `turmeric-godot` GDExtension
> **Date:** 2026-06-24

## Summary

`turi_env_register_native` installs a native binding in `TuriEnv->globals`
just fine, but the *elaborator* (`elab_call.c:2331` and below) only emits a
runtime-dispatch call for unknown identifiers when the process-global flag
`g_interpret_mode` is `true`. Neither `turi_init()` nor `turi_env_new()`
flips that flag. An embedder that does:

```c
turi_init(false);
TuriEnv *env = turi_env_new();
turi_env_register_native(env, "my-fn", my_native, NULL);
turi_eval(env, "(my-fn)");   // -> error: unknown function or operator 'my-fn'
```

...gets a hard elaborator error, even though the binding is sitting in the
env. The fix on the embedder side is one line:

```c
extern bool g_interpret_mode;
g_interpret_mode = true;
```

...but discovering that you need it requires reading `runtime/globals.h`
and `compiler/elab_call.c`. Nothing in `turi/*.h` mentions it.

## Why this is a real embed-API bug

- The whole point of the libturi public surface is that an embedder can
  build an interpreter without owning `tur` the CLI. `g_interpret_mode` is
  set in `src/main.c` (the CLI), so any non-`tur` embed starts in the
  *wrong* mode.
- `turi_env_register_native` returns nothing and silently no-ops at call
  time (well, it sets the binding, but it's unreachable). The first symptom
  is a confusing "unknown function or operator" diagnostic that points at
  the elaborator, not at the embedder's setup.
- `g_interpret_mode` is process-global, so co-residing with other libturi
  callers (e.g. a future spice that itself embeds libturi) is a footgun.

## Fix directions

1. **Smallest fix:** have `turi_env_new()` set `g_interpret_mode = true`.
   Justification: anybody calling `turi_env_new()` is by definition an
   interpreter embedder; the compiler does not call this path. One line.
2. **Better:** make the mode a field on `TuriEnv` (e.g.
   `env->interpret_mode`) and have the elaborator read it from a current
   env passed through the elab context. Removes the global and supports
   per-env policy.
3. **Documentation-only fallback:** if the global stays, document it
   loudly at the top of `turi/eval.h`:
   > Embedders must set `g_interpret_mode = true` before the first
   > `turi_eval` call if they register custom natives with names not
   > declared as compiler builtins.

## Workaround in use

`turmeric-godot/src/turmeric_language.cpp` sets the flag manually with a
pointer to this report:

```cpp
extern "C" extern bool g_interpret_mode;

void TurmericLanguage::init_turi() {
    turi_init(false);
    g_interpret_mode = true; // see embed-API gap report
    turi_env = turi_env_new();
    turi_env_register_native(turi_env, "godot-println", tg_native_println, nullptr);
}
```

## Related

This pairs with `libturi-embed-include-paths.md` (also filed 2026-06-24):
both report independent rough edges in the embed surface that an external
user of `libturi.a` hits within the first hour.
