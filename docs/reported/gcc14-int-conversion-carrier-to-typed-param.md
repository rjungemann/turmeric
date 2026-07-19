# GCC >= 14: int64 carrier args reach typed-pointer parameters uncast (int-conversion)

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

Remaining paths, by callee family:
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
