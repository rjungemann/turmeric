---
title: rank-N + HKT elaboration NULL-derefs `type_param_kinds` under TUR_M7_HKT
severity: medium (crash, but only under the experimental TUR_M7_HKT flag; no
  effect on the shipped default-OFF path)
status: RESOLVED 2026-06-19 (fixed in the same session it was found; guard added
  at elab_types.c:961). Archived.
---

# rank-N + HKT elaboration NULL-derefs `type_param_kinds`

> **RESOLVED 2026-06-19.** Applied the proposed guard
> (`type_param_kinds ? type_param_kinds[i] : KIND_STAR`) at `elab_types.c:961`.
> `TUR_M7_HKT=1` on `hrt-rankn-hkt` / `hrt-rankn-typeclass` now elaborates and
> runs (20 / 55) instead of crashing; flag-off suite stays 1683/0.

## One-line summary

Under `TUR_M7_HKT=1`, elaborating a rank-N type that introduces bound type
variables into an enclosing HKT context dereferences a NULL `type_param_kinds`
array, crashing the elaborator (`elab_types.c:961`, UBSan: "load of null pointer
of type 'Kind'"). **Pre-existing** -- it reproduces identically at the parent
commit of the layer-4 emit work, and is unrelated to it; found incidentally
during a flag-on regression sweep.

## Repro

```sh
TUR_M7_HKT=1 ./build/tur run tests/fixtures/hrt-rankn-hkt/input.tur
TUR_M7_HKT=1 ./build/tur run tests/fixtures/hrt-rankn-typeclass/input.tur
```

Observed (both):

```
src/compiler/elab_types.c:961:52: runtime error: load of null pointer of type 'Kind'
```

With the flag OFF (the default, and what `tests/run.sh` uses) both fixtures
PASS -- this is a flag-on-only crash.

## Root cause

`elab_types.c:958-962` copies the existing type params into an extended array
when a rank-N form binds new variables:

```c
for (uint8_t i = 0; i < n_type_params; i++) {
    ext_params[i] = type_params[i];
    ext_kinds[i]  = type_param_kinds[i];   // <-- line 961: NULL deref
}
```

`type_param_kinds` follows the documented convention "NULL means all-`KIND_STAR`"
(see the field comment in `typeclass.h` and the `is_hkt` detection in
`emit_module.c` / `elab_typeclasses.c`, which both guard `if (tc->type_param_kinds)`
before indexing). This loop does **not** guard it: when `n_type_params > 0` but
`type_param_kinds == NULL`, line 961 reads through NULL.

The reason it only surfaces under `TUR_M7_HKT=1` is that the flag's
method-level-tyvar collection (`m7_collect_form_tyvars`, `elab_typeclasses.c`)
routes a rank-N HKT signature down this extended-params path with a NULL kinds
array that the flag-off path never reaches.

## Proposed fix

Treat a NULL `type_param_kinds` as all-`KIND_STAR` at the copy site, matching
the rest of the codebase:

```c
for (uint8_t i = 0; i < n_type_params; i++) {
    ext_params[i] = type_params[i];
    ext_kinds[i]  = type_param_kinds ? type_param_kinds[i] : KIND_STAR;
}
```

## Validation

- `TUR_M7_HKT=1 ./build/tur run tests/fixtures/hrt-rankn-hkt/input.tur` no longer
  crashes (and ideally produces the same result as flag-off).
- `bash tests/run.sh` stays 1683/0 (flag-off path is untouched -- the guard only
  changes behavior when `type_param_kinds` is NULL, which the all-STAR convention
  already says is the default).
