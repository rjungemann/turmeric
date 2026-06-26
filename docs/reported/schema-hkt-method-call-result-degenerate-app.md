# schema-HKT: HKT method-call result type is a degenerate `(f b)` app under lowering

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 4 fixtures.

## One-line summary

Under the `defstruct-as-defadt` lowering, a higher-kinded typeclass method call
(`(fmap s f)`, `(ap ff fa)`, `(alt-or a b)`) over a phantom record-newtype
`(Schema A)` produces a **degenerate result type** -- a `TY_APP` whose `app.fn`
and `app.arg` are both NULL -- instead of the concrete `(Schema int)`.  A
subsequent field read `(.raw (fmap s f))` therefore cannot resolve the receiver
to the Schema record ADT and fails with `error: no typeclass method found for
'raw'`.

## Minimal repro

`tests/fixtures/schema-hkt-functor/input.tur` under the force-lower probe
(`TUR_FORCE_LOWER=1 ./build/tur emit-c`), or once the experiment graduates:

```turmeric
(defstruct Schema [A] (raw :int))            ; phantom A; one int field

(definstance Functor [Schema]
  (fmap [container fn]
    (:: (make-struct Schema (schema/fmap (.raw container) fn)) (Schema int))))

(defn main [] : int
  (let [s (:: (make-struct Schema (schema/int)) (Schema int))]
    (println (schema-decode! (.raw (fmap s double-it)) (json/int 5)))))  ; <- fails
```

Affected: `schema-hkt-functor`, `schema-hkt-alternative`,
`schema-applicative-user`, `schema-applicative-user-errors` (all identical root,
all on `.raw` over an HKT-method-call result).

## Root cause (located)

The receiver of the failing `(.raw ...)` is an `EX_CALL` (the `fmap` call).
Instrumenting the dot-access ADT-field branch (`elab_typeclasses.c`, the
`adt_base` unwrap at ~L5142) shows:

```
DOT method=raw fot.kind=TY_APP base.kind=TY_ADT obj.kind=EX_VAR  ...   <- (.raw s), OK
DOT method=raw fot.kind=TY_APP base.kind=TY_APP obj.kind=EX_CALL app.fn=(nil) app.arg=(nil)  <- (.raw (fmap s f)), FAILS
```

i.e. the `fmap` **call's own result type** is a `TY_APP` with `app.fn == NULL &&
app.arg == NULL` -- a zero-initialized/degenerate application, not `(Schema
int)`.  The dot-access ADT branch unwraps the `TY_APP` head looking for a
`TY_ADT` (the record def) and finds only the empty app, so it falls through to
typeclass-method dispatch, which has no `raw` method -> the diagnostic.

At **default** the same call's result head is a concrete `TY_STRUCT(Schema)`, so
the dot-access struct-match loop (`elab_struct_type_extract_args`, which requires
a concrete `TY_STRUCT` head) recovers it.  The struct->ADT lowering is the only
change: under lowering Schema is a record ADT (absent from `struct_defs`), the
struct-match loop misses it, and the ADT branch only handles a *concrete*
`TY_ADT` head -- which the degenerate app is not.

The degeneracy originates in the HKT method **result-type concretization**: the
method's declared return is the applied class family `(f b)`, and binding `f` to
the receiver's type constructor (Schema) is what should yield `(Schema int)`.
The M7 head-rewrite (`elab_typeclasses.c` ~L3293-L3344) special-cases a
`TY_STRUCT` head (`type_args[ti].as.app.fn->kind == TY_STRUCT`, the partial-hole
path) and otherwise grafts `type_args[ti]` as the new head; under lowering the
Schema type constructor flows through as a lowered ADT and the rewrite leaves the
result an empty app rather than `app(Schema_adt, b)`.

## Fix directions

1. **Primary**: in the HKT method result-type rewrite (M7,
   `elab_typeclasses.c` ~L3293), handle a lowered **record-ADT** type
   constructor head the same way the `TY_STRUCT` head is handled -- graft the
   concrete `TY_ADT` (or its app) into the `(f b)` head so the call result is
   `(Schema int)`, not a degenerate app.  This is the principled fix and unblocks
   all four fixtures (and likely the `hkt-stdlib-*` HKT fixtures that share the
   phantom-wrapper shape).
2. **Fallback at the dot-access** (narrower, less principled): when the dot-access
   receiver type is a degenerate/abstract app but the receiver is an `EX_CALL` to
   a class method whose dispatch instance is known, recover the result head from
   the resolved instance's return type.

Option 1 is correct; it lives in the experimental M7 HKT machinery
(`g_m7_hkt_enabled`), which has high regression surface, so it wants dedicated
care rather than a leaf patch.

## Notes

- Default suite is unaffected (the whole path only triggers under the lowered
  ADT representation of a phantom record newtype).
- The dot-access ADT branch itself is correct; it simply receives a result type
  that was never concretized.  No leaf change there resolves the cluster.
