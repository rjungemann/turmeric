# GCC >= 14: CPS threaded-fn-value dispatch passes uncast args to the int64 fn-ptr (int-conversion)

> **Status:** RESOLVED 2026-07-19. Added `atoms_csv_call_cps` (a carrier variant
> that casts every pointer-like arg -- fn/ptr<void>/cstr/rc/weak/ref/cont/forall
> -- through `(int64_t)(intptr_t)`) and used it at both E2a `__tur_cps_lookup`
> dispatch sites (the via_registry tailcall and the heap-join tailcall). All
> ~11 affected fixtures now compile clean under `-Werror=int-conversion`; zero
> snapshot churn (run-only fixtures); full suite green. Commit: `33f5cb1`.
>


**Severity:** medium -- latent today (masked by the `-Wno-error=int-conversion`
workaround in `src/main.c`), a hard `cc` error the moment CI's compiler crosses
GCC 14. One of the fronts split out of `codegen-gcc14-permerrors.md`.

## Summary

The E2a "threaded fn-value" dispatch emits a call through
`__tur_cps_lookup`, casting the looked-up function pointer to
`int64_t (*)(int64_t, DK *)` and then passing the call's arguments **uncast**.
When an argument is a `const char *` (or any pointer/carrier that is not already
`int64_t`), passing it into the `int64_t` parameter is a `-Wint-conversion`
("integer from pointer" / "pointer from integer"), promoted to an error under
GCC >= 14.

## Repro

```sh
tur emit-c tests/fixtures/effect-fn-type-annot/input.tur \
  | cc -x c -c - -o /dev/null -Werror=int-conversion -Wno-implicit-function-declaration
```

Emitted C (the offending call):

```c
return ((int64_t (*)(int64_t, DK *))__tur_cps_lookup((intptr_t)f))(
           "fn type annot test", __kont); /* E2a threaded fn-value */
//         ^^^^^^^^^^^^^^^^^^^^^ const char * passed into an int64_t parameter
```

## Affected fixtures (~11)

`effect-fn-type-annot`, `capability-effect-poly`, `cps-backend-fn-param-effectful`,
`cps-tramp-resume-e2a-fnvalue`, `cps-tramp-resume-e2c-effectful-fnvalue-nontail`,
`cps-tramp-resume-effect-subtype-capability`,
`cps-tramp-resume-effect-subtype-capability-effectful`, `effect-struct-field-row`,
`effect-subtype-capability`, `effect-type-alias`, and peers. (List gathered by
compiling every previously-flagged fixture under `-Werror=int-conversion`; a full
re-scan when fixing may surface a few more of the same shape.)

## Root cause

The E2a threaded-fn-value call emitter (grep the codegen for
`E2a threaded fn-value` in `src/compiler/emit_cps_ir.c` / `emit_expr.c`) builds
`((int64_t (*)(int64_t, DK *))__tur_cps_lookup(...))(<args>, __kont)` but does not
coerce each `<arg>` to the `int64_t` carrier the synthesized fn-ptr type
declares. A scalar `const char *`/pointer argument therefore reaches an
`int64_t` formal without the `(int64_t)(intptr_t)` carrier cast the rest of the
call path applies.

## Fix direction

At the E2a dispatch emit site, cast each threaded argument to the fn-ptr's
declared carrier parameter type -- `(int64_t)(intptr_t)(<arg>)` for a
pointer/cstr arg -- mirroring the carrier-scalar cast the ordinary call-arg loop
already performs (`emit_expr.c`, the `needs_fn_cast` / `scalar_carrier_cty`
machinery). The cast is value-preserving, so runtime behavior is unchanged; it
only makes the emitted C well-typed. Verify by compiling the affected fixtures
under `-Werror=int-conversion` (a stand-in for GCC >= 14's default) and by the
full suite staying green.

## Note

This is one of three remaining int-conversion/pointer fronts under the umbrella
`docs/archive/codegen-gcc14-permerrors.md`. The two ctor fronts (rc/weak fields;
fn-typed monomorph fields) are already fixed. The two `-Wno-error` flags in
`src/main.c` can only be dropped once THIS report, the carrier-to-typed-param
report, and the inline-C anon-struct report are all resolved and the whole tree
compiles clean under `-Werror=incompatible-pointer-types -Werror=int-conversion`.
