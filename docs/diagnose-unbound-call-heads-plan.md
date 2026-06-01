# Diagnose Unbound Call Heads at Compile Time -- Plan (UCH0--UCH2)

> **Status:** Not started.
>
> **Last updated:** 2026-06-01

---

## Motivation

An unbound symbol in **call-head position** is not diagnosed by the compiler.
A call like `(foo 1)` where `foo` is undefined (a typo, or a function the
caller forgot to `import`/`extern-c`) silently elaborates to an `int`-typed
runtime-dispatch call. The consequences in the **compiled** path
(`tur build`, `tur emit-c`, `tur run`, `tur check`) are:

1. `tur check` passes with exit 0 -- the type error is never reported.
2. Downstream type errors become misleading. `(if (foo "x") 0 1)` reports
   *"if condition must be bool, got int"* -- pointing at the `if`, not at the
   real problem (`foo` is unbound).
3. The only real failure is a cryptic C-compiler error much later:
   `'foo_835' undeclared (first use in this function)`.

By contrast, an unbound symbol in **value position** (`foo`, not `(foo ...)`)
is correctly reported as `unbound symbol 'foo'` with a "did you mean"
suggestion. The asymmetry is the bug.

### Why it happens

`elab_call` (`src/compiler/elab_call.c`) resolves the head symbol via
`elab_lookup_sym`. When that returns no binding and the name is not a builtin
operator, the final fallback (around line 1222) is:

```c
} else if (e->separate_compilation) {
    diag_emit(DIAG_ERROR, head->span,
              "unknown function or operator '%s'", name->name);
} else {
    /* eval mode: create a runtime-dispatch call so native builtins
     * registered in TuriEnv (e.g. async scheduler functions) are
     * callable without a compile-time declaration. */
    Binding *dyn_b = binding_new(e, name, TYPE_INT, false, false, head->span);
    ... EX_CALL typed TYPE_INT, fn_binding = NULL ...
    return out;
}
```

The dynamic-dispatch fallback exists for a real reason: the tree-walking
**interpreter** (`tur eval`, `tur --interpret`, `tur repl`, `tur worker`)
resolves natives registered in `TuriEnv` at runtime, so they must be callable
without a compile-time declaration (verified: `tur eval '(task-cancelled?)'`
returns `false` via this path, and an unknown name yields a graceful *runtime*
`unbound variable` error rather than a compile error).

The defect is that the fallback is gated on `!e->separate_compilation`, which
is **too broad**: plain single-file compilation (`tur build foo.tur`,
`tur emit-c foo.tur`, `tur run foo.tur`, `tur check foo.tur`) is also
`separate_compilation == false`, so it wrongly takes the interpreter fallback.

The correct discriminator is "are we interpreting?" -- the global
`g_interpret_mode`, which is already set by `cmd_eval`, `cmd_eval_expr`, and
the worker path, and is already read directly by the elaborator
(`elab_toplevel.c` reader-conditional). The REPL (`turi_repl_run`) is also an
interpreter entry point but currently does **not** set `g_interpret_mode`; it
relies on the same fallback, so it must set the flag too.

---

## Goals

- A genuinely-unbound call head in any **compiled** path is a hard error at
  compile time: `unknown function or operator 'foo'`, located at the head.
- The interpreter paths (`eval`, `--interpret`, `repl`, `worker`) keep the
  runtime-native fallback unchanged.
- No regression to the existing `unbound symbol` value-position diagnostic,
  to typeclass-method bare-name dispatch, or to forward references within a
  file (which resolve to a pre-registered binding before bodies elaborate).

## Non-goals

- "Did you mean" suggestions for unbound call heads (the value-position path
  has them; adding them here is a nice-to-have, deferred).
- Any change to how natives are registered or resolved at runtime.

---

## Phase UCH0: Flag the REPL as interpret mode

`cmd_repl` (`src/main.c`) sets `g_interpret_mode = true` before invoking
`turi_repl_run`, matching `cmd_eval` / `cmd_eval_expr` / worker. This is
independently correct (the REPL is interpret mode; the INT-1 reader
conditional should pick the `:turi` branch there) and is a prerequisite for
UCH1 so the REPL keeps the native fallback.

### Acceptance

- `tur repl` still resolves TuriEnv natives (e.g. async scheduler fns).
- The INT-1 reader conditional picks `:turi` in the REPL.

---

## Phase UCH1: Gate the dynamic fallback on interpret mode

In `elab_call.c`, change the unknown-head fallback so the runtime-dispatch
branch is taken only when `g_interpret_mode` is true; otherwise emit the same
`unknown function or operator '%s'` error the `separate_compilation` branch
already emits. Concretely, the branch order becomes:

```c
} else if (e->separate_compilation || !g_interpret_mode) {
    diag_emit(DIAG_ERROR, head->span,
              "unknown function or operator '%s'", name->name);
} else {
    /* interpret mode only: runtime-dispatch fallback for TuriEnv natives */
    ...
}
```

### Acceptance

- `tur check` on `(defn main [] :int (+ (foo 1) 2))` reports
  `unknown function or operator 'foo'` and exits non-zero.
- `(if (foo "x") 0 1)` reports the unbound head, not "got int".
- `tur eval '(task-cancelled?)'` still returns `false`.
- `tur eval '(totally-unknown-fn 1)'` still gives a runtime `unbound
  variable` error (not a compile error).
- Full fixture suite + stdlib-checks remain green.

---

## Phase UCH2: Regression fixtures

- `tests/fixtures/errors/unbound-call-head/` -- a `(foo 1)` call with no
  binding; `expected.diag` contains `unknown function or operator 'foo'`.
- Confirm an existing positive fixture exercises a TuriEnv native through the
  interpreter so the eval path stays covered (or add a small one).

### Acceptance

- `bash tests/run.sh` passes with the new negative fixture.

---

## Risks / Open Questions

1. **Are there compiled programs that legitimately call an undeclared
   symbol?** No -- compiled code reaches C functions via `extern-c`
   declarations (which create bindings) or Turmeric `defn`s (pre-registered).
   The `separate_compilation` path already errors on unknown heads, so
   extending that to all compiled paths is consistent.
2. **Does gating on a process-global rather than an `Elab` field risk the
   compiler-as-library (wasm/LSP) paths?** Those configure their mode
   explicitly; `g_interpret_mode` defaults to false (compile semantics),
   which is the safe default for tooling that wants errors surfaced.
