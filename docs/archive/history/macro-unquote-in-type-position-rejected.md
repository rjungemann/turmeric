---
title: Unquote (`~symbol`) in type-annotation position is rejected by the type parser
category: Reported
severity: Blocks "macro that emits a typed defn parameterized over the component type"
discovered: 2026-06-11, executing ECS prereq plan step 3 (post-gap-E upgrade of defworld)
resolved: 2026-06-11. Fix in `src/compiler/elab_macros.c::substitute_params` --
  the F_TYPE_ANN case now recurses into the payload instead of returning
  the wrapper as-is. Unquotes inside type-position slots participate in
  the same substitution pass as everywhere else, so `~SymbolName` is
  resolved to the macro argument before the expansion reaches the type
  parser. Regression: `tests/fixtures/macro-unquote-in-type-position/`.
---

# Unquote (`~symbol`) in type-annotation position is rejected by the type parser

> **Status: fixed 2026-06-11.** The minimal repro `(defmacro mk-getter
> [WName TName] \`(defn get-x [w : ~WName] : ~TName w))` now expands.
> The ECS spice's `defcomponent-accessors` macro -- which mints typed
> `get-<Comp>` / `set-<Comp>!` / `has-<Comp>?` for one (world, component)
> pair -- works end-to-end against struct-by-value components and is
> regression-tested by `tests/defcomponent-accessors.tur`.
>
> Root cause was upstream of the type parser: the reader wraps
> `: type-expr` into an F_TYPE_ANN whose payload holds the type
> expression. `substitute_params` had a leaf case for F_TYPE_ANN that
> returned the wrapper untouched, so unquotes inside the payload were
> never substituted from macro args -- they reached
> `type_expr_from_form` as live F_UNQUOTE forms and tripped the
> "unsupported type expression form" diagnostic. The fix recurses
> into the payload, mirroring the F_VEC / F_LIST handling already in
> the same function.

## Summary

A `defmacro` body that uses `~SymbolName` inside a backquoted type
annotation -- e.g. `[w : ~WName]` or `: ~ReturnType` -- fails to
expand with:

```
error: unsupported type expression form (expected symbol, keyword, or list)
```

The diagnostic points at the macro definition site, not at any call.
Type-position parsing apparently runs before quasi-quote expansion,
so the unquote form survives to the type parser, which rejects
anything that isn't a `F_SYM`, `F_KEYWORD`, or `F_LIST` (see
`src/compiler/elab_types.c::parse_type_form`).

Three substitution positions exhibit the bug, all identical in form:

- `[param : ~T]` -- parameter type
- `[name : ~T value]` -- typed let binding
- `defn nm ... : ~T body` -- return type

## Severity

Blocks macro-emitted typed accessors keyed on a component type. The
ECS spice's `defworld` upgrade hit this immediately when trying to
emit per-component `get-<Comp>` / `set-<Comp>!` / `has-<Comp>?`
wrappers around `dense-get` / `dense-set!` / `dense-has?`.

Without macro-substitutable types, the macro can only emit
accessors that return the int carrier; the caller is responsible
for adding a typed-let binding or `::` ascription on every use to
recover the component type.

## Minimal repro

```turmeric
(defstruct Pos [x : int])

(defmacro mk-getter [WName TName]
  `(defn get-x [w : ~WName] : ~TName w))   ;; <-- fails to expand

(mk-getter Pos Pos)
```

Diagnostic:

```
test.tur:4:21: error: unsupported type expression form (expected symbol, keyword, or list)
3 | (defmacro mk-getter [WName TName]
4 |   `(defn get-x [w : ~WName] : ~TName w))
  |                     ^^^^^^
```

Symbol synthesis via `str->sym` (gap A) and macro-emitted multiple
top-level forms via top-level-`do` splice (gap E) both work and are
necessary for this use case; this is the third missing piece.

## Observed vs. expected

Observed: the type-position parser is reached with the unquote form
intact and reports the form as not matching a type expression.

Expected: quasi-quote expansion runs before any type-position
parsing, so by the time `parse_type_form` sees the form, all
`~Symbol` markers have already been resolved to plain symbols.

## Root-cause pointer

`src/compiler/elab_types.c::parse_type_form` (around line 304 where
`form->tag == F_SYM` is checked) returns the "unsupported type
expression form" diagnostic when it sees an `F_UNQUOTE` (or whatever
the tag for `~Symbol` is). Either:

1. The macro expander needs to run on type-position forms before
   type parsing -- the obvious correct fix, but it requires
   threading the expander through the binding-list parser used by
   `defn`.

2. `parse_type_form` could special-case `F_UNQUOTE` by calling the
   expander recursively on its body. Smaller blast radius; only
   touches the type parser.

3. The defn-form parser could expand its parameter list and return
   type with the quasi-quote walker before handing them to
   `parse_type_form`. Most local fix; matches the conceptual model
   that quasi-quote always runs first.

(3) is the smallest move that gets the macro shape working. (1) is
the principled fix if there are other places that hit the same
shape (e.g., struct field types in macro-generated `defstruct`).

## What this blocks downstream

- **ECS `defworld` per-component accessors.** Currently blocked.
  With gap A + gap E in, all the *structural* generation works
  (multiple top-level emits, identifier synthesis); only the type
  annotations on each generated defn can't be parameterized over
  the component type.

- **Any macro that builds a typed wrapper around a polymorphic
  operation.** A `(deftyped-wrapper Pos dense-get)` shape, a
  `(defcomponent-pair Pos Vel ...)` shape, anything that wants to
  emit `(defn name [x : T] : T ...)` keyed on a macro arg `T`.

## Workaround in the ECS spice

`ecs/world.tur`'s `defworld` is reverted to the E0 shape (defstruct
emit only). The plan's per-component named accessors stay deferred.
Users keep calling the polymorphic `dense-get` / `dense-set!` /
`dense-has?` with the storage field directly: `(dense-get (.Pos w)
e)`. Typed reads use a typed-let binding: `(let [p : Pos (dense-get
(.Pos w) e)] ...)`.

## Proposed fix directions

(See "Root-cause pointer" above for the three locations.) (3) is the
smallest move that unblocks the ECS use case; (1) is the cleanest
long-term fix. A user-side workaround like building the defn form
via `(list defn nm ...)` instead of backquote does not work either
-- the assembled list still has the symbol in type position and
parses identically; the bug is in the type parser's handling of the
form, not in the quasi-quote walker.

## Validation plan

A fix is validated when:

- The minimal repro above expands, compiles, and runs.
- ECS `defworld` can re-add the per-component accessor emit,
  producing typed `get-Pos` / `set-Pos!` / `has-Pos?` defns.
- Existing fixtures that use macro-generated defns with literal
  type annotations continue to emit byte-identical C.

## Interaction with gaps A and E

This is the third in a series:

| Gap | Surface | Status |
|---|---|---|
| **A** `str->sym` | macros can compute fresh identifier names | shipped |
| **E** top-level-`do` splice | macros can emit multiple top-level forms | shipped |
| **this** unquote in type position | macros can put computed types into the emit | OPEN |

A, E, and this gap together gate the "natural macro-generated typed
defn family" shape. With A and E in, multi-defn emission with
*untyped* signatures works; with this third gap closed, the typing
falls into place too.

The ECS prerequisite plan
([`ecs-prereq-plan.md`](ecs-prereq-plan.md))
should add this as a sixth open gap (gap G after F).
