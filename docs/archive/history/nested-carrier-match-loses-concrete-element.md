# Fix trail: nested-carrier-match-loses-concrete-element

**Resolved / archived:** 2026-06-28

## Symptom

`match`ing through two parametric carrier layers lost the concrete element
type: the inner field bindings surfaced as `<struct>`/tyvar, so an arithmetic
use failed operator lookup (TUR-E0006).

```turmeric
(defdata Pair2 [a b] (MkPair2 a b))
(defdata Nest [a]    (N (Pair2 a a)))
(defn main [] : int
  (let [n (N (MkPair2 3 4))]
    (match n
      (N inner)
        (match inner
          (MkPair2 x y) (println (+ x y))))))   ; was: error TUR-E0006
```

A prior full attempt was reverted because step 3 looked like a broad
memory-ownership rework. In the meantime `substitute_adt_app_type_owned`
(deep-clone + `free_struct_app_type`) landed, which de-risked the blocker.

## The fix (three seams, all on the default path)

### 1. Construction-site type-param inference -- `src/compiler/elab_call.c`

The Phase-G0 ctor-call result-type pass (TP5/TS4P1) only grounded a type param
when a field's `full_type` was a bare `TY_TYVAR`. `N`'s sole field is the
parametric `(Pair2 a a)` (TY_APP), so it was skipped and the result stayed a
bare `Nest`. Replaced the app-building block so it grounds EVERY param by
unifying each field's declared `full_type` against the argument's actual type
via `adt_field_collect_type_args`, descending into TY_APP/TY_FN. Kept the
EX_REINTERPRET unwrap so a float payload infers as float, not the int carrier.
`adt_field_collect_type_args` was made non-static (declared in
`elab_internal.h`).

### 2. Match field-bind substitution -- `src/compiler/elab_structs.c`

Both arm-binding branches in `elab_match` only instantiated a field type that
was a bare `TY_TYVAR`. Broadened the guard to also fire for `TY_APP`/`TY_FN`
field types (which `adt_field_instantiate_type` already substitutes through), so
the scrutinee's concrete app args thread into the whole field type. `inner`
binds as `(Pair2 int int)`; the inner `x`/`y` bind as `int`.

### 3. Codegen seams -- `src/compiler/emit_expr.c` + `src/compiler/types.c`

- **Match field read** (`emit_expr.c`, both arm-emit blocks): a non-wide
  by-value ADT field in a monomorph-app scrutinee is stored INLINE as the
  aggregate (the typedef emitter writes the aggregate, not int64, when
  `!type_is_wide_byval_adt`). The old B3 path always deref-unboxed it
  (`*(T*)(intptr_t)(agg)`), an invalid aggregate->intptr_t cast. Now gated on a
  new `scrut_is_app_monomorph` flag (hoisted from the existing `inst_name`
  computation): monomorph-app + non-wide -> inline read; base-carrier / GADT
  representation keeps the int64-box deref. (The first cut over-broadened to all
  non-wide reads and regressed `gadt-adt-skolem`, where the field is genuinely
  int64-boxed; the monomorph-app gate fixes that.)
- **`substitute_adt_app_type` leaks** (`types.c`): `(Nest int)` flowing as a
  concrete app newly drove `adt_field_c_type`, the ADT-app typedef/ctor emitter,
  `adt_app_byval_pass_by_ptr`, and `adt_app_is_byvalue_product`, each of which
  built a malloc'd TY_APP spine via the aliasing `substitute_adt_app_type` and
  dropped it -> LeakSanitizer failure. Switched these local-use callers to the
  already-existing owned variant `substitute_adt_app_type_owned` (deep-cloned,
  no aliasing) and released the result with `free_struct_app_type` after reading
  what they needed. No change to `substitute_adt_app_type`'s contract, so no
  double-free blast radius (the concern that reverted the earlier attempt).

## Verification

- Repro prints `7`; float-through-nest preserves `7.75` (`775` after `*100`);
  three-deep `D(N(MkPair2 10 20))` prints `30`. No leaks (ASan/LSan ON).
- New regression fixture `tests/fixtures/nested-carrier-match-element-type`
  (int + float + three-deep, all unascribed) PASSes.
- `gadt-adt-skolem` (the first-cut regression) restored to PASS.
- Full gate `bash tests/run.sh` (10-min timeout): `1871 passed, 0 failed`.
