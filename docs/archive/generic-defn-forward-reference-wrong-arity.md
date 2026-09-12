# A forward-referenced generic defn is declared with the arity of its type-parameter list

**Severity: medium-high** -- a hard compile error whose message describes a
function the author did not write ("returns int, which is not callable -- did
you mean to pass all 1 argument(s)?"), and whose trigger is **source order**.

**Status: RESOLVED** 2026-09-12. Found migrating `crdt/ormap` to a parametric
`(ORMap V)`, where making existing helpers generic broke callers that had
compiled for months.

## Repro

```turmeric
(defstruct B [V] [e : int])

(defn caller [^borrow a : (B int) ^borrow b : (B int)] : int
  (helper a b))                                   ;; callee defined BELOW

(defn helper [V] [^borrow a : (B V) ^borrow b : (B V)] : int
  (+ (.e a) (.e b)))
```

```
error [TUR-E0002]: function 'helper' returns int, which is not callable
  -- did you mean to pass all 1 argument(s)?
```

Move `helper` above `caller` and it compiles. So it reads as "generics cannot be
forward referenced", which is not a rule anyone wrote down.

## Mechanism

The forward-declaration pre-pass takes the vector right after the name as the
value parameters. For a generic defn that vector is the **type** parameters:

```
(defn f [V] [params] : R body)                 ;; 2 vectors
(defn f [V] [(C V)] [params] : R body)         ;; 3, with a constraint vec
```

So `helper` was forward-declared with arity 1 -- the length of `[V]` -- and a
2-argument call looked like over-application of a 1-argument function returning
`int`.

**Two** pre-passes had it, with separate code:

- `elab_toplevel.c` -- the return-type probe there already skipped the
  type-param vec (`poly-defn-recursive-return-type-inference` added it), but the
  arity scan next to it still used `name_idx + 1`.
- `elab_module.c` -- the `defmodule` half did not know about type-param vectors
  at all, so it got the arity **and** the return type wrong (the return slot is
  derived from the same index). A `: bool` generic was forward-declared `int`.

## Fix

Point both arity scans at the params index the type-param skip already computes,
and give `elab_module.c` that skip (including the optional constraint vec).

## Fixtures

- `tests/fixtures/generic-defn-forward-reference` -- top level, caller above
  callee, with and without a constraint vector.
- `tests/fixtures/generic-defn-forward-reference-defmodule` -- the defmodule
  half, returning `: bool` so the return-type half is covered too. Separate file
  because a `defmodule` must be the first form in its own.
