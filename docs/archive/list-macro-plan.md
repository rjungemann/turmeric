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

### `list*` (companion, optional) -- [ ] not landed

```turmeric
(list* a b c rest)
; => (cons a (cons b (cons c rest)))
```

The Lisp `list*` / Clojure `list*` convention: last argument is the
tail, prior args are prepended. Useful when you have an existing list
you want to extend at the head. Tiny macro, ~10 lines.

Include only if a real call site wants it within the same week; don't
ship speculative helpers.

### `concat` (companion, optional) -- [ ] not landed (only `str-concat`, `bytes-concat`, `seq/concat` exist)

Already exists? Check `stdlib/list.tur` before writing. If absent and
the `list-quasiquote-plan.md` work is being considered, `concat` is a
hard prerequisite for unquote-splicing semantics, so it's better to
land it here than there.

## Implementation steps

1. [x] **Locate a home.** `stdlib/list.tur` (next to `cons`/`nil-value`)
   is the natural place; `stdlib/macros.tur` is the alternative. Pick
   list.tur -- keeps related stuff colocated.
2. [x] **Write the macro.** Pattern: variadic `& xs`, recursive expansion
   into nested `cons`. Reference `cond` in `stdlib/macros.tur:35` for
   the rest-args pattern. (Landed at `stdlib/list.tur:193` as nested
   `tcons`/`tnil`, not `cons`/`nil-value` -- see "Carrier type and the
   typed `(Cons A)` boundary" below for why.)
3. [x] **Docstring.** Full `;;;` block per CLAUDE.md standard. Example
   must use `; =>` to show expansion or evaluated result. Call out the
   "elements must unify to one type" gotcha. (`stdlib/list.tur:176-192`.
   The "unify to one type" gotcha is not explicitly called out -- minor
   follow-up.)
4. [~] **Fixture.** `tests/fixtures/typed/list-macro/` covering: empty
   list, single element, several elements of one type, a type-error
   case for mixed types. Pattern mirrors existing typed fixtures.
   (Empty / single / many-length / handwritten-equivalence cases are
   present; the **type-error case for mixed types is still missing**.)
5. [x] **Re-run `just docs`.** The macro must show up in the generated
   HTML reference. Also rebuild `stdlib/docstrings.tur` if the doc
   lookup table is part of the change set. (`stdlib/docstrings.tur:436`.)
6. [ ] **Spot-check stdlib for migration candidates.** Cheap wins only --
   don't churn working `(cons ...)` chains for stylistic reasons.
7. [ ] **Add unify-or-fail callout to the `list` docstring.** The
   docstring at `stdlib/list.tur:176-192` does not explicitly mention
   the "elements must unify to one type" gotcha. Add a short note (and
   ideally a `; =>` example showing the failure shape) so users reach
   for `tuple` for heterogeneous needs instead of fighting the
   typechecker.
8. [ ] **Land `list*` (cons-onto-tail).** Variadic macro with the
   Lisp/Clojure semantics: last arg is the existing tail, prior args
   are prepended. Expansion mirrors `list` -- right-fold over the
   leading args terminating in the tail expression rather than
   `(tnil)`. Same carrier as `list` (`:int`); same docstring contract
   re: element-type unification (and now also tail-type unification
   with the element type). Gate on a real call site per "Surface
   design" -- don't ship speculative.
   - Home: `stdlib/list.tur`, immediately after `list`.
   - Docstring: full `;;;` block; `; =>` example showing both
     `(list* 1 2 (tnil))` and `(list* 1 (list 2 3))` shapes.
   - Fixture: extend `tests/fixtures/typed/list-macro/` with a
     `list*` round-trip (`(list-eq? (list* 1 2 (list 3 4))
     (list 1 2 3 4) ...)`) and an empty-prefix case
     (`(list* (tnil))` -> `(tnil)`).
   - Regenerate `stdlib/docstrings.tur` via `just docs`.
9. [ ] **Land list-level `concat`.** Two-list (or variadic) `concat`
   over the `:int` carrier; right-fold the first list onto the
   second. Name collision check: `str-concat`, `bytes-concat`, and
   `seq/concat` already exist -- use bare `concat` only if it doesn't
   shadow anything in scope after import; otherwise prefer
   `list-concat`. Audit before committing.
   - Home: `stdlib/list.tur`, near `list-length` / `list-eq?`.
   - Shape: `defn` (not macro) -- it's a runtime traversal, not a
     syntactic fold. Recursive over the first arg; terminates by
     returning the second.
   - Docstring: full `;;;` block; `; =>` example covering empty-LHS,
     empty-RHS, and both-non-empty.
   - Fixture: new `tests/fixtures/typed/list-concat/` (separate from
     `list-macro/` so the failures triangulate cleanly).
   - Note for `list-quasiquote-plan.md`: unquote-splicing semantics
     should call into this same `concat`, so the signature it picks
     here is load-bearing for that plan. Pick a shape that handles
     N-ary splicing without re-folding at the macro level.
   - Regenerate `stdlib/docstrings.tur` via `just docs`.
10. [ ] **Add a "use `tupleN`" diagnostic for heterogeneous `(list ...)`.**
    When a `tcons`/`tnil` chain fails to unify across element types,
    emit a hint suggesting the matching `tupleN` constructor. Tuples
    have shipped, so this is no longer speculative.
    - Hook site: the elaborator's unify-failure path for `tcons`
      argument positions. The diagnostic should fire on the
      second-or-later element whose type fails to unify with the
      first element's, not on the surface `list` form (the macro is
      already gone by the time elab runs).
    - Hint shape: if the chain has N elements with N in {2, 3, 4, 5},
      suggest `(tupleN <args>)` literally. For N > 5, suggest
      "consider grouping into nested tuples or a record".
    - Carrier-vs-typed: the hint should not steer users toward
      `make-struct Cons` for the homogeneous case -- that's the
      escape hatch documented above, not the recommended response
      to a unify error. Heterogeneous -> tuple; homogeneous-but-
      wanting-`(Cons A)` -> the docstring's escape hatch.
    - Fixture: `tests/fixtures/typed/list-macro-tuple-hint/` with a
      failing `(list 1 "x" 3.14)`-shaped input and an `expected.stderr`
      asserting the hint text. Mark as a compile-fail fixture per
      the existing typed-fixture conventions.
    - Provenance gating: **decided -- option (b), no gating.** The
      hint fires for any `tcons` arg whose type fails to unify with
      the first element's, regardless of whether the call came from
      a `list` macro expansion or hand-written `tcons`. Rationale:
      the suggestion is genuinely useful in both cases (mixed-type
      `tcons` is almost always a "wanted a tuple" mistake), and it
      avoids threading expansion-provenance markers through the
      macro->elab boundary just to scope a hint. Revisit only if a
      real call site shows the hint firing where it shouldn't.

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
- **Interaction with `tuple-type-plan.md`.** Tuples have landed
  (`tuple3` / `tuple4` / `tuple5` constructors and positional
  accessors -- see `tests/fixtures/tuple-345-basic/`). Users who
  reach for `(list 1 "x" 3.14)` and hit the unify error should be
  pointed at the right tool, not left to puzzle out the message.
  Tracked as Implementation step 10 below: an elab-level diagnostic
  that, when a `tcons`/`tnil` chain fails to unify across element
  types, appends a hint suggesting the matching `tupleN` constructor.

## Carrier type and the typed `(Cons A)` boundary

`(list 1 2 3)` expands to `(tcons 1 (tcons 2 (tcons 3 (tnil))))` and
yields the `:int` carrier, **not** a `(Cons int)` struct cell. This
is a deliberate design point, not a TODO, and the asymmetry is worth
documenting explicitly so future readers don't try to "fix" it via
the wrong mechanism.

### Why `:int` and not `(Cons A)`

- `Cons` is a `defstruct [A]` (`stdlib/list.tur:16`). Building a typed
  `(Cons A)` value requires `make-struct Cons` plus an
  `(:: ... (Cons A))` ascription so the elaborator can resolve `A`.
- `tcons` / `tnil` (`stdlib/list.tur:33`, `:68`) are deliberately
  type-erased `defn`s with signature `:int :int -> :int`. They are
  the carrier-level path; the whole `:int`-taking list API
  (`list-length`, `list-eq?`, etc.) is built against them.
- A macro can't synthesise the `(:: ... (Cons A))` ascription on its
  own -- it has no access to the surrounding expected type. So
  expanding into `make-struct Cons` would work only when the result
  is used in an explicitly ascribed position; everywhere else it
  would fail to infer `A` and force the user to add the ascription
  anyway. The carrier-level expansion is the form that "just works"
  without context.

### Why TS4 poly-ADT monomorphisation does not change this

It's tempting to assume the polymorphic-ADT monomorphisation plan
([`typed-slots-ts4-poly-adt-plan.md`](typed-slots-ts4-poly-adt-plan.md))
would eventually let `(list 1 2 3)` pick up `(Cons int)` codegen
"for free". It will not, because:

- TS4 mirrors the GS5 struct-app instantiation work for **ADT
  constructors** (`defdata` ctors like `Just`/`Nothing`). `Cons` is
  a `defstruct`, so TS4's machinery doesn't apply to it; struct-app
  monomorphisation is already handled by GS5.
- The macro's expansion goes through `tcons` / `tnil`, which are
  plain `defn`s. No ADT codegen path runs through them; they are
  typed `:int :int -> :int` at the source level and would remain so
  under any monomorphisation scheme that operates on constructors.
- The `tcons-of [A]` typed companion (`stdlib/list.tur:56`) already
  returns `:(Cons A)` today. The question of how to get there from
  `(list ...)` is an **elab/inference** problem (threading the
  expected type into a macro-expansion result so `A` resolves), not
  a codegen one. TS4 doesn't move that needle.

### The escape hatch (current accepted state)

When a call site genuinely needs a typed `(Cons A)`, write the
explicit struct form. The docstring at `stdlib/list.tur:176` already
points to this; the design contract is:

```turmeric
;; carrier-level (what (list ...) gives you)
(let [xs (list 1 2 3)]
  (list-length xs))            ; => 3

;; typed (Cons int) cell
(let [xs (:: (make-struct Cons 1 (tnil)) (Cons int))]
  (thead xs))                  ; => 1
```

### When to revisit

The right trigger for a typed-list macro is **not** "TS4 landed" --
it's a real call site where the carrier-level expansion forces an
awkward ascription that the macro could have inserted. Until that
shows up, the carrier-level expansion is the right default and the
explicit `make-struct Cons` form is the documented escape hatch.

A future "typed list macro" follow-up plan, if it ever lands, would
sit alongside this one and depend on macro-level access to the
expected type (an elab feature), not on TS4.

## Success criteria

- [x] `(list 1 2 3)` typechecks and prints the same as the explicit
  cons chain. (Verified via `test-eq-handwritten` in the fixture.
  **Note:** the carrier type is `:int` per the tcons/tnil chain, not
  the typed `(Cons A)` constructor. The original wording of this
  criterion ("typechecks as `(Cons int)`") was aspirational; see
  "Carrier type and the typed `(Cons A)` boundary" above for why
  that's a separate, deferred concern rather than a defect.)
- [x] `(list)` is `(tnil)` and typechecks in any position that
  accepts an empty list. (Fixture's `test-empty` confirms `tnil?`.)
- [ ] A type-mismatch fixture demonstrates the unify-or-fail
  behaviour.
- [x] The generated docs render `list`'s `;;;` block with a usable
  example. (`stdlib/docstrings.tur:436`.)

## When to do this

Cheap enough to land any time. The minimum bar: at least one real
call site in stdlib or a spice currently using a nested `cons` chain
that would read better as `(list ...)`. Without that, defer -- adding
sugar without a consumer is exactly the trap CLAUDE.md warns about.
