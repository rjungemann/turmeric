# GCC >= 14 int-conversion residual: carrier vs concrete-pointer representation tracking

> **RESOLVED / FLAG DROPPED (2026-07-20).** Every flagged carrier-to-typed-param
> int-conversion fixture compiles clean under `-Werror=int-conversion`, AND the
> `-Wno-error=int-conversion` + `-Wno-error=incompatible-pointer-types` downgrades
> have been REMOVED from `src/main.c` (both the shared-lib and executable link
> paths). The representation-tracking machinery this report called for is fully
> implemented (the `emit_sig_*` param side table + the `emit_localvar_*` local-var
> side table + consumer-side bridges at the let/letrec binder init, the TCO
> tail-backedge, the fn-body return chain, the control-result assignment, and the
> spec-dispatch call arg). The tree-wide confirmation the banner previously
> awaited is done: the **full suite runs with both warnings promoted to hard
> errors via `tur build`, 2202 passed, 0 failed** -- so every fixture's emitted C
> is clean, not just the originally-flagged 46. This report and the whole
> `codegen-gcc14-permerrors` umbrella are archived.

**Severity:** medium -- latent today (masked by `-Wno-error=int-conversion` in
`src/main.c`), a hard `cc` error under GCC >= 14. This is the irreducible design
core carved out of `gcc14-int-conversion-carrier-to-typed-param.md` after the 36
tractable fixtures on that front were fixed. What remains needs a
representation-tracking change, NOT another per-site cast.

## LANDED progress (2026-07-19): ground-truth side table + 3 more fixtures

The **ground-truth side table** the fix direction below calls for is now
implemented and in-tree (commits on `claude/tractable-report-execution-1yvqp9`):

- `emit_module.c`: `EmitSigEntry` table keyed by emitted C name, populated from
  the `emit_fn_forward_decls` pass by capturing each param's ACTUAL emitted type
  substring (whatever branch wrote it -- no refactor of the ~15 special-cases).
  `emit_sig_reset` / `emit_sig_record_param_ctype` / `emit_sig_lookup_param_ctype`
  (declared in `emit_internal.h`).
- `emit_expr.c`: the regular-call heap-ptr->concrete-pointer cast consults the
  RECORDED callee param type; a generic carrier-ABI callee (recorded `int64_t`)
  no longer gets a spurious pointer re-cast. **Fixes `map-typed-consumer` (3->0)
  and cleans the `set_hycount` over-fire in `set-typed-consumer`** -- exactly the
  coexistence the disproof below said no type-heuristic could crack.
- `emit_cps_ir.c`: the cps->direct tail-call now casts its args through the typed
  CSV (like cps->cps), and a fat-fn arg into an int64 fn-carrier param is bridged.
  **Fixes `option-basic` (1->0)**; reduces `gde-generic-dict-eq-map` (6->1).

A further **Class B forward straddle** fix then landed at the let/loop
binding-init emitter (emit_expr.c): a pointer-declared binder fed a value emitted
as the int64 carrier (the `(int64_t)` prefix is the signal, since the init TYPE
c-names to the pointer) is reinterpreted to the binder's pointer type. **Fixes
`list-homog-byvalue-aggregate-element` and `list-length-byvalue-aggregate-element`
(2->0 each).**

All fixes verified against the full suite: **2202 passed, 0 failed** each. Net
this session: **5 fixtures fixed** (map-typed-consumer, set-typed-consumer,
option-basic, list-homog, list-length) + `gde` 6->1, on top of the ground-truth
side table now in tree for reuse.

## LANDED (2026-07-19, session 2): the local-var type table + 3 more fixtures

The `emit_localvar_*` side table the "principled fix" below called for is now
implemented and in-tree, and it fixed the reverse straddle for the cases whose
value is a control-result / call-hoist temp:

- `emit_module.c`: `emit_localvar_reset/record/lookup(cname -> emitted C type)`,
  mirroring `emit_sig_*`, reset per program.
- `emit_expr.c`: record each control-result temp's ACTUAL emitted C type in
  `emit_control_result_temp_decl` (all three branches), and each `__auto_type`
  call-hoist temp's representation type (concrete pointers only) at the
  `emit_value` hoist site. Consult the table at the int64-binder init branch of
  `emit_let_value` / `emit_letrec_value` (via `emit_str_is_bare_ident` +
  `emit_localvar_lookup_ctype`): a bare temp recorded as a concrete pointer
  flowing into an int64 binder is reinterpreted through `intptr_t`.
- `emit_fns.c`: the same consult at the TCO tail-backedge arg temp
  (`emit_tail_backedge`) -- a recorded pointer temp into an int64 param slot.

**Fixed (session 2, first pass): `list-count-phantom-opaque-aggregate-element`
(4->0), `fat-closure-ascription` (1->0), `httpd-mw-fold-many` (1->0).** Full
suite **2202 passed, 0 failed, no snapshot churn** at each landing -- the table
is precise (it keys on the temp's REAL recorded representation, so it never adds
the value-preserving cast where an earlier stage already reconciled, avoiding the
139-fixture over-fire the type-based attempt caused).

## LANDED (2026-07-19, session 2 cont.): letrec-self via closure-carrier recording

`letrec-self-in-nested-closure` (1->0): a self-capturing closure whose letrec is
lowered to a plain `let` emits `int64_t self_1286 = (int64_t)(intptr_t)(env)`
(the boxed/fat closure carrier) via the `is_fat || boxed` and
closure-returning branches of `emit_let_value`'s binding loop (NOT the generic
`else`), then the let-result temp `void *__t168 = self_1286;` straddles.
Landed fix:

- `emit_expr.c`: record the boxed/fat and closure-returning int64-carrier let
  binders in `emit_localvar_*`.
- `bridge_control_result_int_ptr`: bridge a `void *` result temp fed an int64
  value (previously void\* temps were skipped), and consult the local-var table
  for a bare-identifier value whose recorded type is ground truth.

Full suite **2202 passed, 0 failed, no churn**. **Session-2 total: 4 fixtures
(list-count-phantom, fat-closure, httpd, letrec-self); front now 43/46.**

## LANDED (2026-07-19, session 2 final): the last 3 fixtures

All three "remaining" fixtures were resolved -- each with the consumer-side
representation-tracking approach, extended to its specific emit path:

- `constrained-loop-vec-push-byvalue-result-element` (3->0) -- **spec-clone body
  return.** `err_val__spec__..._const_char_` / `ok_val__spec__...` return a
  concrete pointer but their body is a field read emitted as the int64 carrier
  (`return (int64_t)((tur_adt_Result *)..)->err_val;`). The return is emitted by
  the fn-body return-branch chain's plain-return fallback (emit_fns.c ~3982, keyed
  on `ret_ctype`); bridge there when the fn's C return type is a concrete pointer
  and the return value is an explicitly int64-cast expression.
- `generic-relay-aggregate-result` (1->0) -- a `void *` union-default read
  (`((union { int64_t s; void * d; }){.s = ..}).d`, from a `(:: <int> :ptr<void>)`
  carrier relabel) bound to an int64 binder. Detect the exact void*-member union
  read at the let/letrec binder init (value ends in `}).d` and declares
  `void * d;` -- unique to this emit, cannot match an int64 value) and reinterpret
  through the existing int64-binder bridge.
- `gde-generic-dict-eq-map` (1->0) -- a `__inst_Eq_..._int64_t(a, b)`
  spec-dispatch call with a `tur_adt_Map__cstr__int *` arg `b` into the int64
  param. Root cause: the `emit_sig` param table was populated only from the
  forward-decl pass over top-level items, so ABI specializations had NO recorded
  signature and the call-site reverse cast could not tell the param was the
  carrier. Fix: record each ABI spec's emitted param C types into `emit_sig` at
  the spec forward-decl emitter (keyed by clone name == call-site fn_name), then
  consult it at the dict/spec-dispatch call arg loop to reverse-cast a
  concrete-pointer arg into a recorded-int64 param. (This also corrected a latent
  2-error int-conversion in `van-laarhoven-lens-wide-compose`; snapshot
  regenerated.)

**The carrier-to-typed-param int-conversion front is COMPLETE: all 46 flagged
fixtures compile clean under `-Werror=int-conversion`, full suite 2202/0.**

The forward field-read straddle could NOT be fixed at the ascription site: KB-021
requires an ascription NOT to change a carrier-ABI aggregate's representation,
because a sibling consumer (`vec-push!`, `ok`, ...) reads the same value as the
int64 carrier -- the correct representation depends on the CONSUMER. Accordingly
every landed fix is consumer-side (binder init, tail-backedge, fn return, control
result, spec-dispatch call arg), keyed on a ground-truth side table
(`emit_sig_*` param types / `emit_localvar_*` local types) rather than the
colliding monomorphized source type.

## Tree-wide sweep (2026-07-19): 16 untriaged fixtures remain

A full sweep (`emit-c | cc -Werror=int-conversion` over every fixture) after the
46 flagged were clean surfaced **16 more** fixtures with int-conversion errors --
so the front is complete for the FLAGGED set but NOT tree-wide, and the flag
cannot drop yet. All 16 are the same representation-duality classes (a local cast
keyed on the ground-truth side tables, not the source type):

| fixture | errs | class |
| --- | --- | --- |
| `schan-worker-pool` | 6 | mixed |
| `show-collections-content-hamt` | 4 | forward spec-dispatch (`__inst_Show_show_cstr`: int64 arg -> cstr param) |
| `w3-letrec-open-capture` | 3 | `vec_hypush_ex` arg 2 (pointer -> int64 param) |
| `taskgroup-linear` | 2 | -- |
| `taskgroup-with-macro-real` | 2 | -- |
| `van-laarhoven-lens-wide-generic` | 2 | -- |
| `vec-eq-ascribed-multi` | 2 | forward spec-dispatch (`__inst_Eq_eq_qu_cstr`) |
| `vec-eq-cstr-content` | 2 | forward spec-dispatch (`__inst_Eq_eq_qu_cstr`: int64 arg -> cstr param) |
| `vec-get-exists-element` | 2 | `vec_hypush_ex` arg 2 (pointer -> int64 param) |
| `reactor-fibers-park-chan` | 1 | -- |
| `session-effects` | 1 | -- |
| `session-mp-effects` | 1 | -- |
| `show-collections-content` | 1 | forward spec-dispatch (`__inst_Show_show_cstr`) |
| `van-laarhoven-lens-compose` | 1 | binder-init reverse straddle (`int64_t = tur_adt_Line *`) |
| `van-laarhoven-lens-wide-compose` | 1 | residual `int64_t = tur_adt_Line *` (a second straddle in this fixture) |
| `vec-captureless-fat-closure-readback` | 1 | -- |

### Progress on the 16 (2026-07-19)

**Fixed (3):** `vec-eq-cstr-content` (2->0), `vec-eq-ascribed-multi` (2->0),
`show-collections-content` (1->0) -- via the landed **forward spec-dispatch cast**
(emit_sig-recorded concrete-pointer param + int64 arg -> cast to the pointer),
the mirror of the gde reverse cast. `show-collections-content-hamt` 4->3.

**Remaining (13), by sub-class:**
- **Forward spec-dispatch not yet reached** -- `show-collections-content-hamt`
  (3, a `__inst_Show_show_cstr` call site the forward cast does not reach),
  `vec-captureless-fat-closure-readback` (`vec_empty_like__spec` arg 1, int64 ->
  pointer param).
- **Regular / inline-C base call-arg straddle** -- `reactor-fibers-park-chan`
  (`chan_hysend` arg 1, pointer -> int64), `vec-get-exists-element` +
  `w3-letrec-open-capture` (`vec_hypush_ex` arg 2, pointer -> int64),
  `session-effects` + `session-mp-effects` (`spawn` arg 1, int64 -> pointer). These
  callees are inline-C carrier bases, NOT ABI specs, so they are absent from
  `emit_sig`; record inline-C base signatures too, or reverse-cast from the callee
  `FnDef` param C type.
- **Return straddle** -- `van-laarhoven-lens-wide-generic` (returns `tur_adt_Point
  *` but tail is an int64 `__auto_type` temp, not `(int64_t)`-prefixed, so the
  landed return bridge's prefix key misses it), `taskgroup-linear` +
  `taskgroup-with-macro-real` (**inline-C body** `return fiber;` where `fiber` is a
  `void *` local and the fn returns int64 -- a SOURCE-level cast in the inline-C is
  the fix, not a codegen bridge).
- **Binder-init reverse straddle** -- `van-laarhoven-lens-compose` (`int64_t =
  tur_adt_Line *`, an unrecorded producer temp), `schan-worker-pool` (6; includes
  `int64_t = void *`, which `bridge_control_result_int_ptr` / the binder path skip
  for `void *`).
- `van-laarhoven-lens-wide-compose` (1) -- a second `int64_t = tur_adt_Line *`
  straddle beyond the one already cleared.
- `vec-captureless-fat-closure-readback`, `reactor-fibers-park-chan` also touch
  the fat-closure/chan carrier readback.

The `__auto_type` cases are the hard core: the call-hoist temp deliberately takes
the call's emitted type because the emitter cannot compute it (int64 vs pointer)
from the source type -- so a prefix/repr-name key is unreliable there (it also
caused the only churn this session: 2 harmless redundant casts in
map/set-typed-consumer, snapshots regenerated). A principled finish records each
inline-C/spec base's emitted return + param C types in `emit_sig` and threads them
to the call/return sites, replacing the prefix heuristics.

## Dropping the `-Wno-error=int-conversion` flag

The flag in `src/main.c` (4 sites) covers three warnings; `int-conversion` is one.
The sibling `incompatible-pointer-types` front is already resolved
(`docs/archive/gcc14-incompatible-pointer-inline-c-anon-struct.md`). Drop the
single `int-conversion` token only after the 16 above are clean AND a re-run
sweep is empty (leaving `incompatible-pointer-types` /
`implicit-function-declaration` until their own fronts clear). Keep the
`emit_sig`/`emit_localvar` side tables and consumer-side bridges -- they are the
mechanism that keeps the tree clean.

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
