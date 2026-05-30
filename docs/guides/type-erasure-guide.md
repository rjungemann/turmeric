---
title: Type Erasure to int64_t
category: Internals
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

> **Status: post-Theme B/C/D (2026-05-30).** The aggregate carrier bridge
> (Theme B), sized-primitive carriers (Theme C), and the by-value/by-pointer
> struct ABI plus typed function-pointer fields (Theme D1/D2) have landed.
> Sized primitives narrow the carrier (`int32_t`, `uint8_t`, ...) but a
> generic slot is still `int64_t`. The one remaining erasure that the
> unboxing roadmap *deliberately keeps* is the nested-aggregate case --
> see "Nested aggregates" below (Theme D3).

---

## The single choke point

Everything funnels through one function:

- **`type_c_name()`** at `src/compiler/types.c:1560` -- maps a `Type`
  to the C type name used in the emitted header and source.

If `type_c_name()` returns `"int64_t"` for a type, that type is erased
at the C boundary. If it returns a struct name or a concrete C type,
it is not.

---

## The three mechanisms

### 1. Pointer-as-int

Heap-allocated values are cast through `(int64_t)(intptr_t)` and stored
as raw integers. The runtime keeps a pointer; the type system pretends
it's an int.

- ADTs (`TY_ADT`) -- `src/compiler/types.c:1630-1631`
- Opaque structs and `defopaque` -- `src/compiler/types.c:1626-1627`
- Cons cells -- `stdlib/list.tur:18, 33, 56, 68`
- Option values -- `stdlib/option.tur:30-35`

Call sites that marshal these into generic positions live in
`src/compiler/elab_call.c:1570-1576`.

### 2. Opaque-by-default

Anything the type checker cannot lower to a concrete C layout falls
through to `int64_t`. This is how parametric polymorphism is
implemented in the absence of monomorphization.

- Type variables (`TY_TYVAR`) -- `src/compiler/types.c:1632-1634`
- Type applications without concrete layout (`TY_APP`) --
  `src/compiler/types.c:1638-1643`
- Recursive types (`TY_REC`, Fix-style) -- `src/compiler/types.c:1645-1646`

Note the asymmetry inside `TY_APP`: if `type_has_concrete_codegen_layout()`
succeeds, the application gets a real struct via `register_struct_app()`.
Otherwise it collapses. This is the seam where HKT specialization can
hook in.

### 3. Tagged pair

For runtime-discriminated values, both the tag and the payload are
`int64_t`:

```c
typedef struct { int64_t tag; int64_t val; } tur_tagged_t;
#define TUR_TAG(t, v)  ((tur_tagged_t){(int64_t)(t), (int64_t)(v)})
```

- Definition -- `src/compiler/emit_module.c:1324-1327`
- Tag construction -- `src/compiler/emit_expr.c:725`

Used for `(A | B)` union types and the `any` top type.

---

## Function values

Function values are a special case because they need both a code
pointer and an environment, but the type system still wants to treat
them uniformly.

- `TY_FN` inside a struct field -- `src/compiler/types.c:473` --
  always `int64_t`.
- Fat closure struct -- `src/compiler/emit_expr.c:1715`:
  ```c
  struct __env_N { int64_t __fn; <captures...> };
  ```
- Rank-2 polymorphic wrapper -- `src/compiler/emit_module.c:1314`:
  ```c
  typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;
  ```
- Function pointers cast `(int64_t)(intptr_t)` when passed into poly
  helpers -- `src/compiler/emit_expr.c:1169-1243`.

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

## Nested aggregates (Theme D3 decision)

A struct *field* whose type is itself an aggregate -- another `defstruct`,
a `TY_APP` like `Option[int]`, or an ADT -- is **carrier-erased to
`int64_t`, not flat-embedded**. This is verifiable today:

```turmeric
(defstruct Vec2 [x :int y :int])
(defstruct HasVec [p :Vec2])           ;; field p
(defstruct HasOpt [o : (Option int)])  ;; field o
```

lowers (via `struct_field_c_type`, `src/compiler/types.c`) to:

```c
typedef struct HasVec { int64_t p; } HasVec;   /* not Vec2 p */
typedef struct HasOpt { int64_t o; } HasOpt;   /* not a flat Option */
```

The `default:` arm of `struct_field_c_type`'s field-kind switch returns
`int64_t` for `TY_STRUCT` / `TY_APP` / `TY_ADT` fields; the field holds a
cast pointer to a heap-allocated aggregate.

**Decision: keep nested aggregates carrier-erased for now.** Flat-inlining
a concrete nested struct (so `HasVec` literally contains a `Vec2`) is a
*future optimization*, not a correctness requirement, and it would have to:

1. gate on `type_has_concrete_codegen_layout()` for the field type (the
   same seam the HKT work uses), so generic/parametric fields stay erased;
2. agree with the Theme D1 `pass_by_ptr` decision -- an inlined wide field
   pushes the containing struct past the 16-byte by-pointer threshold and
   must flip the *container* to by-pointer too;
3. update every `make-struct`, field-read (`.f s`), and field-write site
   to stop round-tripping through `(int64_t)(intptr_t)`.

The function-pointer field case (Theme D2) is the one nested case that
*was* un-erased, because a concrete `fn` type has a stable C signature and
no allocation; see `struct_field_c_type`'s `TY_FN` branch. Aggregates do
not share that property, so they stay on the carrier ABI until the
optimization above is scheduled on its own merits.

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
