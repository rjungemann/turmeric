---
title: A vec-typed argument substituted into a cross-module wrapper macro's body is elaborated in expression position before the wrapper expands
category: Macro expansion / cross-module elaboration
status: FIXED -- the elaborator now tracks a STACK of in-progress macro expansion modules (`Elab::macro_expansion_stack` in `src/compiler/elab_internal.h`); the visibility check in `elab_lookup_macro` (`src/compiler/elab_core.c`) walks the stack so any module in the active expansion chain contributes its visibility. Push/pop landed in `elab_macros.c` (the `elab_expand_macro` entry) and `elab_call.c` (the post-expansion re-elaboration site); cleanup landed in `elab_toplevel.c`. Regression covered by `tests/fixtures/cross-module-macro-vec-arg-in-wrapper-body`. The ECS spice's variadic `for-each` was restored to the recursive-macro implementation (no arity cap); new test `tests/for-each-arity-12.tur` exercises 12 components end-to-end.
severity: WAS A macro that worked in single-file mode failed at the user's call site when split across modules. The error ("unbound symbol") pointed at the user's source, not at the macro internals.
description: A `defmacro` M defined in module A that emitted a backquoted form `(W ... (H ~vec-arg ...) ...)` mis-elaborated the substituted `~vec-arg` when W was a macro defined in a module other than A and H was a helper macro defined in A. During W's body re-elaboration, the elaborator's single-slot `macro_expansion_module` got overwritten with W's defining module (NULL for stdlib), so H became invisible. H then resolved as an ordinary function call; its vec arg landed in expression position; the data-literals reader lowered `[A B]` to `(vec-of A B)`; symbol elements failed to resolve.
---

# Cross-module wrapper macro elaborates substituted vec arg in expression position

> **UPDATE: fixed.** The earlier framings ("intervening `let`" /
> "F_VEC loses form-mode") both pointed at the wrong layer. The real
> root cause was a single-slot expansion-module visibility tracker
> that nested wrapper expansions clobbered. See the "Fix" section at
> the bottom.

## Summary

A recursive variadic macro pattern that works fine in single-file mode
fails when the macro is moved into a separate module and imported.
The trigger has been bisected to a precise three-module pattern:

```
module A defines  M  -- the outer macro the user calls
module B defines  W  -- a wrapper macro that M emits
module U imports  M  -- the use-side that triggers expansion
```

When M emits `(W ... ~vec-arg ...)`, the substituted `~vec-arg`
(an F_VEC form supplied by U) is elaborated *in expression position*
before W's macro expansion runs, so a vec literal like `[A B]` lowers
to `(vec-of A B)` and the elaborator demands `A` and `B` be value
bindings. The diagnostic points at the user's source position with
"unbound symbol 'A'".

Discovered while implementing truly variadic `for-each` for the ECS
spice's E1 plan (`../turmeric-spices/spices/ecs/src/ecs/query.tur`).

## Trigger conditions (bisected)

All three must hold:

1. **Outer macro M** is defined in some module A and emits a backquoted
   form that substitutes its vec argument: `` `(... ~comps ...) ``.
2. **A wrapper macro W** appears in M's emit *around* the substituted
   vec or a form depending on it. W is defined in a module other than
   A (stdlib counts; W imported into A from yet another module counts).
3. **M is invoked from a different module** -- a use-side that imports M.

In single-file mode (M, W, and the call site all in the same file),
the pattern works. In single-file mode where M and W are co-located
but M is invoked from another module, the pattern also works.

The wrapper W can be `when`, `unless`, `cond`, or any user-defined
macro imported from a third module. Special forms (`do`, `if`) do
*not* trigger the bug.

## Minimal repro

`mod-a.tur` (defines outer macro `m` that wraps `~comps` in `when`):

```turmeric
(defmodule mod-a (export m)
(defmacro m [comps] `(when true ~comps))
) ;; end
```

`use.tur`:

```turmeric
(defmodule use (export)
(import mod-a :refer [m])
(defn main [] : int (m [A B]) 0)
) ;; end
```

```
$ tur run -Xdata-literals use.tur
use.tur:3:24: error: unbound symbol 'A'
3 | (defn main [] : int (m [A B]) 0)
  |                        ^
```

`when` lives in `stdlib/macros.tur` (a different module from `mod-a`),
so condition 2 fires. Inline `(when true ...)` as `(if true (do ...))`
in `m`'s emit -- the bug disappears (special forms aren't macros).
Move `(defmacro my-when ...)` into `mod-a` next to `m` and rewrite `m`
to emit `(my-when ...)` -- the bug disappears (W and M co-located).

## Bisection results

Probed in `/tmp/probe-bug-*` while tracking down the original
ECS for-each failure. Each row is a self-contained repro.

| Outer-macro emit (M in module A, called from module U) | Result |
|---|---|
| `` `(when true ~comps) `` (W = stdlib `when`) | **FAIL** |
| `` `(unless false ~comps) `` (W = stdlib `unless`) | **FAIL** |
| `` `(cond :else (fold-len ~comps)) `` (W = stdlib `cond`) | **FAIL** |
| `` `(if true (do (fold-len ~comps))) `` (W = `if`/`do` special forms) | OK |
| `` `(if true (do ~comps)) `` (same shape as `when` expansion, hand-written) | OK |
| `` `(my-when true (fold-len ~comps)) `` -- `my-when` defined in module A next to M | OK |
| `` `(my-when true (fold-len ~comps)) `` -- `my-when` defined in module B, imported into A | **FAIL** |
| `(defmacro my-when [t b] (if t (do b)))` co-located with M | OK |
| `(defmacro my-when [t b] `(if ~t (do ~b)))` co-located with M | OK |
| `` `(when true (println 1)) `` -- no `~comps` substitution | OK |
| `` `(when (> (fold-len ~comps) 0) (println 99)) `` -- `~comps` in test position | **FAIL** |
| `` `(when true (do (fold-len ~comps))) `` -- explicit extra `do` wrapper | **FAIL** |
| `` `(when true (println ~(symbol-name (first comps)))) `` -- comps processed at macro-time, not substituted as a form | OK |
| `` `(when true ~comps) `` invoked with `[1 2 3]` (literals only) | OK |
| `` `(when true ~comps) `` invoked with `[A B]` (symbols, A/B unbound) | **FAIL** |

Observations:

- The OK rows where `comps` is processed at macro-time (`symbol-name`
  + `first`) or replaced with literal values confirm the bug is
  about the *substitution as a form* being elaborated as an
  expression.
- The OK row for "same-module my-when" -- whether defined with or
  without backquote, matching stdlib `when`'s body verbatim -- is
  the strongest single clue that the trigger is **cross-module
  identity of W**, not the shape of W's body.
- Position inside W's body (`test` vs `body`) does not matter.
- Wrapping in an explicit extra `(do ...)` does not help, since the
  outer form is still a macro call.

## Same-file works, exact body

```turmeric
(defmacro fold-len [comps]
  (let [c (first comps) s (str->sym (str-append "__s_" (symbol-name c)))]
    (if (empty? (rest comps)) `(dense-len ~s)
      `(+ (dense-len ~s) (fold-len ~(rest comps))))))

(defmacro m [comps] `(when true (fold-len ~comps)))

(defn dense-len [v : int] : int v)
(defstruct W [A : int B : int])
(defn main [] : int
  (let [__s_A 3 __s_B 5]
    (println (m [A B])))
  0)
```

Result: prints `8` (3 + 5). Same definitions, same call site,
different *file structure* (all in one file vs split across modules).

## Hypothesis (provisional)

The macro-expansion machinery walks a backquoted form once per
substitution. When the substituted form (`~comps` -- an F_VEC) lands
inside *another macro's* unexpanded body, the order of operations is:

1. M's emit produces a form `(W ... <F_VEC>)`.
2. The elaborator processes this form. To expand W, it must first
   look W up and read its arguments.
3. Reading W's args evaluates the vec literal positionally somewhere
   in the cross-module path -- the F_VEC is lowered to `(vec-of A B)`
   *before* control reaches W's macro body. The substituted vec
   loses its "stay-as-form" property.

Two pieces of evidence support this:

- Replacing W with a special form (`if`, `do`) skips step 2 entirely;
  the F_VEC is processed by the elaborator's own special-form
  handlers and stays a form. (Works.)
- Replacing W with a same-module user macro skips the cross-module
  path; the macro-arg form bookkeeping stays intact. (Works.)
- Processing `comps` at macro-time via `(first comps)` /
  `(symbol-name ...)` -- producing a literal symbol or string before
  the backquote substitutes -- never lets the F_VEC reach W's
  argument slot, so no expression-position elaboration. (Works.)

The fix is presumably in the cross-module macro-arg-passing path:
F_VEC arguments substituted via `~` should retain their form-mode
flag across the module boundary, the same way they would in a
same-file expansion.

## Workarounds (in increasing order of intrusion)

1. **Co-locate the wrappers.** Move the wrapper macros (`when`-like
   forms) into the same module as the outer macro. Awkward when the
   wrapper is stdlib `when` -- it would have to be re-defined locally.
2. **Inline the wrapper as special forms.** Replace `(when test body)`
   with `(if test (do body))`. Avoids the trigger entirely (special
   forms don't take this path). This was the recursive `__fe-has-conj`
   trick -- `(if (dense-has? ...) ... false)` instead of `and`.
3. **Process the vec arg at macro-time.** Walk it with
   `first` / `rest` / `cons` to build a *list* (not an F_VEC) before
   substituting. Bypasses the F_VEC-evaluated-as-`vec-of` path. Works
   but requires every `~comps` use to flow through a list builder.
4. **Keep the explicit arity ladder.** The ECS spice E1 does this for
   now (`for-each` dispatches to `for-each1` .. `for-eachN` by
   component count). No vec is substituted into a wrapper's body --
   each `for-eachN` takes the components as positional args.

The ECS spice E1 went with (4) because (1)-(3) all required
contorting the macro design enough to lose the readability win.

## Validation of a fix

A fixture `tests/fixtures/cross-module-macro-vec-arg-in-wrapper-body`
that pairs the two-module repro above and expects exit 0 with no
diagnostic. Currently it would fail with `unbound symbol 'A'` at the
caller's source position.

Bonus: fold the broader bisection table above into the fixture as a
multi-line table-driven test so a regression is caught precisely.

## Related

- `stdlib/macros.tur:derive-show-rest__` -- a recursive variadic
  macro that always worked cross-module, because its emit wraps in
  `(list str-concat ...)`: `str-concat` is a function, not a wrapper
  macro, so the visibility-stack collapse never happened.
- `../turmeric-spices/spices/ecs/src/ecs/query.tur` (E1) -- the
  motivating use case. After the fix, the spice's variadic `for-each`
  was restored to the recursive-macro implementation
  (`__fe-storages` / `__fe-min-cap` / `__fe-has-conj` /
  `__fe-value-binds`) with no arity cap. The `for-each1`..`for-each8`
  dispatch ladder was retired in favour of thin
  `(for-each ~world [~A1 ~A2 ...] [~e ~v1 ...] body)` shims; the
  `for-each-arity-12.tur` test exercises 12 components.
- `stdlib/macros.tur` is the home of the wrappers (`when`, `unless`,
  `cond`) that triggered the bug most often before the fix.

## Fix

The elaborator tracks the currently-expanding macro's defining module
in `Elab::macro_expansion_module` (a single slot) so private helper
macros of that module are visible during the expansion. The slot was
being saved/restored around each macro expansion, but nested
expansions clobbered it: when M's expansion produced
`(W ... (H ~comps ...) ...)` and the elaborator re-elaborated this
form, the inner `W` expansion overwrote the slot with W's module
(NULL for stdlib), so during W's body re-elaboration the inner H
lookup no longer saw M's module. H fell back to ordinary
function-call elaboration, and `~comps` (an F_VEC) landed in
expression position where the data-literals reader lowered it to
`(vec-of A B)` with unbound symbol elements.

The fix turns the single slot into a stack:

- `Elab::macro_expansion_stack` (typed `const Symbol **`) plus a
  pair of `n_/cap_` counters in `elab_internal.h`.
- `elab_lookup_macro` (`elab_core.c`) walks the stack and accepts the
  macro when any element matches its `defining_module_name`.
- `elab_expand_macro` (`elab_macros.c`) and the post-expansion
  re-elaboration site in `elab_call.c` push the expanding macro's
  defining module on enter and pop on exit (in addition to the
  existing single-slot save/restore for back-compat with sites that
  read the slot directly).
- `elab_toplevel.c` frees the stack alongside the existing macros
  array teardown.

Net effect: a nested wrapper expansion preserves the outer module's
visibility, so M's private helpers remain visible through W's body
re-elaboration. The F_VEC substituted via `~comps` therefore reaches
H's macro-arg slot (form mode) instead of being elaborated as
`(vec-of ...)`.

Regression covered by `tests/fixtures/cross-module-macro-vec-arg-in-wrapper-body`
(two-module fixture: `mod-vec-bug.tur` exporting the outer macro `m`
that wraps a private recursive helper `fold-len` in stdlib `when`;
`input.tur` invoking `(m [A B])` and expecting `8` on stdout).
Pre-fix this fixture would fail with `unbound symbol 'A'` at the
user's `[A B]` source position.
