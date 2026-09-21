---
title: when, unless and a dozen other body-taking forms accept exactly one form, so every multi-statement body pays an explicit (do ...)
category: Reported
description: `(when c a b)` is a hard error naming stdlib/macros.tur; `(defer a b)` silently drops `b`. The stdlib is split down the middle -- 12 `& body` macros against 14 single-`body` ones -- and the formatter and the turi docstrings both already describe these forms as variadic. Same in Saffron, which shares the prelude.
---

# Single-form bodies: `when` / `unless` and friends need an explicit `do`

**Severity: medium**, with one **high** sub-case. Most of the family fails
loudly at the call site, which makes this an expressiveness and consistency
hole rather than a correctness one. `defer` is the exception: it accepts extra
body forms and **silently discards them** on both back ends.

**Status:** OPEN. Filed 2026-09-21. Not previously reported -- a grep of
`docs/reported/` and `docs/archive/` for this shape turns up only
`refine-callsite-path-conds-lost-multi-form-body` (archived, unrelated: a
refinement path-condition bug in `defn` bodies, which have always been
variadic).

Verified against `./build/tur` v0.50.0, freshly built from this tree
(`main` @ `361024bad`).

## Repro

### 1. `when` / `unless` -- hard error, and it points at the stdlib

```turmeric
(defn main [] : int
  (when (= 1 1)
    (println "a")
    (println "b"))
  0)
```

```
stdlib/macros.tur:55:3: error: macro 'when' expects 2 arguments, got 3
55 |   (defmacro when [test body] (if test (do body)))
   |   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
probe.tur:2:3: note: in expansion of macro 'when' -- the diagnostics above are
  inside code this call generated
```

Exit 1. Three things are wrong with this even as a rejection: the primary
location is `stdlib/macros.tur`, not the user's file; there is no `TUR-Exxxx`
code, so `tur explain` has nothing to say; and "expects 2 arguments, got 3"
describes the macro's parameter vector rather than the thing the programmer
did. `unless` is identical (`stdlib/macros.tur:56`).

The workaround is the `do` everyone already writes:

```turmeric
(when (= 1 1)
  (do (println "a")
      (println "b")))
```

### 2. `defer` -- accepted, and the extra forms are dropped

```turmeric
(defn main [] : int
  (defer (println "d1") (println "d2"))
  (println "body")
  0)
```

```
$ ./build/tur run probe.tur
body
d1
```

No diagnostic, exit 0, `d2` never runs. `tur --interpret` prints the same two
lines, so the interpreter agrees with the compiler about the wrong answer.

Root cause is direct: `elab_defer` (`src/compiler/elab_forms.c:3938`) rejects
`len < 2` and then elaborates `call->as.list.items[1]` and nothing else --
once at `elab_forms.c:3947` for the top-level/`atexit` case, once at
`elab_forms.c:3956` for the in-scope case. Indices 2..n are never read.

### 3. Saffron inherits all of it

Saffron shares `stdlib/macros.tur` -- `stdlib/saffron/prelude.tur` does not
redefine these -- so the same program under `#lang saffron` produces the same
`stdlib/macros.tur:55` error, byte for byte. Anything done here lands in both
dialects at once; nothing Saffron-specific is required.

### 4. Sweet-exp pays an extra indentation level for it

```turmeric
#lang sweet-exp
defn main [] : int
  when {1 = 1}
    println("a")
    println("b")      ; error: macro 'when' expects 2 arguments, got 3
  0
```

The indentation layer hands `when` three arguments, which is exactly what the
t-expr reading should mean. The fix is to add a `do` line and indent the body
one further -- one level of nesting that sweet-exp exists to remove.
`while` in the same position needs no `do`, because `while` is variadic:

```turmeric
  while {i < 2}          ; this works today
    println("x")
    set!(i {i + 1})
```

Worth noting that CLAUDE.md's own sweet-exp example writes
`while not(window-should-close?(w))` / `do` / body -- a `do` that `while` has
never needed. The habit has already spread past the forms that require it.

## The inventory

Everything below takes a body. The split is not along any principled line.

### Already variadic -- no action needed

| Form | Where |
| --- | --- |
| `do`, `let`, `let*`, `letrec`, `defn`, `fn` | elaborator |
| `while` | `elab_while` |
| `unsafe` | `elab_unsafe` |
| `binding` (dynamic vars) | `elab_binding` |
| `stm` | `src/compiler/elab_concurrent.c:689` -- loops `i = 1 .. len` |
| `for` | `stdlib/macros.tur:84` |
| `with-resource` | `stdlib/macros.tur:132` |
| `with-lock` | `stdlib/mutex.tur:143` |
| `with-read-lock` / `with-write-lock` | `stdlib/rwlock.tur:155,176` |
| `with-unique` | `stdlib/unique.tur:40` |
| `task-group-with` / `-with-timeout` / `-with-cancellation` | `stdlib/taskgroup.tur:578,584,682` |
| `open-vec` / `open-sized` | `stdlib/vec-existential.tur:77`, `stdlib/sized-handle-existential.tur:81` |
| `deftest` | `stdlib/test.tur:166` |

### Single-form body -- the hole

| Form | Where | Today |
| --- | --- | --- |
| `when` | `stdlib/macros.tur:55` | hard error, points at the stdlib |
| `unless` | `stdlib/macros.tur:56` | hard error, points at the stdlib |
| `defer` | `src/compiler/elab_forms.c:3938` | **silently drops forms 2..n** |
| `reset` | `src/compiler/elab_effects.c:199` | `(reset body) requires exactly one argument` |
| `cloneable-reset` | `src/compiler/elab_effects.c:429` | same message |
| `serial-reset` | `src/compiler/elab_effects.c:1301` | same message |
| `atomically` | `src/compiler/elab_call.c:3657` | dispatch is gated on `len == 2`; a 3-element call falls out of the special-form table and reports **`unknown function or operator 'atomically'`** -- the form exists, the arity is wrong, and the diagnostic says neither |
| `with-write`, `with-fail-panic`, `with-getenv`, `with-read-console`, `with-stderr-log`, `with-silent-log`, `with-abort-panic`, `with-async`, `with-await` | `stdlib/effects.tur:175,190,206,239,272,288,320,369,385` | `[body]`, one form |
| `with-capability` | `stdlib/capability.tur:254` | `[binding cap_expr body]` |
| `defimage-reload-hook` / `-finalize-hook` | `stdlib/image.tur:258,266` | `[name body]` |

Counting only the stdlib macros: **12 take `& body`, 14 take `body`.** Two of
them (`with-resource` and `when`) are eleven lines apart in the same file.

### Where a body block is *not* sensible -- leave these alone

- `if` -- an expression with a value in each branch; `(do ...)` is the honest
  spelling and a second "then" form would have no meaning.
- `cond`, `case`, `match`, `handle` -- all four have the flat pair-up-two-at-a-time
  shape CLAUDE.md documents. A body block is not even expressible: an extra
  form is read as the **next clause's test**. `(cond (= 1 1) (println "a")
  (println "b") :else 0)` is not an error about arity, it is
  `if condition must be bool, got nil` from the third form being taken as a
  test. The shape, not the arity check, is what forbids it.
- `with-region`, `bt-scope` -- these take a `^fat` thunk, not a body
  (`stdlib/region.tur`, `stdlib/trail.tur:480`). The thunk's own body is
  already variadic: `(with-region (fn [] : int (println "a") (println "b") 0))`
  works today. Note that a stray second argument here reads as a *curried
  application* of the thunk's result, which produces the unhelpful
  `TUR-E0002: function 'with-region' returns tyvar, which is not callable`.
  Worth a better diagnostic, but not a body-block change.

## Why this is worth closing

**The rest of the toolchain already assumes these forms are variadic.**

- The **formatter** does. `fmt_when` (`src/compiler/fmt.c:1278`) lays `when`
  out as `fmt_header_items(f, 2, ...)` followed by `fmt_body_forms(f, 2, ...)`
  -- a two-item header and an N-form body, the same shape it gives `do` and
  `let`. `tur fmt` formats a multi-form `when` perfectly; the macro then
  refuses to expand it.
- The **docstrings** do. `src/turi/docstrings.c:238-239` reads
  `(when cond body ...)` and `(unless cond body ...)`. The `...` is a promise
  the prelude does not keep, and it is what `(doc 'when)` prints at the REPL
  and in the web doc panel.
- The **guides** teach the workaround as though it were the design.
  `docs/guides/binding-forms-guide.md:72` lists `when` / `while` as one row of
  the "supported body positions" table with the example
  `(when c (do (def x 1) (use x)))` -- while `while` in that same row needs no
  `do`. The `def`-in-expression-position diagnostic likewise names
  "`do`, `fn`, `let`, `when`, `while`" as body positions without distinguishing
  which of them actually is one.

**The tax is measurable.** Across `stdlib/`, `tests/`, `examples/`,
`tutorials/`, `benchmarks/`, `validation/`, `web/` and `docs/`, a paren-aware
scan finds **487 calls** to the single-body forms, of which **28 pay an
explicit `(do ...)`** purely to get a block:

| Form | uses | with a `(do ...)` body |
| --- | ---: | ---: |
| `reset` | 124 | 0 |
| `when` | 110 | 14 |
| `cloneable-reset` | 101 | 1 |
| `serial-reset` | 80 | 11 |
| `atomically` | 57 | 0 |
| `unless` | 5 | 0 |
| `with-*` (effects) | 10 | 2 |

(The scanner is at the bottom of this report's "Reproducing the counts"
section; it strips inline-C fences and matches on the parsed form, not on text.)

That is 13% of `when` sites and 14% of `serial-reset` sites writing a `do` that
carries no information. It is also 28 sites that would be a mechanical cleanup
once the forms accept a block -- and 28 places where a reader has to check
whether the `do` means something.

## Fix direction

### `when` / `unless` -- one line each, and it has been measured

```turmeric
(defmacro when   [test & body] (if test (do ~@body)))
(defmacro unless [test & body] (if test nil (do ~@body)))
```

`& body` is available at this point in the bootstrap: `cond` (`macros.tur:44`),
`do-m` (`:76`), `for` (`:84`) and `with-resource` (`:132`) are all in the same
prelude block and all use it.

Applied to a scratch tree and run:

- `(when c a b)`, `(unless c a b)`, the sweet-exp indented form, and the
  `#lang saffron` form all compile and run correctly.
- Single-form `(when c a)` is unchanged.
- Empty `(when c)` compiles -- `(do)` is nil.
- `bash tests/run.sh` (12-minute timeout, per CLAUDE.md): **3077 passed, 12
  failed.** All 12 failures are `no runnable input` on stale untracked artifact
  directories (`lang-layer-stringed`, the nine `session-*-turi` dirs,
  `errors/lang-layer-unknown`) that hold only `actual.*` / `turi.*` leftovers
  from deleted fixtures. They are pre-existing and cannot be caused by a macro
  change. **No fixture changed behavior**, and no codegen snapshot moved.

The trial patch was reverted; the tree is clean.

One thing not to break: `when` is also the **match-guard keyword**
(`elab_structs.c:3354,3640`, `elab_fns.c:3365,3976`, `e->sym_when`). That is a
positional token inside a `match` clause, never a macro call head, so the macro
change does not reach it -- but any fixture work should assert a guarded match
still compiles.

### `defer` -- fix the silent drop first

This is the one that is actively wrong rather than merely inconvenient. Two
options, in preference order:

1. Make it a body: loop `i = 1 .. len` at both `elab_forms.c:3947` and
   `:3956`, wrapping the forms in an `EX_DO` (or synthesizing a `(do ...)`
   Form and re-entering `elab_form`, which is what `elab_defer`'s sibling
   `elab_binding` already does). The capture analysis below it walks `body`,
   so it needs a single `Expr *` -- a `do` node satisfies that with no change
   to `collect_free_vars`.
2. If a body is judged wrong for `defer`, at minimum **reject** `len > 2`
   instead of dropping. Silently discarding a user's side effect is not an
   acceptable resting state either way.

Either change wants a fixture asserting both forms of `(defer a b)` run, in
`run.sh` and `run-turi.sh`.

### `reset` / `serial-reset` / `cloneable-reset` / `atomically`

Mechanically the same shape as `stm`, which sits at
`src/compiler/elab_concurrent.c:689` and already loops its body -- so the
precedent is in the same file as `atomically`'s gate. Each of these is a
delimiter around a computation, and `(do ...)` is already how a multi-step one
is written inside them, so folding the `do` in changes no semantics. The
delimiter still delimits exactly the same extent.

`atomically`'s dispatch gate (`elab_call.c:3657`) should widen to `len >= 2`
regardless of whether the body becomes variadic -- falling through the
special-form table into `unknown function or operator 'atomically'` is a
wrong diagnostic for an arity mistake, and the same pattern (`&& len == N`)
guards a dozen neighbouring rows that would report the same way.

### The stdlib `with-*` family

`stdlib/effects.tur`'s nine handlers and `with-capability` are one-line
changes each -- `[body]` becomes `[& body]` and the use site becomes
`(do ~@body)`, since `handle`'s first argument is one expression:

```turmeric
(defmacro with-write [& body]
  (handle (do ~@body) (Write [s] k) (do (println s) (resume k nil))))
```

These are lower value than `when` (10 call sites in-tree, 2 of them already
wrapping) but they are what makes the stdlib's 12-vs-14 split go away, and
leaving half a family variadic is how the next person learns the wrong rule.

### Follow-on, once the forms accept a block

- Drop the 28 now-redundant `(do ...)` wrappers.
- Fix `docs/guides/binding-forms-guide.md:72` so the `when` row stops teaching
  the workaround, and re-check the `def`-in-expression-position diagnostic
  text while there.
- `src/turi/docstrings.c:229` (`(defer body)`) becomes accurate rather than
  aspirational, and `:238-239` stop over-promising.

## Reproducing the counts

The table above came from a paren-aware scan rather than a grep, so a `(do`
on a continuation line counts and a `do` inside a string or an inline-C fence
does not. The script is small enough to re-derive: tokenize with
`"(?:[^"\\]|\\.)*"|;[^\n]*|[()\[\]{}]|[^\s()\[\]{}";]+`, strip ```` ```c ... ``` ````
fences first, parse to nested lists, then walk for heads in the single-body
set and test whether the body argument is a list whose head is `do`. Roots:
`stdlib tests examples tutorials benchmarks validation web docs`, skipping
`build*` and `node_modules`.
