# A `defn` nested inside another `defn` is accepted, and the outer function returns 0

**Severity: critical** (silently wrong runtime behavior, no diagnostic at any
stage). Found 2026-08-28 getting `turmeric-spices` CI green, against
`tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang), independently of the Linux/gcc box it was found on. `tur check` prints
nothing, the program links, and it exits 0 with the wrong answer.

**Status: RESOLVED 2026-08-29** as TUR-E0713 -- but NOT as this report
proposed. Its failure (1) is not a failure: a definition in expression position
is a shipped feature. See [Resolution](#resolution).

## Repro

A missing close paren makes the following `defn`s parse as nested definitions
inside the previous function's body.

```turmeric
(defmodule nd (export)
  (defn helper [] : int ```c return 42; ```)

  ;; One paren short: `other` is parsed as a nested definition inside
  ;; `outer`'s body.
  (defn outer [] : int
    (let [x (helper)]
      x)

  (defn other [] : int 7))

  (defn main [] : int (outer)))
```

## Observed

`tur check` prints nothing. The emitted C computes the value into `__t163` and
then throws it away:

```c
static int64_t nd__outer() {
        int64_t __t163;
        {
            int64_t __ps_164 = (nd__helper());
            if (tur_panicking) return ((int64_t)0);
            int64_t x_1368 = __ps_164;
            (void)x_1368;
            __t163 = x_1368;
        }
        return 0;          /* <-- __t163 is never returned */
}
```

The process exits `0`; `main` should have returned `42`.

`nd__other` *is* emitted (it appears twice in the output: forward declaration
and definition), so the nested `defn` is hoisted to module scope -- the nesting
is not what breaks. What breaks is that a `defn` occupying the tail position of
`outer`'s body leaves the emitter with no tail value, and it falls back to
`return 0;` rather than the block temp it just assigned.

## Why this is the worst severity in the batch

The other five defects in this batch fail loudly somewhere -- five of them in
`cc`, one silently but only by dropping code that never ran. This one **runs**,
returns a plausible value (`0` is a successful exit status, a `false`, a
"nothing found"), and does it from a source file whose only visible flaw is a
paren an editor will happily reindent around.

There are three independent failures stacked here, and each one alone would
have caught it:

1. The parser accepts a definition in expression position.
2. The elaborator does not reject a `defn` as a function-body tail.
3. Codegen emits `return 0;` for a function whose body produced a value into a
   live temp.

## Expected

A definition in expression position should be a parse or elaboration error --
that is the fix that catches the real-world cause (a paren slip), and it is the
cheap one. Failing that, codegen must not emit `return 0;` for a function whose
body assigned a result temp; that is the backstop that turns any *other* route
to the same state into a build break instead of a wrong answer.

Both are worth doing. (1)/(2) give the author the right message at the right
line; (3) makes the whole class non-silent.

## Where it bit

`spices/watch/src/watch/watch.tur`: `__watch-next-tree` stopped one paren
short, so `__watch-make-cons` and `watch-drain` became nested definitions. The
function always returned 0, so tree-mode `watch-next` could never return an
event.

Three assertions in `tests/tree_test.tur` had been failing for months with no
indication where to look. The single-file path was unaffected, because it
re-stats the target rather than reading the event buffer -- so the spice looked
mostly-working, which is why nobody pulled the thread.

## Fix direction

Start in the elaborator, not the parser: the parser has no reason to know that
`defn` is disallowed in expression position, but the elaborator already walks
the body and knows the context. Reject `EX_*` forms carrying a definition head
when they are not a direct child of a module/top-level, and point the
diagnostic at the `defn` keyword -- "a definition cannot appear here; check for
a missing close paren on the enclosing `defn`" is the message that would have
saved this one, because the missing paren is always the cause in practice.

Then, separately, audit the `return 0;` fallback. Grep the function-body tail
emitter for the path that produces a bare `return 0;` when a result temp was
declared; that pairing (temp declared, temp assigned, temp not returned) is
mechanically detectable and should be an internal compiler assertion, not a
silent zero.

## Guides to update when fixed

- docs/guides/syntax-guide.md -- it does not state that definitions are
  top-level-only.

---

## Resolution

Fixed 2026-08-29 as **TUR-E0713**. The severity assessment here was right and
the diagnostic this report asked for is the one that shipped -- but its
diagnosis listed three stacked failures, and **the first two are not failures.**

### "The parser accepts a definition in expression position" is a feature

This report's recommended fix was "reject `EX_*` forms carrying a definition
head when they are not a direct child of a module/top-level." That would have
deleted a shipped feature. `tests/fixtures/nested-fn-basic` is titled *Phase B3:
Nested function definitions* and says so:

```turmeric
(defn outer [] : int
  (defn inner [x] : int (+ x 1))
  (inner 5))                        ;; => 6
```

Three more fixtures rely on it (`bare-fat-float-result-dedup`,
`bare-fat-nontail-float-noannot`, `shift-crossfn-resume-named-fn`). So failures
(1) and (2) as listed are the feature working, and this report's own observation
that "`nd__other` *is* emitted, so the nesting is not what breaks" was the
correct thread -- it just was not followed to the conclusion that the nesting is
*intended*.

The real defect is narrower, and this report states it exactly one paragraph
later: **tail position.** Varying only that:

```turmeric
(defn a [] : int (defn g [] : int 7) 5)                 ;; => 5, correct
(defn b [] : int (let [x 42] x) (defn g [] : int 7))    ;; => 0, wrong
```

`def` and `defstruct` in tail position do it too, so it is not specific to
`defn`.

### The fix

Reject a definition as the **last** form of a function body, when the function's
declared return type owes the caller a value (`return_kind` is neither `TY_NIL`
nor `TY_NEVER`). Nested definitions anywhere else are untouched, and a `: nil`
function may still end with one.

Two tests, because neither alone is enough:

- The elaborated tail's `Expr` kind (`EX_FN_DEF`, `EX_DEF`, `EX_DEFDATA`,
  `EX_DEFECT`, `EX_EXTERN_C`, `EX_TYPECLASS_DEF`, `EX_INSTANCE_DEF`). This
  catches a macro that expands to a definition.
- The tail source form's head symbol, matched against an **exact** list of
  definition keywords. This catches `defmacro` / `defclass` / `deftype`, which
  collapse to `EX_NIL_LIT` once registered and are then indistinguishable from a
  legitimate `nil` tail.

A first attempt used a `strncmp(head, "def", 3)` prefix test for the second
half. The suite caught it immediately: `refine-call-site` tail-calls a user
function named `defined-later`, which is an ordinary expression. Exact names
only.

The diagnostic leads with the paren, because the paren is always the cause:

```
error [TUR-E0713]: function 'outer' ends its body with a definition (defn),
  so it has no value to return
note: check for a missing close paren on the enclosing (defn outer ...) -- that
  is what makes the definitions after it parse as nested ones.  A nested
  definition is otherwise fine; it just cannot be the last form
```

### The `return 0;` backstop (failure 3) was not audited

This report's failure (3) -- codegen emitting `return 0;` for a function whose
body produced a value into a live temp -- is real and is not addressed here. It
is now unreachable by this route, but the pairing it describes (temp declared,
temp assigned, temp not returned) remains mechanically detectable and would
still be worth an internal assertion.

### A wider hole, filed separately

Chasing why this was silent turned up the general case: the body-tail return-type
check (TUR-E0707 / TUR-E0709) fires for a `cstr`, `float` or `bool` tail and
**not** for a `nil` one. `(defn f [] : int nil)` is accepted and returns 0.
Definitions collapse to nil-ish, which is why this defect had no diagnostic at
all. Filed as
[nil-tail-not-checked-against-declared-return](../reported/nil-tail-not-checked-against-declared-return.md);
fixing it would subsume TUR-E0713's job, though TUR-E0713 should survive as the
more specific diagnostic, since a bare type mismatch cannot name the missing
paren.

### Regression

- `tests/fixtures/errors/definition-in-tail-position/` -- the reject, using this
  report's own missing-paren repro.
- `tests/fixtures/nested-defn-in-tail-and-non-tail/` -- the half that must keep
  working: a nested `defn` used non-last, a definition last in a `: nil`
  function, and a tail call to a function named `defined-later` (the prefix-test
  trap).

Suite: 2726 passed, 0 failed.

### Guide

`docs/guides/syntax-guide.md` gains "Definitions inside a function body" --
that they are lifted to file scope and callable by name, that they may not be
last unless the function owes no value, and that the cause is a missing paren.
