# Web REPL: switching `#lang` mid-session drops the preloaded stdlib

**Severity:** medium (display/usability; not a miscompile)

## Summary

In the Try Turmeric web REPL, evaluating a `#lang` line (e.g. running an editor
program that starts with `#lang turmeric/sweet`) and then using a
collection literal at the prompt breaks the stdlib:

```
turi> #map{}
<eval>:1:5: warning [TUR-W0040]: unknown name 'hamt-of'; will runtime-dispatch -- typo?
#<error: unknown function or operator 'hamt-of'>
```

Before any `#lang` switch, `#map{}` instead prints a raw pointer int (e.g.
`32818056`) rather than a Show'd `{}` -- a second, smaller display gap.

## Repro (against the committed WASM, driven through `_turi_wasm_eval`)

1. `turi_wasm_init()`
2. eval `#lang turmeric/sweet\n\nprintln "Hello, Turmeric!"`  (sets sweet reader)
3. eval `#map{}`  => `#<error: unknown function or operator 'hamt-of'>`

The native `bash tests/run.sh` path is unaffected; this is specific to the
long-lived REPL env that accumulates source across evals.

## Root cause (direction)

`turi_eval_impl` (src/turi/eval.c) discards the accumulated source when the
reader type changes:

```c
if (detected != env->reader_type) {
    /* Reader type is changing: discard accumulated source ... */
    env->src_acc.len       = 0;
    env->prior_toplevel    = 0;
    env->prior_prog_items  = 0;
    env->reader_type       = detected;
}
```

The web preloads the stdlib by evaluating its source into this same env
(`wasm_preload_stdlib` -> `turi_env_preload_*`, src/web/wasm_glue.c). That
preloaded source lives in `env->src_acc`, so wiping `src_acc` on the first
`#lang` switch throws away the definitions `#map{}` elaboration needs
(`hamt-of` et al.), leaving them "unknown".

## Fix directions

- On a reader-type switch, re-establish the stdlib preload for the new reader
  instead of leaving `src_acc` empty (re-run `wasm_preload_stdlib` / the
  `turi_env_preload_*` sequence), or
- keep the preloaded stdlib prefix out of the discarded region (preload under a
  pinned prefix that a `#lang` switch does not reset), and
- separately, route `#map{}` / `#set{}` through their Show instance in the
  web display tiers so an empty map prints `{}` rather than a raw carrier int.

Verifying any fix here requires rebuilding the WASM (emscripten), which was not
available in the session that filed this.
