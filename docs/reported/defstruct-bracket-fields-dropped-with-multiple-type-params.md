# defstruct: bracket `[name : type ...]` fields dropped when the struct has 2+ type params

**Summary.** A `defstruct` written with the bracket field-vector form
`[a : T b : U ...]` **silently keeps only its first field** when the struct
declares two or more type parameters (`[S A]`). The `(name type)` list form is
unaffected. The extra fields vanish from the generated struct and constructor
with no diagnostic, so downstream `.field` access fails with a confusing
"unknown field" / "no typeclass method" error far from the real cause.

**Severity:** medium (silent data loss in a struct definition; no error at the
definition site, only a misleading one at the use site).

## Minimal repro

```turmeric
;; Two type params + bracket fields -> only `a` survives.
(defstruct T [S A] [a : int b : int])
(defn main [] : int 0)
```

Generated C:

```c
typedef struct tur_adt_T {
    int64_t a;          /* b is gone */
} tur_adt_T;
static int64_t ctor_T(int64_t _0) { ... }   /* arity 1, not 2 */
```

Contrast -- both work correctly:

```turmeric
(defstruct T [A]   [a : int b : int])   ; ONE type param  -> a, b both kept
(defstruct T [S A] (a int) (b int))     ; (name type) form -> a, b both kept
```

So the bug is specific to **bracket fields + 2-or-more type params**.

## Impact

Hit while writing `stdlib/lens.tur` (`Lens [S A]` with two fn-typed fields).
Worked around by using the `(name type)` field form. A user reaching for the
bracket form on any 2-parameter container (`Pair`, `Map`, `Result`-like,
`Lens`) loses every field after the first with no warning.

## Root cause (suspected -- not yet pinpointed)

The bracket field-vector parse in `defstruct` lowering (`elab_structs.c`)
appears to mis-associate the field vector after consuming a multi-element type
param vector `[S A]` -- likely reading the field vector's length or start
against the wrong offset once `n_type_params > 1`, so only the first field pair
is registered. The `(name type)` list form takes a different parse path and is
unaffected. Needs a look at the field-collection loop relative to where the
type-param vector is consumed.

## Fix directions

- Pinpoint the bracket-field collection in the `defstruct` -> record-`defadt`
  lowering and make its field count independent of `n_type_params`.
- Add a fixture: `(defstruct T [S A] [a : int b : int c : int])` must generate a
  three-field struct and an arity-3 constructor.
- Until fixed, prefer the `(name type)` field form for structs with 2+ type
  parameters (what `stdlib/lens.tur` does).
