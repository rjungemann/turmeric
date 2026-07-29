# Web REPL: switching `#lang` mid-session drops the preloaded stdlib

**Severity:** medium (display/usability; not a miscompile)

**Status:** open, narrowed to the display gap alone. The headline stdlib drop is
**FIXED (2026-07-26)** -- and it was never web-specific: it reproduces in the
native `tur repl`, which is how it was fixed and verified here. See
[Resolution: the stdlib drop](#resolution-the-stdlib-drop). What remains is the
third fix direction, the `#map{}` / `#set{}` Show routing, which really is
WASM-side and still needs emscripten to verify.

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

### Native repro (found while fixing; not WASM-specific)

`tur repl` shares the env and the preload sequence, so it fails identically:

```
$ printf '#lang turmeric/sweet\n(when true 5)\n(map-count #map{:a 1})\n' | tur repl
; reader set to sweet-exp (session reset)
error: unknown function or operator 'when'
error: unknown function or operator 'hamt-of'
```

Note `when` -- a macro from `macros.tur` -- goes too. The loss is the whole
preload, not just the collection constructors.

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

## Resolution: the stdlib drop

Taken via the second fix direction (pin the prefix), not the first (re-run the
preload) -- see "why not replay" below.

`turi_env_pin_prelude` records where the preload ended in `src_acc`, and
`turi_env_reset_to_prelude` rewinds a reader switch to that point instead of to
zero. All four switch sites now call it; each had open-coded the same reset, and
they had already drifted -- `turi_wasm_set_lang` reset the source and two
counters but left `n_acc_forms`, `acc_next_line`, and the elaboration session
describing forms read under the *old* reader.

| site | file |
| --- | --- |
| inline `#lang` in an eval | `src/turi/eval.c` (`turi_eval_impl`) |
| file extension selects a reader | `src/turi/eval.c` (`turi_eval_file`) |
| `#lang` line at the prompt | `src/turi/repl.c` |
| browser language selector | `src/web/wasm_glue.c` (`turi_wasm_set_lang`) |

The pin is safe because the preload is plain s-expressions, which parse under
every reader variant. That is not a new assumption: `cmd_eval` in `src/main.c`
already relied on exactly it, pre-detecting the file's `#lang` so the prelude
loads under the *file's* reader and no reset fires. Its comment names this very
failure ("a `#lang sweet-exp` ... `#map{...}` fails 'unknown function or
operator hamt-of'") and scopes the workaround to file-eval, leaving "the REPL
keeps its protective reset". The pin generalises that workaround to the two
entry points it deliberately did not cover. The `cmd_eval` pre-detect is kept:
avoiding the switch entirely is still better than rewinding through one.

### Why not replay the preload

The first fix direction -- re-run the `turi_env_preload_*` sequence after the
reset -- was implemented first and is subtly wrong. Rewinding the source without
rewinding the accumulation counters marks the prelude un-run, so its
`(load "stdlib/...")` forms execute again. That works **once**: the second switch
hits the elaborator's `loaded_modules` dedup, the load registers nothing, and
`hamt-of` is unknown again. Caught by the switch-back case in the test below,
which fails under replay and passes under rewind.

So `turi_env_pin_prelude` pins `prior_toplevel` / `prior_prog_items` /
`n_acc_forms` / `acc_next_line` alongside the length, and the prelude comes back
marked already-run: still bound in `env->globals` from the original load, and
re-elaborated (not re-evaluated) when the dropped `ElabSession` is rebuilt.

### Coverage

`tests/turi/lang-switch-prelude.c` (ctest target `tur_lang_switch_prelude`):
switch, switch back, and switch to a third reader, checking a macro (`when`), a
`#map{}` / `#set{}` / `[...]` literal, and `map-count` after each. Verified to
gate -- with the pin call removed it fails 7 of 9 checks.

One shape note worth keeping, because it cost a wrong first test: the switch has
to be its **own** eval with no trailing body, which is how both REPLs drive it.
An eval that carries a body after the directive
(`"#lang turmeric/sweet\n\n(+ 1 2)"`) leaves enough behind that the next turn
still resolves, so a test written that way passes with and without the fix.

### Verification

`bash tests/run.sh` **2359 passed, 0 failed**. The turi-interpreter fixture suite
is 1797 passed / 1 failed (`refine-off-is-contracts-only`), and that one failure
reproduces unchanged at the base commit -- pre-existing, unrelated.

## Still open: the `#map{}` display gap

**Update 2026-07-29: this section is resolved, and what remained of it turned
out to be a different bug. See below.**

Third fix direction only: route `#map{}` / `#set{}` through their Show instance
in the web display tiers so an empty map prints `{}` rather than a raw carrier
int. Untouched here and still needs emscripten to verify.

A related native gap turned up while testing, worth folding in when someone picks
this up: at the native prompt an empty `#map{}` does print `#map{}`, but a
populated one shows its keyword key as a raw pointer --

```
turi> #map{:a 1}
=> #map{91328184843992 1}
```

so the Show routing is incomplete on the native side too, not only in the web
display tiers.

## Resolution of the display gap (2026-07-29)

Verified against a freshly built WASM module (emscripten 5.0.5, macOS), driving
`_turi_wasm_eval` from node exactly as the browser does.

**The stdlib-drop fix holds under WASM.** All four switch paths were exercised
-- inline `#lang` eval, switch-back, a third reader, and the browser language
selector (`turi_wasm_set_lang`, the site this report noted had drifted). Macros,
`#map{}` / `#set{}` / `[...]` literals and `map-count` all resolve after every
one of them. 20 of 21 checks passed.

**The populated-`#map{}` display gap was not a routing gap.** It was type
erasure in the auto-show path, tracked and fixed as root cause B of
[map-show-keyword-key-raw-int](map-show-keyword-key-raw-int.md). The web display
tiers needed no change of their own: `wasm_glue.c` already routes through
`turi_try_show_by_tag`, so it picked the fix up with the rebuild. Confirmed in
WASM -- `#map{:a 1}` => `#map{:a 1}`, `#map{"s" 1}` => `#map{s 1}`,
`#set{:x}` => `#set{:x}`, `(vec-of :a :b)` => `[:a :b]`, and the empty
`#map{}` => `#map{}`.

That closes the third fix direction. Nothing web-specific remains here.

**The one failing check is a separate, pre-existing bug**, now filed as
[lang-switch-breaks-generic-instance-resolution](lang-switch-breaks-generic-instance-resolution.md):
after a reader switch, *generic* (constraint-resolved) typeclass instances stop
resolving, permanently and un-symmetrically. It reproduces natively with an
explicitly-annotated `(show (:: #map{:a 1} (Map Sym int)))`, so it is neither
web-specific nor caused by the auto-show fix -- verified by compiling that fix
out and observing byte-identical behavior.

It is worth noting why this report's own test did not catch it: the checks in
`tests/turi/lang-switch-prelude.c` are all integer results (`map-count`,
`when`), which exercise name resolution but never a generic instance. The
missing assertion is recorded in the new report.
