# Lowered record-ADT constructor skips make-struct's fn-field type-param inference

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 3 fixtures.

## One-line summary

When a `defstruct` lowers to a single-variant record ADT, `make-struct` rewrites
to the ADT constructor call and delegates to `elab_call`
(`elab_make_struct`, elab_structs.c ~L4291-L4301).  That path does NOT run the
struct path's `struct_field_collect_type_args` inference that descends into a
**function-typed** field to ground the struct's type parameters from the
supplied function value's signature.  So for a parametric struct whose type
parameter appears only inside a fn field -- `(defstruct Lens [S A] (get (fn [S]
A)) (put (fn [S A] S)))` -- the lowered ctor leaves `S`/`A` unbound, and a later
fn-field call `(.get l p)` types as a bare tyvar, failing overload resolution in
an untyped context (`(println (.get l p))` -> TUR-E0006 "first arg type tyvar").

## Minimal repro

`tests/fixtures/dot-parametric-fn-field-call`,
`tests/fixtures/make-struct-parametric-fn-field-infer` under the force-lower
probe (`TUR_FORCE_LOWER=1 ./build/tur emit-c`), or once the experiment
graduates:

```turmeric
(defstruct Lens [S A] (get (fn [S] A)) (put (fn [S A] S)))
(defstruct Person :copy [name : cstr age : int])
(defn name-get [p : Person] : cstr (.name p))
(defn name-put [p : Person n : cstr] : Person (make-struct Person n (.age p)))

(defn main [] : int
  (let [l (make-struct Lens name-get name-put)
        p (make-struct Person "Bob" 40)]
    (println (.get l p))    ;; <- types as a bare tyvar A under lowering
    0))
```

All three repros pass at default (the struct make-struct path infers `S`/`A`);
only the lowered ADT-ctor path regresses.

## Root cause (located)

- `struct_field_collect_type_args` (elab_structs.c ~L514) handles the `TY_FN`
  case (L540-L568): it descends into the declared fn field and unifies each
  arg/result slot against the supplied function value's signature, grounding the
  struct type params.  `elab_make_struct` calls it per field (L4417) on the
  STRUCT path.
- Under lowering `elab_make_struct` short-circuits to the ADT constructor call
  (L4291) and never reaches that inference.  `elab_call`'s
  `call_collect_type_bindings` (elab_call.c ~L329) does have a `TY_FN` descent,
  but the fn argument here is a top-level defn reference that arrives fat-boxed /
  as `ptr<void>` at the binding site (L339 returns without binding), so `S`/`A`
  are never grounded for the ctor's result type, and `(.get l p)`'s field type
  stays the unsubstituted tyvar.

(Related but already-fixed sibling: the BARE-receiver tyvar-field collapse
`(.ok-val r)` over an unparameterized `Result` -- that one was a missing
carrier-collapse in the dot-access ADT branch and is now handled.  This report
is specifically the make-struct/ctor INFERENCE gap, not the field-read.)

## Fix directions

Port the fn-field-descending type-param inference to the lowered record-ADT
constructor path so `(make-struct Lens name-get name-put)` grounds `S`/`A` the
same way the struct path does -- either:

1. In `elab_make_struct`'s ADT short-circuit, run the existing
   `struct_field_collect_type_args` walk (adapted to read the ctor's
   `CtorField.full_type` instead of `StructField.full_type`) and pass the
   inferred type args through to the constructor call's result type; or
2. Strengthen `call_collect_type_bindings`'s `TY_FN` arm so a fat-boxed /
   top-level-defn fn argument still contributes its concrete arg/result types
   to the binding set (recover the signature from the argument's binding type
   rather than bailing at `ptr<void>`).

Option 1 keeps the inference identical to the struct path (lowest risk of
divergence); option 2 is more general but touches the hot call-elaboration path.

## Notes

- Default suite is unaffected (only the lowered record-ADT representation of a
  parametric struct with a fn-typed field triggers it).
- `linear-lref-struct-field` shows the same TUR-E0006 shape but over an
  `lref<int>` field (`first arg type ?`), a distinct lref-field-read sub-root,
  not this inference gap.
