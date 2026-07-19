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

## Root cause / fix direction

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
