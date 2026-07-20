# GCC >= 14: int64 carrier args reach typed-pointer parameters uncast (int-conversion)

> **RESOLVED / SPLIT (2026-07-19).** The tractable portion of this front is
> done: **36 of 46** flagged fixtures were fixed and committed across the
> regular-call, cps->cps, parametric-opaque-ascription, inline-C-void*-return,
> and existential-pack-ctor paths (full suite green 2202/0 at each landing).
> The ~10 irreducible residual fixtures are NOT point-fixable -- they are the
> carrier-vs-concrete-pointer representation-tracking core, proven this session
> via the `map_hyhamt`/`size_hyof` coexistence (a c-name-based cast that fixes
> one callee regresses its concrete-pointer sibling in the same fixture). That
> design work is carved into its own open report:
> **`docs/reported/gcc14-int-conversion-carrier-representation-tracking.md`**.
> This coarse report is archived; the focused report tracks the remainder.

**Severity:** medium -- latent today (masked by `-Wno-error=int-conversion` in
`src/main.c`), a hard `cc` error under GCC >= 14. The broadest of the fronts
split out of `codegen-gcc14-permerrors.md`.

## Summary

Turmeric's carrier ABI represents many values (rc/weak handles, boxed ADTs,
by-value aggregate elements, opaque handles, dictionaries) as an `int64_t`
carrier in one position and as a typed pointer in another. At a number of
call/constructor boundaries the emitter passes the `int64_t` carrier straight
into a parameter declared as a typed pointer (or vice versa) without the
`(<paramtype>)(intptr_t)` bridge cast, so `cc` reports `-Wint-conversion`
("integer from pointer" / "pointer from integer") -- a hard error under GCC 14+.

Two ctor sub-cases (concrete `rc<T>`/`weak<T>` fields, and fn-typed monomorph
fields) are already fixed. This report covers the REMAINING boundaries.

## Affected boundaries / fixtures

Gathered by compiling every previously-flagged fixture under
`-Werror=int-conversion`:

- **Existential pack ctor** (`ctor_Box`): `exg4-pack-into-struct`,
  `exg4-pack-into-struct-via-let`.
- **By-value aggregate element pointer** (`tur_adt_Cons__Option__int *` param
  fed an int64 carrier): `list-homog-byvalue-aggregate-element`,
  `list-length-byvalue-aggregate-element`.
- **User function with a carrier/aggregate param**: `elem_hyat`
  (`constrained-loop-vec-push-byvalue-result-element`), `sum_hyvec`
  (`letrec-self-recursive-closure`), `sum_hyvals` (`mutmap-typed-consumer`),
  `size_hyof` (`map-typed-consumer`), `bump_hyint` (`heap-make-struct-roundtrip`),
  `free` (`mutex-linear`).
- **Dict / carrier-helper `__cps` calls**: `__inst_Eq_eq_qu_Map__spec__...`
  (`gde-generic-dict-eq-map`), `hamt_slmap__cps` (`hamt-lisp-map-filter`),
  `hamt_slmerge_hywith__cps` (`hamt-lisp-merge-with`), `list_hyeq_qu__cps`
  (`list-basic`), `option_eq___spec__..._cps` (`option-basic`).
- **`int64_t`-note mismatches** (a typed value into an int64 formal or the
  reverse): `fat-closure-ascription`, `generic-relay-aggregate-result`,
  `httpd-mw-fold-many`, `list-count-phantom-opaque-aggregate-element`.
- **Uncaptured-callee int-conversions** (same class, callee name not captured by
  the triage grep -- reconfirm on fix): `data-literal-nested`,
  `hkt-stdlib-logic-instances`, `hkt-stdlib-parser-instances`,
  `letrec-self-in-nested-closure`, `opaque-fn-carrier-dispatch`,
  `opaque-tyvar-through-wrapper-fn`, and the `logic-*` suite
  (`logic-conjoined/-disjoined/-fresh/-occurs-check/-query/-reify/-unify-basic/-unify-fail`).

## Progress (2026-07-19): 33 of 46 int-conversion fixtures fixed

Multiple paths landed (all committed, full suite green 2202/0):
- **Regular calls** -- heap-container arg -> concrete pointer param.
- **cps->cps calls** -- `atoms_csv_call_typed` casts each arg to the `__cps`
  callee's declared param C type.
- **Parametric-opaque ascription** (the big coordinated fix) -- `(:: <ptr> :Goal)`
  where `(Goal A)` lowers to the int64 carrier now reinterprets the pointer to
  int64, so value/binder/return agree. Cleared the whole `logic-*` suite (8),
  `opaque-fn-carrier-dispatch`, and the two `hkt-stdlib-*-instances` at once, zero
  churn.
- **Inline-C returning void\* from an opaque-carrier (int64) function** -- stdlib
  `future.tur` + `opaque-tyvar-through-wrapper-fn` (return the carrier as int64).
- **Existential-pack ctor arg** (`ctor_Box`) -- cast the `tur_exists_t` (void*)
  pack to int64.

**Remaining (13), each a distinct edge case (a naive cast over-fires -- verified):**
`constrained-loop-vec-push-byvalue-result-element`, `data-literal-nested`,
`fat-closure-ascription`, `gde-generic-dict-eq-map` (dict method arg),
`generic-relay-aggregate-result`, `httpd-mw-fold-many`,
`letrec-self-in-nested-closure`, `list-count/homog/length-*-aggregate-element`
(by-value aggregate ELEMENT init: int64 -> `tur_adt_Cons__Option__int *`),
`map-typed-consumer` (heap-ptr arg -> int64 param; a blanket reverse cast
regressed 11 fixtures + 453-line churn, so needs a precise per-arg-C-type guard),
`mutex-linear` (extern-c `free`: int64 -> void*), `option-basic` (`option_hyeq_qu`
arg 3: void* -> int64). These are the residual carrier-crossing sites; each needs
a targeted, individually-verified cast (the front no longer has a single big
lever left -- the opaque reconciliation was it).

## Root cause (pinned 2026-07-19)

The two ctor sub-fronts and the CPS-fn-value front of the umbrella are now fixed
and archived; this front is what remains, and it is the intricate one. A concrete
probe (`sum_hyvec` in `letrec-self-recursive-closure`):

```c
static int64_t sum_hyvec(tur_adt_Vec__int * rs);        // emitted signature: concrete pointer param
...
__ps_164 = (sum_hyvec((int64_t)(intptr_t)(rs_1290)));   // caller casts the arg to int64
```

`rs_1290` is already a `tur_adt_Vec__int *`; the caller casts it **to int64** and
passes it into a `tur_adt_Vec__int *` parameter -> the int64->pointer mismatch is
the `-Wint-conversion`. The cast originates in the ACB carrier bridge
(`src/compiler/emit_expr.c` ~4779, `expr_emits_byvalue_carrier_abi` +
`emit_carrier_bridge_escaping`) and the sibling `pk == TY_INT || TY_STRUCT`
int64-cast branch (~4632). Both decide the target representation from the callee's
**generic** `fn_binding->type.as.fn.arg_kinds[param_idx]` -- which is `TY_TYVAR`
(the carrier view) -- and so bridge the argument TO the int64 carrier. But the
callee was emitted with the **concrete monomorph** signature
`sum_hyvec(tur_adt_Vec__int *)`, not an int64 carrier param. The metadata
(arg_kinds = carrier) and the emitted C signature (concrete pointer) disagree, and
`matched_spec` is NULL at the call, so the carrier bridge fires when it should not.

The uniform tell across all ~20 fixtures is the note
`expected 'tur_adt_X *' (or 'void *') but argument is of type 'long int'`.

## Progress + multi-path finding (2026-07-19)

Landed the **regular-call path** (commit on-branch): a heap-container argument
(`(Vec int)`/`(Set int)`, carried on int64) into a callee with a concrete pointer
param (`sum_hyvec(tur_adt_Vec__int *)`) is now cast to the param's own C type in
the regular-call arg loop (`emit_expr.c`, keyed on the param C type ending in `*`
and not being int64/void*/RcControlBlock/tyvar). Fixes 3 fixtures
(`letrec-self-recursive-closure` + peers), low churn, full suite green.

**The remaining ~33 go through OTHER emit paths, and each needs the SAME
param-type-aware cast -- a blanket carrier cast does NOT work.** Concretely
proven this session: casting every pointer-like arg to int64 in the cps->cps
path (`emit_cps_ir.c`) fixed `list_hyeq_qu__cps` (int64 param) but BROKE
`map_hyeq_hyloop__cps`, whose param is `void *` (pointer->void* was fine;
int64->void* is a new error). So the `__cps` callees do NOT have uniform int64
params -- the cast must consult each callee's ACTUAL param C type, exactly like
the regular-call path now does. That change was reverted; the CPS path remains.

**Update 2: two call-arg paths landed; remaining ~30 span more emit sites than
just call args.** The **cps->cps** path is now also fixed (commit: `atoms_csv_call_typed`
casts each arg to the callee's declared `__cps` param C type -- int64/void*/concrete
-- computed exactly as `emit_params` does; fixes `list-basic`, `hamt-lisp-*`). Two
call-arg paths (regular + cps->cps) are done, 6 fixtures, suite green.

The remaining ~30 are NOT all call-arg sites. A large sub-cluster -- the entire
`logic-*` suite (8) -- is **return-statement and let-assignment** int-conversion,
not call args:

```
error: returning 'void *' from a function with return type 'int64_t' ...
error: assignment to 'int64_t' ... from 'void *' ...
```

So the fix must also reconcile the carrier vs concrete representation at
return-value delivery and let-binding emission, not only at call args. This is
why the front is a dedicated effort: it is the same carrier-representation
ambiguity (pointer vs int64 vs by-value aggregate) surfacing at EVERY
representation-crossing site, each a distinct emitter.

Remaining paths, by site/family:
- **Return / let-assignment** (`logic-*` suite, 8): void* value delivered where
  the C slot / return type is int64 -- return + `LETVAL`/`LETPRIM` emission.
  **Attempted (3 approaches) and found NOT point-fixable:** the mismatch is a
  three-way carrier-representation inconsistency at once -- a closure/Goal binder
  is DECLARED `void *` (`void * __t272;`), the closure VALUE emits as `void *`
  (`emit_value` EX_FN returns the fat-env pointer), and the FUNCTION RETURNS the
  int64 carrier. Fixing any one site introduces the reverse error at another
  (casting the value to int64 makes `void* = int64` at the binder; casting the
  return conflicts with the existing `emit_fat_return_value` / `tail_bv` carrier
  machinery and broke 2 previously-OK TY_FN returns). The correct fix reconciles
  all three -- declare the closure/opaque-handle binder as the int64 carrier (not
  `void *`) so value, binder, and return agree -- inside the fat-closure/opaque
  carrier lowering, a dedicated change, not a per-site cast. This is the same
  opaque-handle carrier ambiguity behind `opaque-fn-carrier-dispatch` and
  `opaque-tyvar-through-wrapper-fn`.
- **cps->direct calls** (`option-basic` `option_hyeq_qu`, some `logic-*`).
- **cps->cps / cps->direct `__cps` calls** (`emit_cps_ir.c`, `atoms_csv_call`):
  `list-basic`, `option-basic`, `hamt-lisp-*`, the `logic-*` suite,
  `gde-generic-dict-eq-map`. Needs a param-type-aware CSV (thread each callee's
  param C types, cast per-param), not the blanket `atoms_csv_call_cps`.
- **existential `ctor_Box`**: `exg4-pack-into-struct(-via-let)`.
- **by-value aggregate element pointer**: `list-homog/length-byvalue-aggregate-element`.
- **extern-c `free`**: `mutex-linear` (void* param, int64 arg).
- **stdlib inline-C** (`future.tur` etc.): `future-*`.
- **misc carrier sinks**: `map-typed-consumer` (size_hyof), `mutmap-typed-consumer`
  (sum_hyvals), `heap-make-struct-roundtrip` (bump_hyint), `fat-closure-ascription`,
  `generic-relay-aggregate-result`, `httpd-mw-fold-many`, `data-literal-nested`,
  `opaque-*`, `letrec-self-in-nested-closure`, `hkt-stdlib-{logic,parser}-instances`,
  `list-count-phantom-opaque-aggregate-element`,
  `constrained-loop-vec-push-byvalue-result-element`.

## Fix direction (updated)

The principled fix reconciles the two views: either (a) resolve `matched_spec` /
the concrete param C type at these call sites and cast the argument to the
callee's DECLARED param C type (`arg_full_types[param_idx]` -> `type_c_name`,
e.g. `(tur_adt_Vec__int *)(intptr_t)rs`) instead of the int64 carrier; or (b)
emit the monomorph callee with an int64 carrier param so the caller's carrier
bridge is correct. Option (a) is the local change but must thread the concrete
param type through the ACB bridge and the `pk==TY_INT/STRUCT` branch WITHOUT
disturbing the ~15 existing carrier special-cases (each tied to a prior report),
so it needs careful per-case verification against the full suite -- this is the
one gcc14 front that is a genuine dedicated effort, not an isolated cast.

## Original fix direction

The general fix is a single principled rule at the call-argument and ctor-argument
emit sites (`src/compiler/emit_expr.c`): when the argument's emitted C type is the
`int64_t` carrier and the callee's DECLARED parameter C type is a pointer (or vice
versa), emit `(<paramtype>)(intptr_t)(<arg>)`. This needs the callee's declared
param C type in hand -- for constructors that is `adt_field_c_type(def, field,
args)` (currently `static` in `types.c`; expose it or a thin wrapper), and for
ordinary/dict/cps callees the `FnDef`/spec parameter type already consulted by the
existing `matched_spec` / `reresolved_callee` logic. The cast is value-preserving
(pure representation bridge), so it changes only the emitted C type, not runtime
behavior.

Prefer the general rule over per-callee-family patches: the two ctor sub-cases
were fixed narrowly (targeted TY_RC/TY_WEAK and TY_FN casts) to keep churn zero,
but the remaining fronts are diverse enough that a declared-param-type-driven cast
is cleaner and less whack-a-mole. Watch snapshot churn -- a general rule will
regenerate many `expected.c`; that is fine per the fixture-regen policy, but keep
each changed line to just the added cast and re-run the full suite.

## Note

One of three remaining fronts under the umbrella
`docs/archive/codegen-gcc14-permerrors.md` (the others:
`gcc14-int-conversion-cps-fn-value-dispatch.md` and
`gcc14-incompatible-pointer-inline-c-anon-struct.md`). The `-Wno-error` flags in
`src/main.c` drop only once all three are clean tree-wide under
`-Werror=incompatible-pointer-types -Werror=int-conversion`.
