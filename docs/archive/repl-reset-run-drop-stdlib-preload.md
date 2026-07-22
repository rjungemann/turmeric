# `:reset` / `:run` drop the REPL stdlib preload -> spurious TUR-W0040

**Status:** RESOLVED (fixed in `src/turi/repl.c`; see
`docs/archive/history/repl-reset-run-drop-stdlib-preload.md`).

**Severity:** medium (interactive-only, but silently breaks a core surface --
`#map{}`/`#set{}`, typeclass `Show`, and the carrier list helpers -- after the
very first `:reset`/`:run`, which is exactly what an editor's "Run" button does).

## Symptom

After `:reset` (or `:run <file>`), loading code that uses `list-head`/
`list-tail` warns:

```
turmeric> :reset
;; session cleared
turmeric> (load "parser.tur")
parser.tur:17:16: warning [TUR-W0040]: unknown name 'list-head'; will runtime-dispatch -- typo?
parser.tur:18:30: warning [TUR-W0040]: unknown name 'list-tail'; will runtime-dispatch -- typo?
```

and non-native preloaded surface fails outright:

```
turmeric> :reset
turmeric> #map{:a 1}
error: unknown function or operator 'hamt-of'
```

A fresh session (no `:reset`/`:run`) is clean. Surfaced via Trowel, whose Run
action issues a reset before loading the buffer, so every run after the first
looked broken even though the bundled `tur` (v0.30.4) was current.

## Root cause

REPL startup builds the interactive environment in three steps
(`src/turi/repl.c`, startup path):

1. `turi_env_new()` -- registers the inline-C native overrides.
2. **preload** stdlib *source*: `turi_env_preload_macros` /
   `_preload_collections` / `_preload_typeclasses`. This is what elaborates
   `stdlib/list.tur` (and the collection/typeclass surface) so the
   **elaborator** knows `list-head`, `hamt-of`, `#map`, etc.
3. re-register the native overrides so the shims win over the just-loaded
   inline-C bodies.

`:reset` (the `":reset"` handler) and `:run` (`cmd_run`) recreated the env with
`turi_env_free` + `turi_env_new` but ran **only step 1**, skipping the preload
and the native re-registration. `turi_env_new()` re-adds the *runtime* natives
(`src/turi/env.c:232`), so `list-head` still resolved at runtime, but the
*elaborator* no longer had the binding (its source was never reloaded) -> the
TUR-W0040 "unknown name; will runtime-dispatch" warning. For the non-native
preloaded names (`hamt-of` behind `#map{}`) there is nothing to dispatch to, so
they failed at runtime.

## Fix

Factored the startup preload+register sequence into a single helper
`repl_preload_stdlib_and_natives(env)` and called it from all three
env-(re)creation sites: startup, the `:reset` handler, and `cmd_run` (`:run`).
Spice auto-discovery (RP3) is intentionally left out of the helper -- it is
cwd-dependent and prints its own banner.

## Verified

- `:reset` then `(load parser.tur)` -> no W0040, `=> #<fn digit>`.
- `:reset` then `#map{:a 1}` -> returns a real hamt (no runtime error).
- `:run <file>` -> same clean behavior.

## Note

This is distinct from the still-open
`docs/reported/repl-list-head-over-cons-returns-nil.md` (prompt-built `cons` +
`list-head` -> `nil`), which reproduces even in a fresh session.
