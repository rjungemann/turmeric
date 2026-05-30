---
title: Sized Types -- Type-Level Size Index Specification (SZ6)
category: Language Features
description: The representation and refinement rule for type-level size indices on sized GADTs -- how a length-n container's type mentions n, how the index threads through match via the SkolemEnv, and how it is erased in codegen
---

# Sized Types -- Type-Level Size Index Specification (SZ6)

This document specifies the type-level size index introduced in phase SZ6 of
the [sized-types completion plan](sized-types-completion-plan.md). It is the
companion reference for the compiler representation (`SizeTerm` in
`src/compiler/types.h`) and the refinement rule used during `match`.

## 1. The indexed shape

A sized GADT carries a **size index** as one of its type parameters. The index
is a type-level natural number built over the `Size` GADT
(`Static`/`Add`/`Mul`). The canonical example is the length-indexed vector:

```turmeric
(defgadt SizedVec [n]
  (SVNil  : (SizedVec (Static 0)))
  (SVCons int (SizedVec n) : (SizedVec (Add (Static 1) n))))
```

Read the constructor signatures as refinement rules:

- `SVNil : (SizedVec (Static 0))` -- the empty vector has length `0`.
- `SVCons : int -> (SizedVec n) -> (SizedVec (Add (Static 1) n))` -- consing one
  element onto a length-`n` vector yields a length-`(1 + n)` vector.

This replaces the SZ0 **phantom** encoding, where both constructors returned the
same `(SizedVec Size int)` and the type system could not tell a length-3 vector
from a length-4 one.

## 2. Representation: `SizeTerm`

A size index is represented by an arena-allocated `SizeTerm`
(`src/compiler/types.h`):

```c
typedef enum SizeTermKind {
    SZT_CONST,  /* (Static n) -- a literal natural number      */
    SZT_VAR,    /* a size variable, e.g. n                     */
    SZT_ADD,    /* (Add a b)                                   */
    SZT_MUL,    /* (Mul a b)                                   */
} SizeTermKind;

typedef struct SizeTerm {
    SizeTermKind kind;
    int64_t      konst;          /* SZT_CONST          */
    const char  *var;            /* SZT_VAR            */
    struct SizeTerm *lhs, *rhs;  /* SZT_ADD / SZT_MUL  */
} SizeTerm;
```

`size_term_from_form` parses a type-position `Form` into a `SizeTerm`:
`(Static n)` and bare integer literals become `SZT_CONST`; `(Add a b)` and
`(Mul a b)` recurse into `SZT_ADD`/`SZT_MUL`; a bare symbol becomes an
`SZT_VAR`. Anything else (an ADT application like `(Foo int)`) returns `NULL`
and is left to the ordinary type machinery, so size parsing never hijacks a
non-size type argument.

The companion helpers are:

- `size_term_eval(t, &out)` -- fold a **closed** term (no variables) to its
  integer value; returns `false` if any variable remains (the size is not
  statically known).
- `size_term_subst(a, t, var, repl)` -- substitute one size variable.
- `size_term_equal(a, b)` -- the SZ7 decision procedure: equal-by-value when
  both terms are closed, else a syntactic check modulo commutativity of
  `Add`/`Mul`.
- `size_term_to_string(t, buf, cap)` -- render for diagnostics, e.g. `(+ 1 n)`.

## 3. Refinement rule (threading the index through `match`)

Size indices reuse the existing GADT skolem machinery. When `elab_match`
enters an arm for a GADT constructor, it builds a per-arm `SkolemEnv` from the
constructor's *return-type* annotation by matching each return-type argument
against the GADT's declared type parameters
(`gadt_build_skolem_env`, `src/compiler/elab_structs.c`).

SZ6 adds one field to each `SkolemBinding`:

```c
typedef struct SkolemBinding {
    const char      *name;        /* type-parameter name, e.g. "n"        */
    TypeKind         kind;        /* concrete TypeKind (unchanged)        */
    struct Type     *full_type;   /* full Type for ADT/struct (unchanged) */
    struct SizeTerm *size_index;  /* SZ6: captured type-level size term   */
} SkolemBinding;
```

The capture is **additive**: when a return-type argument is structurally a size
expression (head `Static`/`Add`/`Mul`), its `SizeTerm` is parsed and stored on
the binding's `size_index`; `kind`/`full_type` are left exactly as before, so
no existing field-type resolution changes.

Worked example -- matching `SVCons` against a scrutinee of type
`(SizedVec s)`:

1. The constructor's return type is `(SizedVec (Add (Static 1) n))`. Parameter
   `n` is bound with `size_index = (+ 1 n)` (an open `SZT_ADD` whose right
   operand is the size variable `n`).
2. The field `xs` is declared `(SizedVec n)`. Resolving it through the arm's
   `SkolemEnv` types the tail at index `n` -- the same variable that appears in
   the scrutinee's `(Add (Static 1) n)`.
3. The scrutinee is therefore refined to index `(Add (Static 1) n)` while the
   tail is refined to `n`: the equation `s = 1 + n` holds within the arm, which
   is exactly the GADT skolem equality, now carrying a size term.

Phase SZ7 consumes these captured terms: where two indices are both closed,
`size_term_equal` decides equality at compile time; otherwise the check falls
back to the existing runtime assertion (SZ7.3).

## 4. Erasure in codegen

Size indices are **compile-time only**. They never reach codegen: the emitted
C for an indexed `SizedVec` is byte-for-byte the flat tagged-union it was under
the phantom encoding. For

```turmeric
(defgadt SizedVec [n]
  (SVNil : (SizedVec (Static 0)))
  (SVCons int (SizedVec n) : (SizedVec (Add (Static 1) n))))
```

the generated struct is:

```c
typedef struct tur_adt_SizedVec {
    int tag;
    union {
        struct { } SVNil;
        struct { int64_t _0; int64_t _1; } SVCons;  /* element + tail; no size field */
    };
} tur_adt_SizedVec;
```

`SVCons` stores only its element (`_0`) and tail pointer (`_1`); the index
`(Add (Static 1) n)` contributes nothing. The fixture
`tests/fixtures/sized-sz6-erasure` pins this erased shape with an `expected.c`
snapshot.

## 5. Static size checking (SZ7)

SZ7 turns the captured size terms into a compile-time check with its own
diagnostic, **TUR-E0260** (`tur explain TUR-E0260`).

The check is applied at the call site of the size assertions
`size-assert-eq!` and `size-assert-le!` (`sz7_static_size_violation` in
`src/compiler/elab_call.c`):

1. Each argument form is parsed with `size_term_from_form`, which now also
   recognises the value-level spellings `size-static` / `size-add` /
   `size-mul` in addition to the `Static` / `Add` / `Mul` constructors.
2. If **both** arguments fold to constants via `size_term_eval`, the relation
   is decided at compile time:
   - `size-assert-eq!` with unequal constants -> `TUR-E0260` (`size N is not M`).
   - `size-assert-le!` whose left exceeds its right -> `TUR-E0260`
     (`size N exceeds upper bound M`).
   A rejected program does not compile, so **no runtime check is emitted**.
3. If either argument is **not** statically known (a variable, a value read at
   run time, or a size threaded through a function parameter), the checker
   returns without deciding and the call elaborates normally -- the existing
   runtime assertion in `stdlib/sized.tur` guards it. The checker never
   silently accepts a possibly-wrong size (the SZ7.3 fallback boundary).

This deliberately uses syntactic constant folding, not an SMT solver:
non-trivial nonlinear or multi-variable equalities fall back to the runtime
check rather than being proven (see the non-goals in the completion plan).

## 6. Size inference and elision (SZ8)

So that users rarely annotate, the index of a sized-GADT constructor
application is **inferred** by substituting each operand's already-inferred
index into the constructor's declared return-index template
(`sz8_infer_ctor_size_index` in `src/compiler/elab_call.c`):

- A nullary constructor seeds the recursion from its constant template --
  `SVNil : (SizedVec (Static 0))` infers `0`.
- A recursive constructor substitutes the field's index variable for the
  argument's inferred index -- `SVCons : ... (SizedVec n) -> (SizedVec (Add
  (Static 1) n))` applied to an operand of index `t` infers `(Add (Static 1)
  t)`. Hence `(SVCons x (SVCons y SVNil))` infers `(SizedVec 2)`.

The inferred term is stored on the constructor-call `Expr` (`call_.size_index`)
and is **erased in codegen** like every other size index. The `--dump-sizes`
flag prints one line per sized-GADT constructor application during
elaboration, e.g.:

```
size: SVNil  : (SizedVec 0)
size: SVCons : (SizedVec 1)
size: SVCons : (SizedVec 2)
```

### What is inferable

| Shape | Inferable? | Notes |
|---|---|---|
| A literal chain of constructors -- `(SVCons _ (SVCons _ (SVNil)))` | yes | folds to a constant |
| A constructor over an operand with a known index | yes | substitutes operand index into the template |
| Linear `Add`/`Mul` over `Static` and one variable | yes (symbolic) | e.g. `(Add (Static 1) n)` stays open as `(+ 1 n)` |
| An operand whose index is **not known** (a function parameter `v : SizedVec`, a vector built from a runtime list, a value returned by a non-constructor function) | no | the result is left length-polymorphic; `--dump-sizes` prints `(SizedVec ?)` and the value still type-checks |

A function parameter declared `v : SizedVec` (no explicit index) elaborates
against a fresh size variable and works at any length (length polymorphism);
its body's constructions over `v` are the un-inferable case above. This is the
SZ8 elision boundary: inference covers literal/linear shapes, and everything
else falls back to a polymorphic (unannotated) size rather than erroring.

## See also

- [sized-types-completion-plan.md](sized-types-completion-plan.md) -- the SZ4--SZ9 plan
- [guides/sized-types-guide.md](guides/sized-types-guide.md) -- the user guide
- `src/compiler/types.h` -- the `SizeTerm` representation and helper prototypes
- `src/compiler/elab_structs.c` -- `gadt_build_skolem_env` (index capture)
