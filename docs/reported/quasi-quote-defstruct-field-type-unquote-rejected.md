---
title: Quasi-quote unquote in defstruct field-type slot rejected; eager field-type parse blocks list-valued unquotes
category: Macro / defstruct interaction
severity: Macro-system ergonomics gap. A macro that wants to emit `(defstruct Name [name : (TyCtor TyArg1 TyArg2)])` where `TyArg1` is unquoted from the macro's bound vars cannot use the colon-separated field-list form; the defstruct field-type parser appears to read the type expression at template-parse time, before unquote substitution. Workarounds exist (paired-form `(name type)` field list, polymorphic type-param shape) but the colon form is the more common surface and the failure mode is obscure.
status: OPEN. Surfaced 2026-06-14 while wiring E2c slice 3 (sized-defworld macro).
---

# Quasi-quote / defstruct field-type interaction

## Summary

A `defmacro` body that emits a `defstruct` via quasi-quote cannot
splice a list-valued type into a `[name : type]` colon-form field
slot. The defstruct field-type parser rejects the unquote at
template parse time -- before the unquoted form has a value to
splice. The error is `error: unsupported type expression form
(expected symbol, keyword, or list)`, and points at the unquote
syntax inside the template.

The same defstruct, written by hand with the unquoted form
substituted literally, compiles fine. So the issue is specifically
the interaction between quasi-quote / defstruct, not the underlying
type expression.

## Minimal repro

```turmeric
(defmacro emit-w [name cap T]
  `(defstruct ~name
     [field : (SizedDense ~cap ~T)
      live  : int]))

(emit-w GameWorld (Static 64) Pos)
```

Compiles to:

```
error: unsupported type expression form (expected symbol, keyword, or list)
   [field : (SizedDense ~cap ~T)
                            ^^^
```

The carat points at the second unquote (`~T`). The first (`~cap`),
in the size-index slot, parses; the second, in the element-type
slot, does not.

Same defstruct hand-written compiles:

```turmeric
(defstruct GameWorld
  [field : (SizedDense (Static 64) Pos)
   live  : int])
```

## Workarounds discovered while wiring E2c slice 3

1. **Paired-form field list** -- `(defstruct Name (field type) ...)`,
   the form used by `stdlib/pair.tur`. The paired form's type slot
   accepts unquoted lists. But: this form requires the struct to
   have at least one type parameter, so the `[type-params]` vector
   cannot be omitted. (Hand-written `(defstruct Pair [A B] (fst A)
   (snd B))` works; the corresponding parameterless form
   `(defstruct GW [] (Pos ...))` fails with `defstruct field list
   cannot be empty`.)

2. **Polymorphic type-param shape** -- declaring the struct
   parametric over the size index, e.g. `(defstruct GameWorld [n]
   (Pos (SizedDense n Pos)) ...)`, sidesteps the literal-cap-in-
   field-type case entirely. Callers materialise via type
   ascription: `(let [w : (GameWorld (Static 64)) (make-gw 64)] ...)`.
   This is what slice 3 ships -- the polymorphic surface was
   already the cleaner library shape -- but if a user really wants
   a monomorphic alias they have to write a `deftype` manually.

3. **Helper macros that return value-position forms** -- do NOT
   work. `defmacro` is a source-substitution rewriter; calling a
   helper macro from inside another macro's `let` body returns the
   unexpanded source form, not a value. (E.g.
   `(let [fields (__sdw-fields comps)] ...)` binds `fields` to the
   verbatim invocation, not to the expanded list.)

## Suspected root cause

The defstruct macro elaborator appears to parse the field-list at
the same syntactic phase that quasi-quote unquotes get processed.
Specifically:

- `(SizedDense ~cap ~T)` is read as a type expression.
- The type-expression reader walks the head (`SizedDense`), then
  iterates its type-argument slots.
- The size-index slot accepts `~cap` (perhaps because Size
  literals are read leniently).
- The element-type slot has a stricter set of allowed tokens
  (symbol / keyword / list) and rejects the unquote token.

If the reader processed the entire field list as a list-of-symbols
first and only ran the type-expression parser AFTER unquote
substitution, this would not fail.

## Proposed fix direction

Defer the defstruct field-type parse until after the host macro's
quasi-quote has expanded. Concretely: have `defstruct`'s field
list reader retain unquote tokens as opaque syntax until the
enclosing macro's expansion runs, then re-elaborate the field
type expression on the expanded form.

A weaker fix: accept the paired-form `(name type)` field list in
the parameterless case (`(defstruct Name (field type))`), which
would unblock workaround 1.

## Validation of a fix

A direct in-tree fixture:

```turmeric
(defmacro emit-w [name cap T]
  `(defstruct ~name [f : (SizedDense ~cap ~T) live : int]))

(emit-w W (Static 4) int)

(defn main [] : int 0)
```

should compile. Today it does not.

For the spice-side: drop the polymorphic-over-`n` workaround in
`spices/ecs/src/ecs/sized-world.tur` (in
`../turmeric-spices`, branch `e2c/sized-dense`) and ship the
monomorphic `(sized-defworld Name (Static N) [Comps])` form the
design plan originally specified.

## Related

- E2c slice 3 commit on branch `e2c/sized-dense` in
  `../turmeric-spices`.
- Design plan: `docs/upcoming/ecs-sized-world-plan.md` (sets
  surface that this gap forced a partial re-spec on).
- The other bug filed in the same session:
  `docs/reported/cross-module-generic-of-generic-instantiation-missing.md`.
