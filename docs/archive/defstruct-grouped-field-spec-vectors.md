---
title: defstruct rejects grouped `[name : type]` field-spec sub-vectors, blocking the variadic `defworld` collapse
severity: medium -- ergonomics/expressiveness gap. It is the remaining blocker
  for collapsing the ECS `defworld--0..5` per-arity cascade into one
  variadic-over-components macro, now that the `~@`-splice / compile-time `map`
  blocker is fixed (see docs/archive/ct-macro-evaluator-no-function-call-in-splice.md).
status: RESOLVED 2026-06-17
discovered: 2026-06-17
surfaced-by: validating the `defworld` collapse after the CT-macro `~@`-splice
  fix landed -- `map` over the component list now works, but its grouped output
  cannot be spliced into a `defstruct` field list.
---

> **RESOLVED 2026-06-17.** Fix direction (1) implemented in
> `src/compiler/elab_structs.c`: before the old-style field vector is scanned,
> a flattening pre-pass expands every top-level `F_VEC` element (a grouped
> `[name : type]` spec) into the surrounding `name`, `: type` token stream, so
> it is exactly equivalent to writing the field inline. Types are never bare
> vectors (keyword/symbol/list, wrapped in `F_TYPE_ANN`), so a top-level
> `F_VEC` element is unambiguously a grouped spec. The pre-pass is a no-op when
> no grouped specs are present.
>
> Both the direct spelling
> `(defstruct World [gens : int [pos : Dense] [vel : Dense]])` and the variadic
> `defworld` macro
> `` `(defstruct ~name [gens : int ~@(map (fn [c] `[~c : int]) comps)]) ``
> now compile and run. Regression fixture:
> `tests/fixtures/defstruct-grouped-field-specs/`. Full suite green
> (1667 passed, 0 failed).
>
> **Note:** chaining `(.count (.pos w))` through a *by-value struct* field
> (`pos : Dense`) still reads the inner field back through the int64 carrier --
> that is the separate, pre-existing straddle tracked in
> `docs/reported/defstruct-byvalue-struct-field-stored-as-int-carrier.md`, not
> a grouped-field issue. Grouped specs over scalar / opaque field types work
> end to end.

# defstruct rejects grouped `[name : type]` field-spec sub-vectors

## One-line summary

`defstruct` requires a **flat** field list (`[a : T b : U ...]`). It rejects a
field list that contains grouped `[name : type]` sub-vectors, which is exactly
the shape a `~@(map (fn [c] `[~c : (Dense ~c)]) comps)` splice produces. So even
with compile-time `map` + splice now working, a single variadic `defworld`
cannot build its field list by mapping over the component names.

## Minimal repro

```turmeric
(defstruct Dense [count : int])
;; Grouped sub-vectors in the field list:
(defstruct World [gens : int [pos : Dense] [vel : Dense]])
;; error: defstruct field list: expected field name symbol
```

Observed: `defstruct field list: expected field name symbol` pointing at the
first `[pos : Dense]` sub-vector.

Expected (proposed): a `[name : type]` sub-vector is accepted as one field,
equivalent to writing `pos : Dense` inline -- i.e. the field parser flattens a
one-level grouped spec. That makes the macro-generated shape legal:

```turmeric
(defmacro defworld [name comps]
  `(defstruct ~name [gens : int ~@(map (fn [c] `[~c : (Dense ~c)]) comps)]))
(defworld World (pos vel))
;; wants to become: (defstruct World [gens : int pos : (Dense pos) vel : (Dense vel)])
```

`map` returns one form per component, so a flat `name : type name : type ...`
list is not expressible via `map` alone -- each element is naturally the
grouped `[name : type]` pair. Accepting grouped sub-vectors is the natural fit.

## Why it matters

The CT-macro splice fix (now archived) was pursued specifically to collapse the
`defworld--0..5` cascade. With `map`/nested-macro splices working, this
`defstruct` field-shape constraint is the only remaining blocker for that
collapse. Until it is addressed, the per-arity cascade has to stay even though
the macro machinery can now generate the field list.

## Root cause (suspected)

The `defstruct` field-list parser (look for the "expected field name symbol"
diagnostic in the elaborator's defstruct path) walks the field vector expecting
a flat `name : type` token stream and treats an `F_VEC` element as an error
rather than recursing into it as a single grouped field spec.

## Proposed fix directions

1. In the `defstruct` field parser, when a field-list element is itself a
   vector of the form `[name : type]`, treat it as a single field (recurse /
   flatten one level) instead of erroring.
2. Alternatively, provide a compile-time splice that flattens a list of pairs
   into the flat token stream -- less natural than (1).

## Validation when fixed

- `(defstruct World [gens : int [pos : Dense] [vel : Dense]])` compiles and is
  equivalent to the flat spelling.
- The single variadic `defworld` macro above expands and the
  `defworld--0..5` cascade can be deleted.

## Cross-references

- `docs/archive/ct-macro-evaluator-no-function-call-in-splice.md` -- the
  CT-macro `~@`-splice / `map` / nested-macro blocker (RESOLVED); this is the
  remaining, independent blocker for the `defworld` collapse.
- `docs/archive/quasiquote-splice-into-vector-unsupported.md` -- `~@` into a
  vector literal (RESOLVED).
