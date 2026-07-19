# GCC >= 14 int-conversion residual: carrier vs concrete-pointer representation tracking

**Severity:** medium -- latent today (masked by `-Wno-error=int-conversion` in
`src/main.c`), a hard `cc` error under GCC >= 14. This is the irreducible design
core carved out of `gcc14-int-conversion-carrier-to-typed-param.md` after the 36
tractable fixtures on that front were fixed. What remains needs a
representation-tracking change, NOT another per-site cast.

## Why the tractable fixes stopped here

Turmeric's carrier ABI gives a heap value TWO C representations that coexist in
one program: the `int64_t` carrier word and a concrete typed pointer
(`tur_adt_X *`). The 36 fixtures already fixed all had a LOCALLY decidable
representation at the emit site -- the argument's own emitted type, a matched ABI
spec, an ascription target, or the ctor field type told the emitter which side
of the duality it was on, so a value-preserving `(<ty>)(intptr_t)(v)` bridge
could be inserted.

The ~10 residual fixtures are the cases where **the C representation the callee
(or binder) actually uses is NOT visible in the type the emit site has in hand.**

### Proof: the `map_hyhamt` / `size_hyof` coexistence (map-typed-consumer)

`map-typed-consumer` calls two functions with the *same* monomorphized parameter
type `(Map int int)`:

```
static void *  map_hyhamt(int64_t);                 ;; generic  map-hamt [K V] (m : (Map K V))
static int64_t size_hyof (tur_adt_Map__int__int *); ;; concrete  size-of      (m : (Map int int))
```

- `map-hamt` is **generic** (`[K V]`). It is emitted ONCE; its `(Map K V)` param
  has tyvar arguments, so it lowers to the `int64_t` carrier. Its actual C param
  is `int64_t`.
- `size-of` is **concrete**. Its `(Map int int)` param lowers to the typed
  pointer `tur_adt_Map__int__int *`.

At each call site the emitter only has `fn_binding->type.as.fn.arg_full_types[i]`,
which is **monomorphized to `(Map int int)` for BOTH callees**. `emit_type_c_name`
of that type is `tur_adt_Map__int__int *` in both cases. So no c-name-based
predicate can tell that `map_hyhamt` really wants `int64_t` while `size_hyof`
really wants the pointer. Casting the arg to the c-named pointer type fixes
`size_hyof` and breaks `map_hyhamt`; skipping the cast does the reverse. A
session-verified attempt to gate the cast (emit_expr.c block ~5711) on "did the
carrier bridge already fire" cleared `map_hyhamt` but regressed the
previously-passing `mutmap-typed-consumer` (its `sum_hyvals`/`total` concrete
pointer params lost their needed cast) -- confirming the two are not separable at
this site.

## The two residual classes

### Class A -- call-arg: generic-carrier callee vs concrete-pointer callee

The arg is bridged to (or already is) the `int64_t` carrier, and the callee's C
param is either the carrier (generic callee -> no cast) or a concrete pointer
(concrete callee -> `(ty)(intptr_t)` cast). Indistinguishable from
`arg_full_types` alone.

- `map-typed-consumer` -- `map_hyhamt` (int64 param) beside `size_hyof` (ptr).
- `gde-generic-dict-eq-map` -- `map_hyhamt`/`map_hycount` beside
  `__inst_Eq_eq_qu_Map__spec__...` (int64 param, ptr arg).
- `option-basic` -- `option_hyeq_qu(int64_t cmp_fn)` fed a `void *` closure temp
  on the cps->direct path (emit_cps_ir.c typed-call CSV).

### Class B -- binder init: int64 <-> pointer straddle at a let/temp declaration

The binder is DECLARED with one representation but INITIALIZED from a value
emitted with the other. The declared C type and the init expression's emitted
type disagree, and the value flowing in carries no local signal of which
representation is authoritative.

- `list-homog-byvalue-aggregate-element`,
  `list-length-byvalue-aggregate-element` -- `tur_adt_Cons__Option__int *`
  binder initialized from an `int64_t` (aggregate-element carrier).
- `list-count-phantom-opaque-aggregate-element`,
  `httpd-mw-fold-many` -- `int64_t` binder initialized from a
  `tur_adt_Cons__* *` value.
- `generic-relay-aggregate-result` -- `int64_t` initialized from `void *`.
- `fat-closure-ascription`, `letrec-self-in-nested-closure`,
  `constrained-loop-vec-push-byvalue-result-element` (also a `const char *`
  return + `vec_hypush_ex` arg straddle).

## Root cause

`fn_binding->type` (call sites) and the binder's declared `Type` (let/temp sites)
are **monomorphized source types**. They faithfully record what the value *is*
(`(Map int int)`, `(Cons (Option int))`), but the emitted C ABI for that same
value depends on WHERE it was produced/consumed:

- a **generic** callee/producer emits its heap params/result on the `int64_t`
  carrier (tyvar args cannot be given a concrete C name);
- a **concrete** callee/producer emits the typed pointer.

The type system deliberately erases this producer/consumer-representation
distinction (that is what the carrier ABI is *for*), so it cannot be recovered
by inspecting the monomorphized type at the boundary.

## Fix direction (a dedicated change, not a cast)

Thread the callee's / producer's **actual emitted C parameter/return
representation** to the boundary, rather than re-deriving it from the
monomorphized type:

1. **Call-arg (Class A).** At the regular-call arg loop (emit_expr.c ~4535) and
   the CPS typed-call CSV (`atoms_csv_call_typed` / `cps_call_param_ctype`,
   emit_cps_ir.c), resolve the callee FnDef (not just `fn_binding->type`) and ask
   whether param `i` was emitted as `int64_t` or a concrete pointer -- i.e. reuse
   the exact decision `emit_fns.c` made when it declared the signature
   (`type_uses_carrier_abi` on the callee's DECLARED param type WITH tyvars, plus
   `emit_byvalue_carrier_abi`). Cast only when the callee's real C param is a
   concrete pointer and the arg is the carrier (or vice versa). A generic callee's
   int64 param then never receives a pointer cast. Getting the callee FnDef
   generically is the missing plumbing: today only `emit_reresolve_method_fndef`
   exists, and it is NULL for non-method callees.

2. **Binder init (Class B).** At the let/temp declaration (emit_expr.c ~1557 and
   the binder-decl paths in emit_cps_ir.c), when the binder's declared C type and
   the init's emitted C type sit on opposite sides of the int64<->pointer duality,
   bridge with `(<decl_ty>)(intptr_t)(<init>)`. The signal must come from the
   init producer's emitted representation, not the binder's monomorphized type
   (which is why `bind_is_ptr_repr && init_cn == "int64_t"` under-fires: the value
   is emitted as int64 but its TYPE c-names to a pointer).

Both must preserve the ~15 existing carrier special-cases (each tied to a prior
report) -- verify against the full suite per changed case, watching for the
`mutmap-typed-consumer`-style regression where a concrete-pointer sibling in the
same fixture loses a cast it needs.

## Reproduce

```sh
TUR=./build/tur
for f in map-typed-consumer gde-generic-dict-eq-map option-basic \
         list-homog-byvalue-aggregate-element list-count-phantom-opaque-aggregate-element \
         generic-relay-aggregate-result fat-closure-ascription \
         letrec-self-in-nested-closure constrained-loop-vec-push-byvalue-result-element \
         httpd-mw-fold-many; do
  n=$($TUR emit-c tests/fixtures/$f/input.tur 2>/dev/null \
      | cc -x c -c - -o /dev/null -Werror=int-conversion -Wno-implicit-function-declaration 2>&1 \
      | grep -c "int-conversion")
  echo "$f: $n"
done
```

## Note

The `-Wno-error=int-conversion` flag in `src/main.c` (4 sites) drops only once
this residual AND the sibling `incompatible-pointer-types` front are clean
tree-wide. The `incompatible-pointer-types` front is already resolved
(`docs/archive/gcc14-incompatible-pointer-inline-c-anon-struct.md`); this
representation-tracking residual is the last blocker on the `int-conversion` half.

`httpd-mw-fold-many` also surfaces an unrelated `httpd_tls_ops` /
`HttpdConn`-undeclared emit gap under a bare `emit-c | cc` (its `(load
"stdlib/httpd.tur")` path); that is a separate stdlib-httpd emit concern, not part
of this front -- only its single Class-B `int64_t <- tur_adt_Cons__int *` straddle
belongs here.
