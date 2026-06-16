---
title: M2b -- make-struct / default-of body form for #{Construct}
category: Planning -- ABI / Codegen rework
description: Design for M2b of the end-to-end monomorphization plan -- replace the implicit shape-inference path that M2a uses with explicit (make-struct ...) / (default-of T) body forms. Lets #{Construct} cover constructors that the M2a inference can't reach (Option's some/none with one payload slot; Cons / Pair where the constructor name doesn't match the discriminator's word root; vec-of which isn't a tagged-union constructor at all).
status: shipped (verified 2026-06-15)
---

## Resolution (2026-06-15)

`make-struct` and `default-of` are live across the targeted stdlib types:

- `stdlib/result.tur:39-65` -- both `ok` and `err` use `(make-struct Result ... :err-val (default-of B))` etc.
- `stdlib/option.tur:30, 46-48` -- `some` / `none` use `(make-struct Option ... :value (default-of A))`.
- `stdlib/pair.tur:28` -- `pair` is `(make-struct Pair :fst a :snd b)`.
- `stdlib/list.tur:55` -- `tcons-of` is `(make-struct Cons :head h :tail t)`.

`vec-of` is intentionally out of scope (see `stdlib/vec.tur:32` note). The
plan's stated goals -- covering `Option`, `Cons`, `Pair` -- are met.

# M2b -- make-struct / default-of body form for `#{Construct}`

## Why this exists (and why M2a doesn't suffice)

M2a generalized the Prereq-6 synthesized body from a by-name match on
`ok` / `err` to a `#{Construct}`-driven shape inference. The inference
works for `Result`-shaped tagged unions where:

- the defstruct has exactly one `:bool` discriminator field,
- it has at least two payload fields, each typed by a distinct type
  parameter,
- the constructor has exactly one payload arg whose type is the same
  tyvar as one of the payload fields, and
- there are exactly as many sibling constructors as payload fields,
  one per slot.

That covers `Result` and would cover any other `Either`-like type.
**It does not cover**:

- `Option` -- one payload field `value : A`. M2a's "find by type-param
  position" rule still works, but `none` has zero payload args, so its
  body can't be inferred -- it sets `is-some = false` and that's it.
  Doable, but the inference rule for zero-arg "absence" constructors
  needs to be explicit, not implicit.
- `Cons / Pair` -- the constructor's name (`cons`, `pair`) doesn't
  share a word root with the discriminator field (there isn't one; both
  are products, not sums). The "tag value derives from name-vs-disc"
  heuristic doesn't apply. We'd need an out-of-band signal that says
  "this is the only constructor; no discriminator to set."
- `vec-of` -- not a tagged-union constructor at all. Constructs a
  growable array; the body builds a header + capacity + payload. No
  inference rule could possibly cover it.

Hybrid types (multiple bool fields, payload fields that share a
tyvar, defstructs whose constructors don't line up 1:1 with payload
slots) also fall outside the inference. Even a single counterexample
in stdlib forces an escape hatch.

The escape hatch is a real body form. Instead of inferring shape,
the constructor *says what it builds*.

## Surface syntax

```turmeric
(defn ok [A B] [x : A]
  #{Construct}
  : (Result A B)
  (make-struct Result
    :is-ok true
    :ok-val x
    :err-val (default-of B)))

(defn err [A B] [e : B]
  #{Construct}
  : (Result A B)
  (make-struct Result
    :is-ok false
    :ok-val (default-of A)
    :err-val e))

(defn some [A] [x : A]
  #{Construct}
  : (Option A)
  (make-struct Option :is-some true :value x))

(defn none [A]
  #{Construct}
  : (Option A)
  (make-struct Option :is-some false :value (default-of A)))

(defn pair [A B] [a : A b : B]
  #{Construct}
  : (Pair A B)
  (make-struct Pair :fst a :snd b))

(defn cons [A] [head : A tail : (Cons A)]
  #{Construct}
  : (Cons A)
  (make-struct Cons :head head :tail tail))
```

`vec-of` is not a `make-struct` shape -- it stays inline-C in M2b and
gets its own emission template in a later phase, after the audit has
quantified vector call sites under monomorphization.

## Form: `(make-struct StructName :field-key expr ...)`

- Head symbol `make-struct` is a new core form.
- First positional arg is an unevaluated symbol naming a defstruct.
  (Not a Turmeric expression yielding a struct constructor -- the
  symbol identifies the StructDef at elaborate time.)
- Remaining args are alternating `:keyword` field selectors and value
  expressions, one per defstruct field.

### Required and optional rules

- Every field of the defstruct must appear exactly once.
  `TUR-E0292: make-struct missing field <name>` if any field is
  omitted; `TUR-E0293: make-struct duplicate field <name>` if any is
  repeated.
- Field order in the form is irrelevant -- the elaborator reorders to
  match the StructDef.
- Each value expression is type-checked against the field's declared
  type after substitution from the result type's type-parameter list.
- Unknown field key: `TUR-E0294: make-struct unknown field <name> for
  struct <Type>`.

### Type checking

At elaborate-time:

1. Resolve `StructName` -> `StructDef`.
2. Read the expected result type from the surrounding annotation
   (`: (Result A B)`). If unascribed, infer from the call site's
   context; if still ambiguous, error `TUR-E0295: make-struct cannot
   infer result type-parameters; ascribe the call site`.
3. Substitute the result type's args into each field's declared type.
4. Check each value expression against the substituted type.

### Lowering

Elaborated form `EX_MAKE_STRUCT` already exists in `emit_expr.c`
(`:3700-3731` in the M1 audit). The `#{Construct}` body path reuses
it; the lowering becomes:

```c
StructName__T1__T2 __r;
__r.field0 = <v0>;
__r.field1 = <v1>;
...
return __r;
```

When a field's declared type is a parametric `T` whose concrete instance
is a value-struct, the existing `struct_field_c_type` rule (commit
78589845) already lowers the slot to `T *`, and the existing
`EX_MAKE_STRUCT` emit (`:3700-3731`) heap-allocates the payload and
stores the pointer. M2b doesn't need to duplicate that logic -- it
relies on it.

## Form: `(default-of T)`

A new core form yielding a zero-valued `T`. Used to fill payload slots
the constructor doesn't have a meaningful value for (e.g. `err-val`
in `(ok x)`).

### Semantics

- `T` is a type expression. May reference type-parameters in scope.
- Result: a zero-bit-pattern `T`. For scalars, integer 0 / float 0.0 /
  bool false / cstr `""` / void pointer NULL. For structs, all fields
  zeroed recursively. For tagged sums, the first constructor's payload
  zeroed.
- Lowering: `(T){0}` in C, or `memset(&tmp, 0, sizeof(T))` for value
  structs.

### Why not just "leave the field unset"?

- M2a's prototype zeros every non-discriminator field via `memset` --
  that works because the slots are either scalars or pointer fields
  (`T *`) which are NULL-safe.
- M2b's `(default-of B)` makes the intent explicit and lets the
  elaborator verify that `B` is `default`-able. (For now: every type
  is, because the runtime is zero-initialized-safe; a future
  `:non-default` annotation could opt out.)
- It also matters for non-zero-default scalars in user-defined types
  if Turmeric ever ships a `Default` typeclass. M2b's `default-of`
  is the obvious lowering point.

### Risk

`(default-of T)` for a `T` that the caller never observes is wasted
work -- the slot will be overwritten or never read. The codegen
should elide the assignment when the field is the "dead" slot of a
sum-type constructor (current M2a behavior: zero it; M2b: detect
`default-of T` as the value and elide). This is straightforward in
emit_expr.c.

## Elaborator changes

1. New cached symbols on `Elab`: `sym_make_struct`, `sym_default_of`.
2. In the form dispatcher (the big switch in `elab_forms.c`), route
   `make-struct` head to a new `elab_make_struct` and `default-of`
   to a new `elab_default_of`.
3. `elab_make_struct`:
   - Resolves the struct name to a `StructDef`.
   - Walks the keyword/value pairs into a permutation matching
     `def->fields[]`.
   - Returns `EX_MAKE_STRUCT` with the field-ordered value array.
4. `elab_default_of`:
   - Resolves the type expression.
   - Returns a new `EX_DEFAULT_OF` expression node.
5. Emit:
   - `EX_MAKE_STRUCT` already exists; no change there.
   - `EX_DEFAULT_OF` lowers to a zero-init based on the type's C
     layout (`(T){0}` for scalars / aggregates; `memset` for
     value structs of unknown layout at the call site).

## Migration plan from M2a

When M2b lands, the M2a inference path in `emit_fns.c:629-787` is
deleted. The flow becomes:

1. Elaborator sees `#{Construct}` and tags `b->is_construct_template`.
2. Elaborator sees `make-struct` / `default-of` body, builds normal
   `EX_MAKE_STRUCT` / `EX_DEFAULT_OF` nodes.
3. Codegen emits the body the same way it emits any
   `(make-struct ...)`. No synthesis branch needed.

The `#{Construct}` marker stops being load-bearing for codegen and
becomes pure documentation -- it still helps M9's audit cleanup (a
`#{Construct}` body that *isn't* `(make-struct ...)` is suspicious),
and reserves the marker for future audit / lint passes.

Inline-C bodies in stdlib for `ok` / `err` / `some` / `none` / `pair` /
`cons` get retired. The carrier-return path (typeclass-instance-method
dispatch context) is handled by the orthogonal monomorphization work
in M4, not by retaining inline-C bodies.

## Stdlib changes (full surface)

| Defn | Today | M2b form |
|---|---|---|
| `ok` | `tur_ok((int64_t)(intptr_t)x)` | `(make-struct Result :is-ok true :ok-val x :err-val (default-of B))` |
| `err` | `tur_err(...)` | `(make-struct Result :is-ok false :ok-val (default-of A) :err-val e)` |
| `some` | `tur_some(x)` | `(make-struct Option :is-some true :value x)` |
| `none` | `TUR_NONE` | `(make-struct Option :is-some false :value (default-of A))` |
| `pair` | (currently a let over fields) | `(make-struct Pair :fst a :snd b)` |
| `cons` (typed) | inline-C heap alloc | `(make-struct Cons :head head :tail tail)` |
| `vec-of` | inline-C | stays inline-C until vector-codegen phase |

`some` / `none` get re-declared polymorphic in the same change
(currently typed `:int`). That's a stdlib API change; every fixture
that uses them needs the ascription pattern that `ok` / `err` already
require (`(:: (some x) (Option int))`).

## Fixture impact

Per the M1 audit cost-curve read: 30-60 concrete element types across
the suite, 6-7 constructors gaining new C bodies. Naive fixture-snapshot
estimate: ~150-300 `expected.c` updates if every existing constructor
call produces different C. In practice many constructors are
monomorphized to the same C type and the snapshot churn is closer to
50-100. Regenerate per the project's snapshot rule
(`bash CLAUDE.md`'s "Fixture Snapshots" section). M2b ships its own
fixture for each constructor proving the emitted body is the direct
`make-struct` form, not a `tur_*` call.

## What this design doesn't address

- **Vector constructors (`vec-of`, `vec-with-capacity`, etc.)** --
  these need an emission template that knows the vec header layout,
  not a make-struct lowering. Out of scope for M2b; tracked as a
  follow-up after M5 lands and the constraint-polymorphic codegen
  stabilizes.
- **HKT-class method bodies** -- `fmap` for Option/Result returns a
  freshly-constructed sum via `tur_some` / `tur_ok`. After M2b those
  bodies can switch to `make-struct` too, but the *dispatch* still
  goes through the typeclass dict; that's M4 territory, not M2b's.
- **Existential payloads** -- pack/open keeps the int64 carrier.
  `(default-of T)` for an existential type means "an existential
  packing the default of T," which is more semantic surface area than
  M2b wants to take on. Defer to M8.

## Validation harness

Per phase:

1. `bash tests/run.sh` -- zero `FAIL` regressions.
2. New fixture `tests/fixtures/make-struct-result/` -- asserts the
   emitted C for `(ok 42)` matches a direct `Result__int__cstr{.is_ok
   = true, .ok_val = 42, .err_val = 0}` initializer.
3. New fixture `tests/fixtures/default-of-zeroes/` -- asserts
   `(default-of int)` lowers to `0`, `(default-of (Vec int))` lowers
   to the zero-initialized struct.
4. Spice-side: rerun `../turmeric-spices/spices/json` (uses `ok` /
   `err` heavily); confirm decoder output identical to pre-M2b.

## Estimated cost

1-2 sessions for the elaborator changes + emit; +1 session for
fixture regen and stdlib rewrite. Total ~3 sessions -- larger than
M2a, smaller than M4/M7. Schedule after M3 (accessor cleanup) so the
deleted carrier-bridge path doesn't survive into the M2b fixture
regen.
