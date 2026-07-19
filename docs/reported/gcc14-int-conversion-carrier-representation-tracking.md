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

## Why call-site AST inspection cannot work (three-way disproof, 2026-07-19)

An attempt to gate the pointer cast on the callee's *declared* param type -- to
tell `map_hyhamt` (int64 param) apart from `size_hyof` (pointer param) -- was
carried out with a live `TUR_DBG_C2P` probe over `map-typed-consumer`. The
result: **every reachable AST/type view gives the SAME `tur_adt_Map__int__int *`
for both callees**, even though their emitted C signatures differ
(`map_hyhamt(int64_t)` vs `size_hyof(tur_adt_Map__int__int *)`):

| view consulted at the call site | `map_hyhamt` (generic) | `size_hyof` (concrete) |
| --- | --- | --- |
| `fn_binding->type.as.fn.arg_full_types[i]` (monomorphized) | `tur_adt_Map__int__int *` | `tur_adt_Map__int__int *` |
| callee FnDef `param_types[i]` (via `flatten_program_items` lookup) | `tur_adt_Map__int__int *` | `tur_adt_Map__int__int *` |
| callee FnDef-expr `type.as.fn.arg_full_types[i]` | `tur_adt_Map__int__int *` | `tur_adt_Map__int__int *` |

The int64 carrier param of `map-hamt` is produced ONLY transiently inside
`emit_fns.c`'s signature loop: because `map-hamt [K V]` is a **generic inline-C**
defn, its single emission c-names the param from the *unbound-tyvar* `(Map K V)`
-> `int64_t`. That string is written to the output and then discarded -- it is
not stored back on any FnDef/Type the call site can read. So no predicate over
the AST can recover it. This is the concrete proof that the residual is a
representation-tracking gap, not a missing cast.

## Fix direction (a dedicated change, not a cast)

**Ground-truth approach (recommended).** Record each function's ACTUAL emitted
param C-type strings in a side table keyed by the emitted C name, then consult it
at the cast sites:

1. A dedicated forward-declaration pass already exists and runs before any
   function body: `emit_fn_forward_decls` (`emit_module.c:5003`). It emits every
   `static <ret> <cname>(<param C types>);` prototype up front, so by the time
   call sites (inside bodies) are emitted the table is fully populated. Ordering
   is therefore already satisfied -- no new pass is needed.
2. Factor the per-param C-type computation (the ~15 interacting carrier
   special-cases duplicated between `emit_fns.c`'s signature loop ~3127-3240 and
   the `emit_fn_forward_decls` param loop) into ONE shared function that both
   EMITS and RECORDS `cname -> [param C-type strings]` on `EmitCtx`. This shared
   function is the bulk of the work and the main regression surface -- the two
   copies must stay bit-identical or the forward decl and the definition diverge.
3. At each cast site -- the regular-call arg loop (`emit_expr.c` ~5664/5711), the
   CPS typed-call CSV (`atoms_csv_call_typed`/`cps_call_param_ctype`,
   `emit_cps_ir.c`), and the binder-init straddle (`emit_expr.c` ~1557) -- look up
   the callee's recorded param C type by name and bridge int64<->pointer ONLY when
   the recorded type and the arg's emitted type sit on opposite sides of the
   duality. A generic callee's recorded `int64_t` param then never receives a
   pointer cast; a concrete callee's recorded `tur_adt_X *` param always does.

This replaces the fragile type-heuristic casts with the emitted signature itself,
so it cannot be fooled by the generic/concrete monomorphization collision. It is
nonetheless an invasive cross-cutting change (shared param-C-type function + side
table + three consult sites) with real regression risk against the ~15 carrier
special-cases and the green 2202-test suite -- a dedicated effort, verified per
case against the full suite, NOT an isolated cast.

### Alternative (superseded) fix direction

> Superseded by the three-way disproof above: point 1's premise -- that the
> callee's DECLARED param type is reachable WITH tyvars at the call site -- is
> false. The FnDef's `param_types[i]` and its expr `arg_full_types[i]` are both
> monomorphized to the concrete pointer, so `type_uses_carrier_abi` on them
> returns the same answer for the generic and concrete callee. The
> emitted-signature side table is required. Kept here for the Class-B binder
> reasoning, which still holds.

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
