---
title: Struct Ergonomics Plan
category: Planning
description: Auto-bound struct constructors, keyword-argument construction for both `make-struct` and the new constructor, and a `with` macro for functional field-update of `:copy` structs.
---

# Struct Ergonomics -- Plan

## Context

Constructing and updating structs today is more verbose than it needs to
be. Every callsite spells `(make-struct Person "Bob" 40)`, and there is
no functional-update form: copying a struct with one field changed forces
the user to either re-list every field by hand or thread mutation through
a `:copy` ownership dance.

Three small ergonomic features close the gap without touching IR or
codegen:

1. **Auto-bound constructor.** `defstruct Person ...` also binds
   `Person` as a function such that `(Person "Bob" 40)` is equivalent
   to `(make-struct Person "Bob" 40)`. Struct names live in the type
   namespace; the auto-bound function lives in the value namespace, so
   there is no shadowing conflict.
2. **Keyword arguments for construction.** Both `make-struct` and the
   auto-bound constructor accept a keyword form
   `(Person :name "Bob" :age 40)`. Keyword order is free; missing
   required fields are a hard error; type-checking happens per
   keyword/value pair against the declared field type. Positional and
   keyword forms cannot be mixed in one call.
3. **`with` functional update.** `(with p [name "Alice"])` returns a
   new struct with the listed fields overridden and the rest copied
   from `p`. Only `:copy` structs are supported; non-`:copy` structs
   reject the form at elaboration with a clear message pointing at the
   ownership reason.

All three are macro / elaboration work. No new IR nodes, no new
codegen, no new runtime support.

## Goals

1. **CTOR-V0 -- auto-bound constructor.** Every `defstruct S ...` also
   produces a `defn S` (in the value namespace) that takes the field
   types positionally and returns an `S`. Existing `make-struct S ...`
   callsites continue to work unchanged.
2. **KW-V0 -- keyword construction.** `make-struct` and the auto-bound
   constructor both accept `:field value` pairs in place of positional
   args. Mixing positional and keyword in one call is an elaboration
   error (TUR-E0xxx). Order does not matter; every declared field
   must be supplied; an unknown keyword is an error; a duplicate
   keyword is an error.
3. **WITH-V0 -- `with` macro.** `(with src [f1 v1 f2 v2 ...])`
   expands to a constructor call that takes `f1`/`f2`/... from the
   override list and every other field from `src` via the existing
   field-accessor codegen. Only valid on `:copy` structs; an explicit
   error fires on non-`:copy`.
4. **DOC-V0 -- guide + docstrings.** Update the struct guide with the
   new forms; add `;;;` docstrings to the new builtins/macros so they
   surface in generated docs and `(doc 'with)`.

Non-goals for this plan:

- Trailing-dot reader macro (`Person.` -> constructor). Deliberately
  skipped in favor of the auto-bound function form.
- Pattern-matching destructuring of structs. Out of scope; tracked
  separately if/when needed.
- `with` on non-`:copy` structs (linear ownership transfer with
  partial override). Deliberately rejected; revisit only if a real
  callsite needs it.

## Design

### CTOR-V0 -- auto-bound constructor

`defstruct` already lowers to a struct-type declaration plus a set of
field accessors. Extend the lowering to also emit:

```turmeric
;; given (defstruct Person :copy [name : cstr age : int])
(defn Person [name : cstr age : int] : Person
  (make-struct Person name age))
```

Implementation notes:

- The function lives in the value namespace; the type `Person` lives
  in the type namespace. Disambiguation is by *position* in the form
  (a type annotation slot vs. a call head), which is already how the
  elaborator distinguishes type and value occurrences elsewhere.
- For parametric structs (`(defstruct Lens [S A] ...)`), the
  auto-bound constructor is itself generic and elaborates the same
  way `make-struct Lens` does today. No new inference machinery.
- Arity check, type check, and currying behavior are identical to any
  other `defn`. `(Person "Bob")` returns a closure expecting `age`,
  which composes with the existing curry path.
- A `:no-auto-ctor` attribute on `defstruct` opts out (escape hatch
  for the rare case where the struct name is already used as a value
  binding elsewhere).

### KW-V0 -- keyword construction

KW-V0 is specified over **named-field constructors**, not "structs"
specifically. Today the only named-field constructors in the language
are struct constructors and (after CTOR-V0) auto-bound struct
constructors; if record-style ADT variants are added later, KW-V0's
machinery applies to them with no changes.

Extend the call-elaboration path for both `make-struct` and any
auto-bound constructor:

1. If the first non-type argument is a keyword (`:name`), switch to
   keyword mode.
2. Walk the remaining arguments in pairs (`:keyword value` ...).
3. Resolve each keyword against the struct's declared field set;
   unknown keyword -> error; duplicate keyword -> error.
4. After the walk, every declared field must have a value;
   missing field -> error listing the missing names.
5. Reorder the values into declared-field order and emit the same
   construction node the positional path produces.

Mixing positional and keyword in one call is rejected with a
diagnostic that suggests picking one form.

Diagnostic codes to reserve:

- `TUR-E0xxx` -- unknown field keyword in struct construction
- `TUR-E0xxx` -- duplicate field keyword
- `TUR-E0xxx` -- missing required field(s)
- `TUR-E0xxx` -- mixed positional and keyword arguments

Type-checking each keyword/value pair uses the same field-type lookup
the positional path uses; the reordering happens before the type
check fires, so error messages reference the declared field name, not
a positional slot.

### WITH-V0 -- `with` macro

WITH-V0 is specified over **values of a single-variant named-field
type with a `:copy` payload**. Today that means `:copy` structs. If
record-style ADT variants and narrowing-after-`match` are added later,
WITH-V0 naturally extends to "narrowed ADT values whose variant
payload is `:copy`," with no change to the macro's lowering -- the
narrowing context provides the same "single variant statically known"
guarantee the struct case has by construction.

Surface syntax:

```turmeric
(with src [name "Alice"])
(with src [name "Alice" age 41])
```

Sweet-exp:

```turmeric
with src [name "Alice"]
with src
  [name "Alice"
   age  41]
```

Lowering (assume `Person` has fields `name : cstr age : int`, both
declared on a `:copy` struct):

```turmeric
(with p [name "Alice"])
;; -->
(Person "Alice" (. p age))     ; auto-bound constructor + field access
```

With the keyword form available (KW-V0), the macro can equivalently
lower to:

```turmeric
(Person :name "Alice" :age (. p age))
```

Pick the keyword lowering -- it is robust to field-order changes in
the `defstruct` and produces clearer error messages if a field name
is wrong.

Elaboration rules:

- `src` must have a struct type with the `:copy` attribute. If it does
  not, fire a dedicated diagnostic explaining why (ownership of the
  source would otherwise be consumed) and pointing at `defstruct ...
  :copy` as the fix.
- Every override field must exist on the struct type; unknown ->
  error.
- Duplicate override field -> error.
- Override value type must unify with the declared field type;
  reuses the existing field-type-check machinery.
- The macro generates a fresh constructor call per `with` site;
  field accessors on `src` are inlined for the non-overridden fields.

Out of scope for WITH-V0:

- Nested-field update (`(with p [address.city "NYC"])`). Revisit if a
  real callsite asks for it.
- Conditional update (`(with p [name (if cond ...)])`). The override
  *value* is an ordinary expression, so this works for free; only
  conditional *presence* of an override is out of scope.

### Convergence with ADTs (deferred)

A struct is, in type-theory terms, a single-variant ADT with named
fields. The three features in this plan are intentionally aligned
with that view:

- **CTOR-V0** auto-binds a constructor function whose shape is
  identical to what ADT variant constructors (`Some`, `Cons`) already
  have. The plan is making the two surfaces consistent, not
  introducing a struct-only mechanism.
- **KW-V0** is specified over named-field constructors, so it lifts
  unchanged to record-style ADT variants if/when those land.
- **WITH-V0** is specified over single-variant-known contexts with a
  `:copy` payload, so it lifts unchanged to narrowed ADT values.

The deeper unification -- treating `defstruct S [...]` as sugar for
`defadt S (S [...])`, sharing codegen, letting `match` accept struct
values -- is intentionally **out of scope here** and tracked
separately in
[`docs/upcoming/v1/struct-adt-convergence-plan.md`](v1/struct-adt-convergence-plan.md).
The features in this plan do not foreclose that direction; they
prefigure it.

### Interaction with the `(. l get p)` issue

`with` produces field reads via `(. src field)`. The companion bug
[`dot-method-call-misroutes-to-typeclass.md`](../reported/dot-method-call-misroutes-to-typeclass.md)
notes that `(. obj field args...)` currently misroutes; the
single-arg `(. obj field)` form should be unaffected, but verify
this with a fixture before relying on it for WITH-V0 lowering. If
needed, lower to the explicit accessor `(.field src)` instead of
`(. src field)`.

## Fixtures and tests

- `tests/fixtures/struct-auto-ctor/` -- `(Person "Bob" 40)` produces
  the same value as `(make-struct Person "Bob" 40)`; curried
  application works; `:no-auto-ctor` opts out.
- `tests/fixtures/struct-kw-ctor/` -- keyword construction works
  with `make-struct` and with the auto-bound constructor; ordering
  is free; missing field, unknown field, duplicate field, and
  mixed-form errors all fire with the right code.
- `tests/fixtures/struct-with-copy/` -- `with` on a `:copy` struct
  produces a new value with overrides applied; multi-field override
  works; field-order in the override list is free.
- `tests/fixtures/struct-with-non-copy-rejects/` -- `with` on a
  non-`:copy` struct fails with the dedicated diagnostic.
- `tests/fixtures/struct-with-type-error/` -- override value of the
  wrong type fails with the same diagnostic the positional
  constructor would have produced.

## Risks and open questions

- **Namespace collision.** If a user has already defined `Person` as
  a value before declaring `(defstruct Person ...)`, the auto-bound
  constructor will shadow it. Mitigation: `:no-auto-ctor` opt-out and
  a warning when the constructor overwrites an existing binding.
- **Parametric struct ergonomics.** `(Lens name-get name-put)` needs
  to infer `S` and `A` from the argument function types. This
  already works for `make-struct Lens ...`; verify the auto-bound
  constructor inherits the same inference path with no special
  handling.
- **Keyword form and currying.** Partial application of a keyword
  constructor is ambiguous (no positional "next slot"). Disallow
  currying when the keyword form is used; require all fields in one
  call. Positional form retains its existing curry behavior.
- **Diagnostic budget.** Four new diagnostic codes for KW-V0 plus one
  for WITH-V0's `:copy` rejection. Reserve them in the diagnostic
  registry before implementation.

## Order of work

1. CTOR-V0 -- auto-bound constructor + fixtures. Lowest risk.
2. KW-V0 -- keyword construction. Independent of CTOR-V0 but pairs
   nicely; land after CTOR-V0 so fixtures can exercise both surfaces.
3. WITH-V0 -- `with` macro built on KW-V0's lowering.
4. DOC-V0 -- guide page + docstrings + generated docs regen.

Each step ships with its own fixtures; the test suite (`bash
tests/run.sh`, timeout 600000) gates each landing.
