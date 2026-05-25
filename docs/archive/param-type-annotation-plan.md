# Plan: Fix spaced `: T` parameter type annotations

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Compiler / Reader / Language

---

## Overview

Turmeric advertises two interchangeable parameter type annotation syntaxes:

- **Fused** -- `[n :float]` (colon glued to the type name; reader produces
  `F_KEYWORD(:float)`)
- **Spaced** -- `[n : float]` (colon separated by whitespace; reader produces
  `F_TYPE_ANN(F_SYM("float"))`)

The fused form works. The spaced form is broken inside `defn` parameter
vectors: the parameter ends up with no type annotation, the body
elaborates against an untyped binding, and the substructural / linearity
checker then fires `TUR-E0005` use-after-move on the first re-reference of
the parameter. This makes a sensible piece of source code look like a
linearity bug to the user, which is doubly confusing because nothing about
the source involves moves.

The relevant elaborator code (`src/compiler/elab_fns.c:347-450`) is
explicitly written to accept `F_TYPE_ANN`, and the reader
(`src/compiler/reader.c:470-487`) is explicitly written to produce
`F_TYPE_ANN` when `:` is followed by whitespace -- so the intent is
clear. Something between intent and behavior is broken, and the
diagnostic surface area (use-after-move on an untyped binding) is the
worst possible failure mode for diagnosing it.

This plan fixes the spaced form in `defn`, audits the other annotation
sites for the same gap, fills in a missing site (`fn`), and adds the
fixture coverage that would have caught this regression in the first
place.

---

## Reproducer

```
turmeric> (defn square [n : float] (* n n))
<eval>:1:31: error [TUR-E0005]: use-after-move: binding 'n' was moved
                                and cannot be used again
1 | (defn square [n : float] (* n n))
  |                               ^
<eval>:1:29: note: moved here
1 | (defn square [n : float] (* n n))
  |                             ^
```

vs. the equivalent fused syntax which works:

```
turmeric> (defn square [n :float] :float (* n n))
=> #<fn square>
turmeric> (square 3.0)
=> 9
```

---

## Non-goals

- **New annotation positions.** This plan does not add type annotations
  to `let` bindings, `defmacro` params, or `loop` bindings. Today only
  function-like forms carry them; that scope stays the same.
- **Inference improvements.** The plan only fixes parsing of declared
  annotations; it does not infer types for untyped parameters. (Untyped
  params remaining linear-by-default is a separate design question.)
- **Better linearity diagnostics in general.** The plan does include
  one targeted message (a syntax hint when use-after-move fires on a
  parameter that has no declared type), but a full review of TUR-E0005
  output is out of scope.

---

## Confirmed gaps

All tested against `build/tur` from `main`. Each row is a one-line REPL
input.

| # | Input | Expected | Actual |
|---|---|---|---|
| 1 | `(defn f [n : float] (* n n))` | accepts | TUR-E0005 use-after-move on `n` |
| 2 | `(defn f [n  :  float] (* n n))` (extra ws) | accepts | TUR-E0005 use-after-move on `n` |
| 3 | `(defn f [a : float b : float] :float (+ a b))` | accepts | overload resolution fails on `+` (both params untyped) |
| 4 | `(defn f [n : (vec int)] (vec-len n))` | accepts | param mistyped, body fails to resolve |
| 5 | `((fn [x :int] (* x x)) 3)` | accepts | `fn: parameter name must be a symbol` |
| 6 | `((fn [x : int] (* x x)) 3)` | accepts | same |
| 7 | `((fn [x] :int (* x x)) 3)` | accepts | `call head must be a symbol or closure expression` |

Gaps **1-4** are the spaced-syntax bug in `defn`'s parameter loop.
Gap **5-7** is a separate, pre-existing gap: `fn` (anonymous lambda)
never had parameter type annotations at all -- only return-type
annotations work (`elab_fns.c:1397`). The fused form `[x :int]` is
rejected outright by the `if (p->tag != F_SYM)` check at
`elab_fns.c:1316`. Direct anonymous-lambda application is also a
separate existing limitation; see
[`docs/direct-anonymous-lambda-application-plan.md`](direct-anonymous-lambda-application-plan.md).

For comparison, the spaced **return** type works in `defn`:

```
turmeric> (defn f [n :float] : float (* n n))
=> #<fn f>
```

So the fix is local to parameter-vector handling, not the whole
keyword path.

---

## Root cause investigation (must do first)

The bug needs about ten minutes of instrumented reading before code
changes. The static read of the code says the path should work; the
runtime says it doesn't. We need to know which side is lying before
writing a fix.

Steps:

1. **Verify reader output.** Temporarily print every `Form *p` at the
   top of the param loop in `src/compiler/elab_fns.c:181`. Run
   `(defn f [n : float] (* n n))` through `build/tur eval`. The
   expected dump is:

   ```
   params: F_VEC len=2
     [0] F_SYM "n"
     [1] F_TYPE_ANN -> F_SYM "float"
   ```

   - If the dump matches, the reader is fine and the bug is in
     `elab_fns.c` (some earlier branch in the loop is consuming the
     `F_TYPE_ANN` without typing the previous param).
   - If `[1]` is anything else (two bare `F_SYM`s, a list, etc.), the
     reader's spaced-keyword branch (`reader.c:470-487`) is not firing
     in this context. The conditions there only fire on
     `c2 == ' '|'\t'|'\n'|'\r'|'('|'['|-1`; if the test fails with
     `]` immediately after the symbol (no space before `]`), the
     `F_TYPE_ANN` would never be produced. (See the
     `(defn f [n :(vec int)] ...)` row in the gaps table -- the
     diagnostic implies the parser did see it as a type, which would
     argue for the reader being healthy.)

2. **Pin the failing branch.** Either:
   - **Reader case:** widen the `c2` allowlist in `read_keyword`. Likely
     additions: `']'`, `')'`, `'}'`, `';'`, `'"'`, and EOF (already
     present). The current list is too narrow if a colon is the last
     thing in a delimited form: `:` immediately followed by `]` falls
     through to the bare-colon branch at `reader.c:490-499` and emits
     `F_SYM(":")`.
   - **Elab case:** trace which of the earlier branches in the param
     loop is matching `F_TYPE_ANN` before line 347 reaches it. The
     `(: a T)` equality-constraint branch at `elab_fns.c:194-228`
     explicitly inspects `F_TYPE_ANN` (line 202) -- if a single
     `F_TYPE_ANN(F_SYM("float"))` matches the "type-variable equality"
     shape (`F_LIST len 2 with F_TYPE_ANN at [0]`), it could be
     consuming the annotation as a constraint instead of attaching it
     to the previous param. Read that branch with the running reader
     dump and confirm whether it fires.

3. **Reproduce in a unit test.** Once the failing branch is identified,
   freeze the reproducer as a `tests/fixtures/defn-spaced-typeann/` case
   so any future change to the param loop catches it.

Without this step, any patch is a guess. The plan deliberately does not
prescribe a fix until step 1 produces a verdict.

---

## Phases

Each phase is an independent commit and an independent test fixture.

### TA0 -- Repro fixture (lands first, intentionally failing)

Add `tests/fixtures/defn-spaced-typeann/` with cases that exercise the
spaced syntax across:

- single param, primitive: `[n : float]`
- multi param: `[a : float b : float]`
- compound type: `[xs : (vec int)]`
- spaced return type after fused param (regression guard): `[n :float] : float`
- spaced return type after spaced param: `[n : float] : float`

Mark expected output. CI will go red until TA1 lands; that is the point.

**Files:** new fixture directory only. No source changes.

### TA1 -- Fix the defn param loop (or the reader, per step 2 of the investigation)

Single targeted fix at the site identified above. Either:

- `src/compiler/reader.c` -- broaden `read_keyword`'s spaced-colon
  trigger.
- `src/compiler/elab_fns.c` -- reorder or guard the
  equality-constraint branch (or whichever earlier branch is
  consuming `F_TYPE_ANN`) so it doesn't shadow the param-annotation
  branch at line 347.

Acceptance: TA0 fixture goes green. No regressions in
`tests/fixtures/defn-*`.

### TA2 -- `fn` anonymous lambda param annotations

`elab_fns.c:1314-1335` rejects any non-`F_SYM` parameter outright. This
predates the annotation feature and is the reason `(fn [x :int] ...)`
errors with "parameter name must be a symbol".

The fix mirrors what `extern-c` already does at `elab_fns.c:1649-1683`:
detect `F_KEYWORD` and `F_TYPE_ANN` after a name, run them through
`type_expr_from_form`, attach to the preceding param. Default-typing
for un-annotated lambda params (currently `TY_INT`) stays as-is so
existing untyped `fn` callsites don't change behavior.

Acceptance: new fixture `tests/fixtures/fn-typed-params/` covers fused
and spaced forms; the lambda elaborates and the body type-checks.

### TA3 -- Diagnostic improvement on misparsed annotations

When TUR-E0005 fires on a parameter whose declared type is `TY_UNKNOWN`
*and* the parameter's source span is immediately followed by tokens that
look like a botched annotation (`:`, `: T`, `:T` followed by junk), add
a `note:` to the diagnostic:

```
note: parameter 'n' has no type annotation; did you mean `[n :float]`?
  -- spaces are allowed: `[n : float]` is also valid
```

This will not silence the underlying use-after-move -- that is the
real error if the user actually intended an untyped linear param --
but for the 99% case where the user typoed a colon, it points at the
fix instead of at the moved-here / used-here pair which is a red
herring.

**Files:** `src/passes/borrow_check.c` near the existing
`use-after-move` emission, and a small `binding->source_span` lookup.

### TA4 -- Audit other annotation-bearing forms

Sweep every site that handles parameter or field type annotations and
confirm both fused and spaced forms work. Sites to verify (each gets a
one-line fixture if it works, a one-line fix + fixture if it doesn't):

| Site | File | Form | Status |
|---|---|---|---|
| `defn` params | `elab_fns.c:181-555` | `(defn f [x :T] ...)` | broken (TA1) |
| `defn` return | `elab_fns.c:723` / `:1397` | `(defn f [x :T] : T ...)` | works |
| `fn` params | `elab_fns.c:1314-1335` | `(fn [x :T] ...)` | broken (TA2) |
| `fn` return | `elab_fns.c:1397` | `(fn [x] : T ...)` | works |
| `extern-c` params | `elab_fns.c:1649-1683` | `(extern-c f [x :T] :T)` | works |
| `definstance` methods | `elab_typeclasses.c:339-485` | inside `(definstance ...)` | unverified |
| `defclass` methods | `elab_typeclasses.c:1543-1625` | inside `(defclass ...)` | unverified |
| `defstruct` fields | `elab_structs.c:325-495` | `(:field :T)` | works (handled) |
| `defprotocol` methods | (search) | per-method signatures | unverified |
| `defgadt` ctors | `elab_typeclasses.c` GADT block | `(Ctor field : T)` | unverified -- bare `:` is intentional here per `reader.c:490-499`, so this site is the *reason* the bare-colon fallback exists. Confirm spaced annotations don't regress it. |

For each "unverified" row: one positive fixture (typed) + one negative
fixture (mis-typed argument should be rejected). If anything is broken,
fix is a small loop-body edit; if not, the fixture is documentation.

`defmacro` is intentionally **not** in this list -- macro params are
unsited at runtime, and the loop at `elab_macros.c:1011-1018`
deliberately rejects keywords with a clear message. Leave it alone.

### TA5 -- Doc update

Update `CLAUDE.md` "Sweet-Expression Style" / sample blocks (lines
~75-200) to show one example of each form. Update the docstring in
`elab_fns.c:119-127` so the comment matches actual behavior. Add a
short paragraph to `docs/api/` (the generated docs read source
comments, so updating the elab comment may flow through automatically
-- check after `just docs`).

---

## Test plan

```
tests/fixtures/defn-spaced-typeann/      # TA0
tests/fixtures/defn-spaced-multi-param/  # TA0
tests/fixtures/defn-spaced-compound/     # TA0  -- [xs : (vec int)]
tests/fixtures/fn-typed-params/          # TA2
tests/fixtures/fn-spaced-typeann/        # TA2
tests/fixtures/typeann-diag-hint/        # TA3  -- checks the note: text
tests/fixtures/extern-c-spaced-typeann/  # TA4 regression guard
tests/fixtures/defgadt-spaced-typeann/   # TA4 regression guard
tests/fixtures/definstance-spaced/       # TA4 (or skip if not supported)
```

Every fixture is one or two lines of source plus expected output, in
the existing `tests/fixtures/<name>/{main.tur, expected.txt}` style.
TSan-only fixtures: none of these need the TSan path; they're
parse / elab fixtures.

Manual smoke before/after each phase: `just test` end-to-end, plus the
exact REPL reproducers from the gaps table above.

---

## Risk and rollout

- **Risk: the equality-constraint branch (`elab_fns.c:194-228`)
  swallows valid `F_TYPE_ANN` annotations.** This is the most likely
  TA1 failure mode given the static read of the code. If the
  investigation in step 2 confirms it, the fix is a tightening of
  that branch's match condition (require the surrounding parens
  explicitly, not lone `F_TYPE_ANN`). A wrong tightening could break
  `(: a T)` equality constraints in HKT code. Fixture
  `tests/fixtures/eq-constraint-*` (if it exists) plus a few new
  ones cover that.
- **Risk: widening `read_keyword`'s spaced-trigger list breaks the
  bare-colon emission for GADT constructors.** `reader.c:490-499`
  intentionally emits `F_SYM(":")` for `(Ctor field : RetType)`
  positions. If TA1 lands on the reader side, the new test cases
  in TA4 are the guard.
- **Risk: TA3's heuristic diagnostic note misfires on genuinely
  untyped linear params.** Phrase the note as "did you mean..." so
  it reads as a hint, not a claim. Worst case is a redundant line
  in the diagnostic; the original TUR-E0005 still prints.

No rollback story needed: each phase is one commit, reverting any
phase reverts only its own behavior. TA0 lands first as a failing
test, so the order of TA1/TA2/TA3/TA4 doesn't matter beyond TA1
needing to land for TA0 to go green.
