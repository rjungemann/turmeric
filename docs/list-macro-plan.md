# List Literal Macro Plan

## Scope and non-scope

In scope:
- A `(list a b c)` variadic macro that expands to the existing
  `(cons a (cons b (cons c (nil-value))))` chain over `stdlib/list.tur`.
- An honest answer to "can we add `[a b c]` reader sugar?" -- short
  version: **no**, `[]` is already `F_VEC` and is load-bearing for
  binding vectors, param lists, refer lists, and destructure patterns.
- A couple of convenience companions if they fall out cheaply
  (`list*`, `concat`).

Out of scope:
- Quasiquote at runtime (separate plan: `list-quasiquote-plan.md`).
- Heterogeneous fixed-arity grouping -- that's the tuple plan
  (`tuple-type-plan.md`). The two features are **not** substitutes for
  each other; see "Ordering with tuple-type-plan.md" below.
- Replacing existing `(cons ...)` call sites in stdlib. The macro is
  additive; opportunistic migration only.

## Current state

What exists today:

| Mechanism | Form | Notes |
|-----------|------|-------|
| `(cons h t)` | typed `(Cons A)` | `stdlib/list.tur:33`; the only constructor |
| `(nil-value)` | `(Cons A)` | terminator |
| `[a b c]` | `F_VEC` | reader form; **already meaningful** as a syntactic vector |
| `` `(a b c) `` | `F_QUASIQUOTE` | compile-time only, builds `Form` trees for macros (`elab_macros.c:118`) |
| `'(a b c)` | `F_QUOTE` | compile-time data; not a runtime list constructor |
| `defmacro` with `& clauses` rest-args | works | see `stdlib/macros.tur:35` (`cond`) |

What's missing:
- A direct way to write "give me a list of these N values" without
  hand-chaining `cons` calls. Today you write
  `(cons 1 (cons 2 (cons 3 (nil-value))))` or use a hand-rolled helper.

## Use cases

1. Building short fixed lists inline:
   `(list 1 2 3)` vs `(cons 1 (cons 2 (cons 3 (nil-value))))`.
2. Tests and fixtures that construct small expected lists.
3. Stdlib internals where a helper currently builds nested cons by hand.

If you can't name three real spots in the current tree where the cons
chain is awkward, this is sugar-for-sugar's-sake. Spot-check
`stdlib/list.tur` and a few fixtures before committing.

## Design options

### Option A -- macro only (`(list a b c)`)

A `defmacro list [& xs]` that emits the right-folded cons chain. No
reader changes. No new type kinds.

- Pros: 20-line patch in `stdlib/macros.tur` (or `stdlib/list.tur`).
  No parser, elaborator, or codegen work.
- Cons: still parenthesised. Not "literal" in the lexical sense.

### Option B -- macro plus a new reader sigil (e.g. `#(a b c)` or `@[a b c]`)

Add a fresh reader dispatch that emits the same expansion as `list`.

- Available sigils per `src/compiler/reader.c:1909-2001`: `#(` is
  currently free, `#l[` would be free, plain `@[` is free (`@` exists
  but `@[` is not dispatched). Plain `[` is **not** free.
- Pros: terser at use sites; visually distinct from vector binding.
- Cons: every new sigil is a tax on readers learning the language, and
  on tooling (LSP, formatter, doc generator). The win over `(list ...)`
  is small.

### Option C -- overload `[a b c]` based on position

Make `F_VEC` mean "list literal" in expression position and "binding
vector" in let/fn/refer position.

- Pros: the syntax users actually want.
- Cons: context-sensitive form semantics are a footgun. Tools and
  macros have to know "what position am I in" to interpret a vector.
  Hard rejection.

### Recommendation

**Option A only.** Add `(list ...)` as a macro in `stdlib/list.tur`.
Skip the reader sugar entirely. If after six months of real use the
`(list ...)` ceremony shows up as a measurable annoyance, revisit
Option B then -- the macro lands either way and Option B is purely
additive on top of it.

Do **not** add `[]` sugar under any framing. The collision with
`F_VEC` is not navigable without making the parser context-sensitive,
which is a tax this codebase does not need.

## Surface design

### `list`

```turmeric
(list)            ; => (nil-value)
(list 1)          ; => (cons 1 (nil-value))
(list 1 2 3)      ; => (cons 1 (cons 2 (cons 3 (nil-value))))
```

Type: `(Cons A)` where all elements must unify to a single `A`. This
is the same constraint the explicit cons chain already imposes; the
macro doesn't change typing, just lexical shape.

Error case: heterogeneous elements (`(list 1 "x" 3.14)`) fail
typechecking at the let/return that consumes the value, the same way
the equivalent cons chain fails today. Document this in the docstring
so users reach for `tuple` (when it lands) for heterogeneous needs.

### `list*` (companion, optional)

```turmeric
(list* a b c rest)
; => (cons a (cons b (cons c rest)))
```

The Lisp `list*` / Clojure `list*` convention: last argument is the
tail, prior args are prepended. Useful when you have an existing list
you want to extend at the head. Tiny macro, ~10 lines.

Include only if a real call site wants it within the same week; don't
ship speculative helpers.

### `concat` (companion, optional)

Already exists? Check `stdlib/list.tur` before writing. If absent and
the `list-quasiquote-plan.md` work is being considered, `concat` is a
hard prerequisite for unquote-splicing semantics, so it's better to
land it here than there.

## Implementation steps

1. **Locate a home.** `stdlib/list.tur` (next to `cons`/`nil-value`)
   is the natural place; `stdlib/macros.tur` is the alternative. Pick
   list.tur -- keeps related stuff colocated.
2. **Write the macro.** Pattern: variadic `& xs`, recursive expansion
   into nested `cons`. Reference `cond` in `stdlib/macros.tur:35` for
   the rest-args pattern.
3. **Docstring.** Full `;;;` block per CLAUDE.md standard. Example
   must use `; =>` to show expansion or evaluated result. Call out the
   "elements must unify to one type" gotcha.
4. **Fixture.** `tests/fixtures/typed/list-macro/` covering: empty
   list, single element, several elements of one type, a type-error
   case for mixed types. Pattern mirrors existing typed fixtures.
5. **Re-run `just docs`.** The macro must show up in the generated
   HTML reference. Also rebuild `stdlib/docstrings.tur` if the doc
   lookup table is part of the change set.
6. **Spot-check stdlib for migration candidates.** Cheap wins only --
   don't churn working `(cons ...)` chains for stylistic reasons.

## Risks and open questions

- **Naming collision with `stdlib/list.tur` as a module name.** The
  module is already called `list`; a function/macro named `list` inside
  it should be fine (modules and symbols are separate namespaces in the
  elaborator), but verify by trying it before committing. If there's a
  collision, fall back to `list-of` or accept the small awkwardness of
  importing `list/list`.
- **Macro expansion size.** `(list a b c d e f g h)` expands to 8 nested
  `cons` calls. For very long literal lists, codegen and debug output
  bloat. Cap at... no, don't cap; if a user writes 100-element list
  literals, that's their call. Just document expected expansion shape.
- **Interaction with `list-quasiquote-plan.md`.** If that lands later
  with unquote-splicing, the splice form needs to construct via the
  same primitive `cons`/`nil-value` -- which `list` already does. No
  conflict.
- **Interaction with `tuple-type-plan.md`.** Users may reach for
  `(list 1 "x" 3.14)` for heterogeneous grouping, hit the unify error,
  and feel the language failed them. Mitigate via the docstring
  example and, once tuples land, a diagnostic that suggests `tuple`
  for mixed-type element lists. Tracking only -- diagnostic is a
  follow-up.

## Success criteria

- `(list 1 2 3)` typechecks as `(Cons int)` and prints the same as
  the explicit cons chain.
- `(list)` is `(nil-value)` and typechecks in any position that
  accepts an empty list.
- A type-mismatch fixture demonstrates the unify-or-fail behaviour.
- The generated docs render `list`'s `;;;` block with a usable
  example.

## When to do this

Cheap enough to land any time. The minimum bar: at least one real
call site in stdlib or a spice currently using a nested `cons` chain
that would read better as `(list ...)`. Without that, defer -- adding
sugar without a consumer is exactly the trap CLAUDE.md warns about.
