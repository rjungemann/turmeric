---
title: Type Erasure to int64_t
category: Compiler Internals
description: Snapshot of where the tur compiler collapses higher-level types down to int64_t at the C boundary, and the three mechanisms it uses
---

# Type Erasure to `int64_t`

Turmeric compiles to C. Many high-level types -- closures, ADTs, opaque
structs, type variables, recursive types, tagged unions -- share one
runtime representation: a 64-bit opaque handle. This guide maps where
that collapse happens today, so contributors extending the type system
or codegen know which boundary they are crossing.

This is a snapshot. As sized types (SZ\*), unboxed structs, and
monomorphization land, several of these sites will gain non-erased
representations. Treat the file:line citations as a starting point and
re-verify before relying on them.

> **Status.** The aggregate carrier bridge, sized-primitive carriers, the
> by-value/by-pointer struct ABI, and typed function-pointer fields are all
> in place. Sized primitives narrow the carrier (`int32_t`, `uint8_t`, ...)
> but a generic slot is still `int64_t`. Nested aggregate fields are inlined
> only when the owning product itself flows by value -- see "Nested
> aggregates" below.

---

## The single choke point

Everything funnels through one function:

- **`type_c_name()`** at `src/compiler/types.c:3231` -- maps a `Type`
  to the C type name used in the emitted header and source. Simple
  payload-free kinds get their answer from the shared `TY_SIMPLE_REPR_ROWS`
  table (same file, ~line 484), which also feeds
  `type_has_concrete_codegen_layout` and `append_type_mangle` so the three
  switches cannot drift.

If `type_c_name()` returns `"int64_t"` for a type, that type is erased
at the C boundary. If it returns a struct name or a concrete C type,
it is not.

---

## The three mechanisms

### Pointer-as-int

Heap-allocated values are cast through `(int64_t)(intptr_t)` and stored
as raw integers. The runtime keeps a pointer; the type system pretends
it's an int.

- ADTs (`TY_ADT`) -- `src/compiler/types.c:3338` (by-value flat products
  are the exception: they lower to the real `tur_adt_<Name>` aggregate)
- Opaque structs and `defopaque` -- the `TY_STRUCT` row of
  `TY_SIMPLE_REPR_ROWS` in `src/compiler/types.c`
- Cons cells -- `stdlib/list.tur` (untyped `tnil` / `list-length` paths)
- Option values -- `stdlib/option.tur` (payload slot)

Call sites that marshal these into generic positions live in
`src/compiler/elab_call.c` (search `(int64_t)(intptr_t)`).

### Opaque-by-default

Anything the type checker cannot lower to a concrete C layout falls
through to `int64_t`. This is how parametric polymorphism is
implemented in the absence of monomorphization.

- Type variables (`TY_TYVAR`) -- the `TY_TYVAR` row of
  `TY_SIMPLE_REPR_ROWS`, `src/compiler/types.c`
- Type applications without concrete layout (`TY_APP`) --
  `src/compiler/types.c:3354`
- Recursive types (`TY_REC`, Fix-style) -- `src/compiler/types.c:3379`

Note the asymmetry inside `TY_APP`: if `type_has_concrete_codegen_layout()`
succeeds, the application gets a real struct via `register_struct_app()`.
Otherwise it collapses. This is the seam where HKT specialization can
hook in.

### Tagged pair

For runtime-discriminated values, both the tag and the payload are
`int64_t`:

```c
typedef struct { int64_t tag; int64_t val; } tur_tagged_t;
#define TUR_TAG(t, v)  ((tur_tagged_t){(int64_t)(t), (int64_t)(v)})
```

- Definition -- `src/compiler/emit_module.c:6654`
- Tag construction -- `src/compiler/emit_expr.c` (search `TUR_TAG(`, ~line 3806)

Used for `(A | B)` union types and the `any` top type.

---

## Function values

Function values are a special case because they need both a code
pointer and an environment, but the type system still wants to treat
them uniformly.

- `TY_FN` inside a struct field -- an `int64_t` fat handle for a boxed
  closure field; a concrete `cfnptr` field is the one un-erased case (it
  lowers to a real `R (*)(A...)` typedef).
- Fat closure struct -- `src/compiler/emit_expr.c` (~line 7850):
  ```c
  struct __env_N { int64_t __fn; <captures...> };
  ```
- Rank-2 polymorphic wrapper -- `src/compiler/emit_module.c:7966`:
  ```c
  typedef struct { void *env; int64_t (*fn)(void *, int64_t);
                   int64_t (*fn_cps)(void *, int64_t, struct DK *); } tur_poly_fn_t;
  ```
- Function pointers cast `(int64_t)(intptr_t)` when passed into poly
  helpers -- `src/compiler/emit_expr.c`.

The code pointer is erased into `int64_t`; the environment travels
alongside as a separate `void *`.

---

## What stays unboxed

Not everything collapses. Types that already fit in a register and
have a stable C representation pass through unchanged:

| Turmeric type | C type |
| --- | --- |
| `:int`        | `int64_t` (carrier, not erasure) |
| `:bool`       | `bool` |
| `:float`      | `double` |
| `:cstr`       | `const char *` |
| `:ptr`        | `void *` |
| Concrete `defstruct` | the struct's C name |
| Concrete `TY_APP` with codegen layout | a registered struct app |
| `TY_SET`      | `tur_set_t *` |

The distinction between "`:int` as a carrier" and "`:int` as erasure
target" matters when reading inline-C: a parameter declared `:int` may
be carrying a real integer, or it may be carrying a cast pointer. The
declaring `defn` is the source of truth.

---

## Nested aggregates

A struct *field* whose type is itself an aggregate -- another `defstruct`,
a `TY_APP` like `Option[int]`, or an ADT -- is inlined or carrier-erased
depending on the **owner**, not just the field type. The chokepoint is
`adt_ctor_field_c_type` (`src/compiler/emit_module.c`), which every
field-emission site routes through, consulting
`adt_field_is_inline_byval` (`src/compiler/types.c`):

- **By-value owner, drop-glue-free by-value field** -- the field is
  **flat-inlined**: `HasVec` containing a `Vec2` really holds a
  `tur_adt_Vec2 p;` member. This covers nested by-value ADT/struct
  products and concrete by-value `TY_APP` monomorph fields
  (`(Option cstr)` -> `tur_adt_Option__cstr`).
- **Everything else stays on the carrier**: a `:heap` field is a typed
  pointer; a field whose type owns an rc/ref (needs drop glue) is boxed so
  the owner stays trivially copyable; a *carrier* owner keeps every field
  as an `int64_t` slot holding a cast pointer to a heap-allocated
  aggregate; generic/parametric fields with no concrete layout stay
  erased.

The function-pointer field case is different again: a concrete `cfnptr`
field has a stable C signature and no allocation, so it lowers to a real
function-pointer typedef, while a boxed closure field is an `int64_t` fat
handle (see `type_c_name`'s `TY_FN` arm).

---

## Why this matters

A few practical consequences of the current erasure scheme:

1. **No type-directed dispatch at the C level.** Two functions that
   take an erased generic parameter receive the same `int64_t` and
   cannot branch on the runtime type without a `tur_tagged_t`-style
   discriminant.
2. **Pointer provenance is invisible to the C compiler.** GC and
   sanitizer tooling that wants to walk heap pointers has to know
   which `int64_t` fields are actually casts. The runtime tracks this
   separately.
3. **The HKT codegen seam is `type_has_concrete_codegen_layout()`.**
   Any improvement that turns more `TY_APP` cases into real structs
   passes through this predicate.
4. **Sized types (SZ\*) narrow the carrier, not the erasure.** A
   `:int32` argument still lives in an int-sized slot at the call
   boundary; the narrowing happens inside the function body.

---

## Re-verifying the map

Line numbers drift. To regenerate this list:

```sh
# All sites that emit "int64_t" as a C type from the type lowering
rg -n '"int64_t"' src/compiler/types.c

# Sites that cast pointers through intptr_t into the int64_t carrier
rg -n '\(int64_t\)\(intptr_t\)' src/ stdlib/

# The tagged union and poly function typedefs
rg -n 'tur_tagged_t|tur_poly_fn_t' src/compiler/emit_module.c
```

The choke point at `type_c_name()` is stable -- start there and walk
outward.
