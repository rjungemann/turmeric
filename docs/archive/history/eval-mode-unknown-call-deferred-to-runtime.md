# Eval-Mode Elaboration Defers Unknown-Call Diagnostics to Runtime

> **Status:** RESOLVED (2026-06-25) -- fix direction A landed. Eval-mode
> elaboration now emits **TUR-W0040** ("unknown name '<x>'; will
> runtime-dispatch -- typo?") when an unknown call head is neither bound
> nor present in the typed-native registry, so embedders consuming the
> diag sink surface the likely typo at load time. The runtime-dispatch
> fallback is preserved, so legitimate late-bound TuriEnv natives still
> work. See the resolution note at the bottom.
> **Severity:** Medium -- embedder scripts can ship undefined-name typos
> that pass `_validate` and only fail when the offending line runs.
> Compile-mode (`tur check`/`tur build`) is unaffected; UCH1 already
> hard-errors there.
> **Discovered:** 2026-06-25
> **Discovered by:** turmeric-godot G3.c codegen churn (a method-name
> entry moved into a skip list; references that previously resolved to
> the generated wrapper started silently no-op'ing at runtime instead
> of failing at load).

---

## Summary

In compile mode, an unknown call head is a hard `TUR-E*` error at
`elab_call.c:2331-2354` (the UCH1 "diagnose-unbound-call-heads-plan"
guard). In interpret/eval mode (the embedder path -- `turi_eval`,
`tur --interpret`, REPL, worker), the elaborator falls through at
`elab_call.c:2376-2400` and builds a runtime-dispatch call typed
`:int`, **with no diagnostic at all**. The dispatch fails at runtime
when libturi looks up the name in the env, doesn't find it, and
returns `TURI_ERROR`. The enclosing function body halts at that point.

The diag sink does see the runtime error, so embedders that route it
into a UI (turmeric-godot does, via `script_diag_sink`) get *some*
signal. But the signal is per-execution, not per-script-load, so a
typo on an uncovered branch can stay green through `_validate` and
ship.

---

## Minimal repro

In turmeric-godot (any libturi embedder reproduces it):

```turmeric
;; user.tur, attached to a Node
(defn _ready []
  (godot-println "[undef] before")
  (definitely-not-a-real-function 1 2 3)   ;; misspelled
  (godot-println "[undef] after"))         ;; never runs
```

Observed (driver headless on Godot 4.3):

```
[undef] before
turmeric-godot: Turmeric returned error: unknown function or operator 'definitely-not-a-real-function'
[undef] script ran to completion -- no engine-side failure observed
```

Note that:

1. The script *loaded* without error. `_validate` returned `valid:
   true`. The Godot editor would happily attach it.
2. The error message reached stderr at the moment `_ready` was
   evaluated -- not at load time.
3. The body halted after the offending call. `[undef] after` never
   ran. If `definitely-not-a-real-function` had been guarded by a
   branch the test path didn't exercise, the typo would have shipped.

Compile-mode behavior, for contrast:

```sh
$ ./build/tur run /tmp/undef.tur
/tmp/undef.tur:2:4: error: unknown function or operator 'undefined-fn'
1 | (defn main [] : int
2 |   (undefined-fn 1 2)
  |    ^^^^^^^^^^^^
```

UCH1 catches it. The interpret-mode path doesn't.

---

## Root cause

**`src/compiler/elab_call.c:2376-2400`** -- the
`g_interpret_mode && !separate_compilation` fallback for unknown call
heads:

```c
/* eval mode: create a runtime-dispatch call so native builtins
 * registered in TuriEnv (e.g. async scheduler functions) are
 * callable without a compile-time declaration.
 * ...
 * Known interpreter natives whose return type is not :int get
 * explicit typing here so callers (e.g. `(if (error? r) ...)`)
 * see the right type at the call site. */
Type dispatch_result = TYPE_INT;   /* + the two-entry allow-list */
Binding *dyn_b = binding_new(e, name, dispatch_result, /*...*/);
Expr *var_expr = expr_new(e->arena, EX_VAR, dispatch_result, head->span);
/* ... build EX_CALL and return ... */
```

No `diag_emit` along this path. The runtime-dispatch is the right
behavior for legitimate dynamic natives (the comment cites async
scheduler functions, and `turi_register_default_native` is the
embedder's way to add them). The miss is that it *also* swallows
typos.

The UCH1 guard immediately above this fallback (lines 2331-2374)
already shows the right shape -- it emits a `DIAG_ERROR` with the
stdlib-hint / legacy-form-hint sugar -- but is gated to compile
mode.

The recent typed-native-registration fix (commit `78329855e`,
`docs/archive/untyped-native-registration-blocks-curated-facades.md`)
threads a `tur_native_sig_lookup` through this path. That registry
gives elaboration a way to distinguish "registered native, return
type known" from "registered native, default :int" from "not
registered at all" -- the third case is the one this report covers.

---

## Fix directions

### A. Emit a warning (preferred)

Where compile mode emits `DIAG_ERROR`, interpret mode emits
`DIAG_WARNING` ("unknown name in eval mode -- will runtime-dispatch;
typo?"). The runtime-dispatch fallback stays intact, so legitimate
late-binding via `turi_env_register_native` still works.

Concretely:

```c
if (e->separate_compilation || !g_interpret_mode) {
    /* existing DIAG_ERROR path */
} else {
    /* eval-mode fallback */
    TurNativeRetType ignored;
    if (!tur_native_sig_lookup(name->name, &ignored)) {
        diag_emit(DIAG_WARNING, head->span,
                  "unknown name '%s'; will runtime-dispatch -- typo?",
                  name->name);
    }
    /* ... build runtime-dispatch as today ... */
}
```

The lookup uses the typed-native registry (which records every
registered native, even untyped ones at `TUR_NRT_INT`) so the warning
only fires when the name truly isn't bound at registration time.
Names defined later in the same script (`defn` later in the source)
already resolve through ordinary binding lookup before this fallback
fires, so they don't trip the warning.

Embedders consuming the diag sink (turmeric-godot,
`script_diag_sink`) would now get the heads-up at `_validate` time,
visible in the editor before the script is ever attached. The
`_validate` Dictionary populated by ScriptLanguageExtension would
include these in its `warnings` array (already plumbed).

### B. Embedder opt-in: make it a hard error

Add a flag `turi_env_set_strict_unknown_calls(env, bool)` that flips
the fallback to `DIAG_ERROR` (the UCH1 path). Embedders that own the
script surface (a game engine, a typed REPL) opt in; tooling that
hosts arbitrary scripts (a worker) stays loose.

This is strictly stronger than A and could be layered: ship A first
(warning visible to everyone), add B later if a real consumer needs
to fail-closed.

### C. Don't fix; rely on embedder diag-sink runtime catches

Embedders see the error eventually, just not at load time. Acceptable
for scripts with full branch coverage; bad for the common
"_ready/_process body branches on inspector state" pattern in a game
engine where many branches are exercised lazily.

---

## Recommendation

**A.** Cheap (a single `if` + `tur_native_sig_lookup`), composes with
the existing typed-native registry, and lets embedders surface the
warning in their _validate UI. **B** if a real consumer needs the
stricter mode -- can be added in the same PR or follow-up.

---

## Resolution (2026-06-25)

Fix direction **A** landed.

- **New diagnostic code:** `TUR_W0040_EVAL_UNKNOWN_CALL_RUNTIME_DISPATCH`
  (`TUR-W0040`), declared in `src/compiler/diag.h` and mapped to/from its
  string in `src/compiler/diag.c`.
- **Emit site:** `src/compiler/elab_call.c`, the
  `g_interpret_mode && !separate_compilation` unknown-call-head fallback.
  The fallback already consulted `tur_native_sig_lookup` to recover a
  registered native's return type; that lookup (plus the two hard-coded
  `error?` / `error-message` entries) now also drives a
  `native_registered` flag. When the name is **not** registered, the
  elaborator emits `diag_emit_with_code(DIAG_WARNING, head->span,
  TUR_W0040_..., "unknown name '%s'; will runtime-dispatch -- typo?")`
  before building the runtime-dispatch call exactly as before. Names that
  resolve through ordinary binding lookup (e.g. a `defn` later in the same
  source) never reach this fallback, so they don't trip the warning.
- **Behavior preserved:** compile mode still hard-errors via UCH1; the
  eval-mode runtime-dispatch fallback is unchanged, so embedder natives
  registered with `turi_env_register_native` / the typed registration APIs
  keep working.
- **Regression test:** `tests/turi/eval-unknown-call-warn.{tur,sh}`,
  registered as the `tur_eval_unknown_call_warn` ctest target, asserts the
  `TUR-W0040` warning (with symbol name + "runtime-dispatch" text) appears
  on stderr under `--interpret`.

Fix direction **B** (embedder opt-in hard-error via
`turi_env_set_strict_unknown_calls`) was **not** implemented -- it remains
available as a strictly-stronger follow-up if a real consumer needs to
fail-closed.
