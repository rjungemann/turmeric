# REPL `define` at top level is swallowed by synthetic `do` wrapper

**Severity:** medium (silent data loss -- the binding evaluates without error,
returns `nil`, and is gone on the next prompt; trips up anyone with a
Scheme/Racket reflex)

**Status:** open. Reproduced on a freshly launched `tur repl` (no `(reload)`,
no prior turns).

## Summary

`(define foo "foo")` typed at the REPL top-level prompt does **not** create a
top-level binding. It evaluates to `nil`, and on the next turn the name is
unbound. This is because the REPL evaluator wraps every turn that contains a
`define` in a synthetic `(do ...)`, and `define` is body-scoped to its
enclosing `do` -- so the binding is born inside the wrapper and dies when the
wrapper returns.

## Repro

Fresh `tur repl`:

```
turmeric> (define foo "foo")
=> nil
turmeric> foo
<eval>:2:1: error [TUR-E0003]: unbound symbol 'foo'
1 | (do (define foo "foo"))
2 | foo
  | ^^^
turmeric> (println foo)
<eval>:2:10: error [TUR-E0003]: unbound symbol 'foo'
1 | (do (define foo "foo"))
2 | (println foo)
  |          ^^^
```

The displayed source `1 | (do (define foo "foo"))` is the smoking gun -- the
REPL really did wrap the user's single form in `do`.

By contrast, `(def foo "foo")` at the prompt persists across turns as
expected.

## Root cause

`src/turi/eval.c:8080-8108` -- on each REPL turn, the evaluator scans the
new-turn forms for any `(define ...)` head and, if found, wraps **all** of
that turn's new forms in a single synthetic `(do ...)`:

```c
if (new_has_define) {
    /* ... */
    Form *do_form = form_list(a, sp, wrap, nn + 1);
    forms[prior] = do_form;
    nforms = prior + 1;
    /* ... */
}
```

`define` is elaborated as a let-style local binder by
`src/compiler/elab_forms.c:48` (it walks the enclosing body and binds within
that scope). Inside the synthetic `do`, the binding is local; the wrapper
returns and the binding is gone.

The wrapping logic appears intended to support Scheme-style "internal
defines" (multiple `define`s plus a trailing expression all in one body), but
it fires unconditionally whenever a `define` is present -- even for a single
top-level `(define ...)` that the user clearly meant as a top-level binder.

## Fix direction

User's preference: **option 1 -- make `define` at REPL top level lower to
`def`** (the real top-level binder). Concretely, in
`src/turi/eval.c:8080-8108`, when wrapping new-turn forms, rewrite any
top-level `(define name init)` into `(def name init)` instead of leaving it
as `define` inside the `do`. (Or, equivalently, special-case the single-form
turn to skip the wrapper entirely.)

Either way, after the fix:

```
turmeric> (define foo "foo")
turmeric> foo
=> "foo"
```

should work on a fresh REPL, matching the existing behavior of `def`.

Alternatives considered and rejected:

- **Stop wrapping in `do` when there's only one form.** Fixes the
  single-turn case but still loses the binding if the user types multiple
  `define`s plus a trailing expression on one turn. The lowering-to-`def`
  approach handles both.
- **Emit a diagnostic telling the user to use `def`.** Less friendly than
  Just Working, and `define` is already a documented form elsewhere in the
  compiler -- making it work top-level is the consistent move.

## Out of scope

What `define` should do *inside* a non-REPL body (let-style local vs.
hoisted top-level in a module) is a separate question; this report is only
about REPL top-level behavior.
