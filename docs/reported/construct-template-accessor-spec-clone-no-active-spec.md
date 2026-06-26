# Construct-template accessor spec-clone bodies emit field reads with no active ABI spec

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a default-path bug)

## One-line summary

Under the `defstruct-as-defadt` lowering, a generic accessor spec clone
(`unwrap__spec__tur_adt_Box_tur_adt_Option__Box`, body `(.value o)`) emits its
type-erased field read with `ctx->current_abi_specialization == NULL`, so the
read cannot recover the concrete element type and falls back to the wrong
`(int64_t)` cast on a by-value element (`return (int64_t)(o).as.Option._1;` --
a hard cc error for an aggregate `Box`, a silent truncation for a `float`).

## Minimal repro

`tests/fixtures/nested-construct-byvalue-decode/input.tur` built under the
force-lower probe (`TUR_FORCE_LOWER=1 ./build/tur emit-c`), or once the
experiment graduates. The four `unwrap` monomorphs all emit:

```c
static const char * unwrap__spec__const_char___tur_adt_Option__cstr(tur_adt_Option__cstr o) {
        return (int64_t)(o).as.Option._1;          /* int->ptr warning */
}
static double unwrap__spec__double_tur_adt_Option__float(tur_adt_Option__float o) {
        return (int64_t)(o).as.Option._1;          /* TRUNCATES the double */
}
static tur_adt_Box unwrap__spec__tur_adt_Box_tur_adt_Option__Box(tur_adt_Option__Box o) {
        return (int64_t)(o).as.Option._1;          /* ERROR: aggregate value used where an integer was expected */
}
```

The monomorph typedef stores the element by value (correct):

```c
typedef struct tur_adt_Option__Box  { union { struct { bool _0; tur_adt_Box     _1; } Option; } as; } tur_adt_Option__Box;
typedef struct tur_adt_Option__float{ union { struct { bool _0; double          _1; } Option; } as; } tur_adt_Option__float;
```

## Root cause (located)

`unwrap` is `(defn unwrap [A] [o : (Option A)] :A (.value o))`. Its field type
`A` is a bare tyvar, so at elab `e->type` for the `(.value o)` read collapses to
the int64 carrier (`type_c_name(e->type) == "int64_t"`), and
`emit_resolve_type(ctx, e->type)` keeps it int64. The codegen
(`emit_expr.c`, `EX_GET_FIELD` ADT branch, ~L5541) therefore casts the read to
`cty == "int64_t"`.

The intended recovery is to resolve the field through the **receiver's**
concrete spec type: `o : (Option A)` resolves to `(Option Box)` via the active
spec, then `substitute_adt_app_type(A, Option, [Box]) == Box`. The standalone
helper for this is straightforward (a public `adt_field_type_for_app(recv,
field)` over `type_extract_adt_app` + `substitute_adt_app_type`).

The blocker is **getting the receiver's concrete type inside the spec-clone
body**. Instrumenting the read shows the receiver resolution depends on
`ctx->current_abi_specialization` (via `emit_spec_arg_type_for_binding`, which
matches `o` against `aspec->fn->params[0]` -- and since `spec->fn` is the
*shared* generic `unwrap` FnDef, that match would succeed). But for the
cstr/float/Box monomorphs the read fires with:

```
ACC2 mp=as.Option._1 aspec=(nil)  have_rrt=0 rrt.kind=TY_ADT rrt.cn=int64_t
ACC2 mp=as.Option._1 aspec=(nil)  have_rrt=0 rrt.kind=TY_ADT rrt.cn=int64_t
ACC2 mp=as.Option._1 aspec=0x..   have_rrt=1 rrt.kind=TY_APP rrt.cn=tur_adt_Option__int   <- the int spec only
```

i.e. `current_abi_specialization` is **NULL** during emission of the
cstr/float/Box spec bodies, even though the spec-body emit loops
(`emit_module.c` ~L8783 and ~L9986) set
`ctx.current_abi_specialization = sp` before `emit_fn_def`. Only one monomorph
(int) reaches the read with the spec active; the others do not. With no active
spec the receiver `o` resolves only to the abstract int64 carrier and the field
type cannot be recovered.

## Fix directions

The leaf `EX_GET_FIELD` recovery (resolve the field through the receiver's
spec-resolved app type, then: cast to the element type for a scalar, read the
inline aggregate with no cast for a narrow by-value element, deref the int64
box pointer for a wide one) is correct and ready -- it just needs the field
read to see the concrete receiver. The real fix is upstream:

1. Ensure the spec-clone body emit threads `current_abi_specialization`
   consistently to every `(.field o)` read in the body (find why the
   cstr/float/Box `unwrap` monomorphs reach `EX_GET_FIELD` with `aspec == NULL`
   while the int monomorph does not -- likely a second emission path or an
   intervening reset of `current_abi_specialization`), **or**
2. When `aspec == NULL` but a single concrete monomorph is being emitted
   (`ctx->fn_name_override` is the `__spec__` clone name and the clone's FnDef
   carries a concrete result type), recover the field type from the clone's
   declared result type for a return-position accessor read.

Option 1 is the principled fix (it also unblocks the other accessor/construct
seams in this fixture); Option 2 is a narrower patch limited to return-position
accessors.

## Notes

- The whole `EX_GET_FIELD` ADT branch is **lowering-only**: at default the same
  accessor reads a real struct field (`(o).value`, the `def != NULL` branch),
  so any recovery added here is inert on the default path.
- `nested-construct-byvalue-decode` additionally segfaults at runtime under
  lowering even after the accessor compile error is patched -- it carries
  several other seams ("five emit-time gaps" per its header) and is a poor
  single-fix vehicle; a smaller dedicated fixture (one accessor monomorph over a
  by-value element) would isolate this sub-root.
