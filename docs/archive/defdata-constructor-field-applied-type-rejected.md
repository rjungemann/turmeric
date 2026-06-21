# turmeric report: `defdata` constructor fields reject applied type constructors

**Status:** RESOLVED (fixed in the same change that filed this report).

**Repo:** `rjungemann/turmeric`
**Found by:** turmeric-spices Track C U5 (HKT recursion for ASTs) feasibility pass
**Verified on:** turmeric 0.22.0, main @ `15cf5fe`, built from source (`build-release`)
**Severity:** Medium -- blocks U5 AST nodes with container-typed children and a by-value typed `Fix`

## Summary

A `defdata` constructor field must be a primitive keyword (`:int`, `:bool`,
`:cstr`, ...) or a bare type variable. **Any applied type constructor in a
field position is rejected**, whether parametric or fully concrete.

Diagnostic:

```
error: defdata: constructor field type must be a keyword like :int, :bool, :cstr
```

## Repro

```turmeric
(defdata Nest    [a]   (N   (Wrap a)))          ;; rejected (parametric applied)
(defdata ArrNode [a]   (Arr (Box  a)))          ;; rejected
(defdata JsonNode      (JArr (list JsonNode)))  ;; rejected (concrete applied!)
(defdata Fix     [^f]  (Roll (f (Fix f))))      ;; rejected (blocks by-value Fix)
```

Each fails at the applied-type field with the diagnostic above. Bare
type-var fields (`(AddF a a)`) and keyword fields (`(LitF :int)`) are
accepted.

## Why it matters (Track C U5)

U5 ("HKT recursion for ASTs") re-expresses recursive IRs as `Fix F` with a
generic `cata`. Two things are gated on this:

1. **By-value typed `Fix`.** The clean encoding is
   `(defdata Fix [^f] (Roll (f (Fix f))))`. Rejected -- so `stdlib/fix.tur`
   stays int-carrier (`Roll :int`) and folds must thread through an
   `(:: (unroll fix) (F ...))` ascription.
2. **AST nodes with container-typed children.** The U5 json node shape is
   `JsonF a = Null | Bool b | Num n | Str s | Arr (list a) | Obj (map str a)`.
   The `Arr (list a)` / `Obj (map str a)` arms need a sum constructor that
   carries a `(list a)` / `(map str a)` field by value. Rejected.

ASTs whose children sit in bare recursive positions (`Alt a a`, `Star a`,
`Concat a a`) already work end to end (verified: a pure-Turmeric recursive
`cata` evaluator runs correctly), so c-dsl/glsl/scscm/regex/template are
unblocked; only container-child nodes hit this wall.

(Note: the json U5 target itself has since been dropped -- the json spice is
backed by `yyjson` and is not re-expressed as `Fix (JsonF a)`. The
container-typed-field hole this report tracks still gates the remaining U5
targets and a by-value `Fix`, so the fix stands on its own.)

## Secondary, lower-priority finding (filed together)

Single-param `defdata` reports kind `*`, so a `Functor` instance fails
kind-check:

```turmeric
(defdata ExprF [a] (LitF :int) (AddF a a))
(definstance Functor [ExprF] ...)
;; error [TUR-E0012]: kind mismatch -- instance of 'Functor' provides a
;; kind-'*' type for parameter 1 which expects kind '* -> *'
```

`defstruct`/`defopaque` single-param types report `* -> *` correctly
(`Functor [Schema]`, `Functor [Backtrack]` work). Current workaround is a
phantom first param + partial-application head:
`(defdata ExprF [p a] ...)` + `(definstance Functor [(ExprF P)] ...)`.
A fix would make `defdata` kind reporting consistent with the other type
formers.

## Suggested fix direction

Allow applied type constructors (`(C arg...)`, including the HKT
self-application `(f (Fix f))`) in `defdata` constructor field positions,
lowering them the same way `defstruct` already lowers applied-type fields
(e.g. `Cons`'s `(head A)`). Re-check the kind-reporting path for single-param
`defdata` while in there.

## Resolution

Landed exactly along the suggested direction. Changes (all in
`src/compiler/elab_structs.c` and `src/passes/kind_check.c`):

1. **Applied-type constructor fields.** `elab_defdata`'s field loop now
   routes an `F_LIST` field form through the same `struct_field_type_from_form`
   /`struct_field_storage_from_type` lowering `defstruct` uses, producing a
   `TY_APP` `full_type` and a `TY_INT` (heap-pointer) C storage kind. A
   `Symbol**` view of the (bare) type params is built once per `defdata` so the
   shared helper can match type parameters by interned identity. Parametric
   `(Wrap a)`, binary `(Pair2 :cstr a)`, concrete recursive containers, and the
   HKT self-application `(f (Fix f))` are all accepted; bare-symbol and keyword
   fields are unchanged, and the malformed-field diagnostic still fires for
   genuinely non-type forms (the `errors/defdata-malformed-ctor-field-type`
   fixture still passes).

2. **HKT type params on `defdata`.** `defdata`'s type-param parser now strips
   the `^`/`^^` HKT markers (mirroring `defclass`), storing the bare name and
   recording `KIND_ARROW`/`KIND_ARROW2`. This makes `(defdata Fix [^f] ...)`
   bind `f` (so the body's `f` resolves) at kind `* -> *` (so applying it is
   well-kinded). Previously only `^&` (row kind) was handled.

3. **Single-param `defdata` kind reporting (secondary finding).**
   `type_effective_kind` gained a `TY_ADT` case mirroring `TY_STRUCT`: a bare
   parametric ADT/GADT now reports its arity-based kind, so
   `(definstance Functor [ExprF])` validates without the phantom-param
   workaround. To avoid the bidirectional `kind_infer_from_instances` pass
   wrongly promoting a STAR-declared class off a phantom-param ADT head (e.g.
   `Eq [Bound]` from `(defgadt Bound [A] ...)`), a `parametric_adt_arg` guard
   was added alongside the existing `parametric_struct_arg` (SC7) guard.

Regression coverage: `tests/fixtures/defdata-applied-type-field/` exercises a
parametric `(Box a)` field, a binary `(Pair2 :cstr a)` field, and the by-value
typed `(defdata Fix [^f] (Roll (f (Fix f))))`, constructing and matching
through the applied-type field end to end. Full suite green
(`1736 passed, 0 failed`).

**Residual (out of scope, pre-existing):** at a `match`-bound variable the
inner type-app argument can be left as `(type-app Box <struct>)` rather than
resolved to `(Box int)`, so passing such a value to a function expecting the
fully-applied type needs an `(:: x :int)` ascription. This is a type-inference
limitation shared with `defstruct` applied-type fields, not specific to the
rejection fixed here.
