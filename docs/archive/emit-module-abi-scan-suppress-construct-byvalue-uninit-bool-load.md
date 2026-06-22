# emit_module.c:3170 -- UBSan: load of garbage byte for `bool ctx->abi_scan_suppress_construct_byvalue` (FIXED)

Fixed in the same Track C / U6 session that surfaced it: one-line
initialization at `emit_program`'s `EmitCtx` setup
(`ctx.abi_scan_suppress_construct_byvalue = false;`), inserted alongside
the other field inits at `emit_module.c:7444` and the parallel block at
`:8853`. UBSan no longer trips on `tur check` / `tur run` / `tur test`
against spice tests.

**Found by:** turmeric-spices Track C / U6 c-dsl uplift session (running
`tur test`, `tur run`, and `tur check` against arbitrary spice tests on a
Debug build).
**Verified on:** turmeric main @ `affeb7a6` (post #499), Debug ASan/UBSan
build (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`).
**Severity:** Low. UBSan tripped, but the affected field is read as a
guard for a suppression behavior; values that happen to be nonzero will
flip the suppression on for unrelated specializations. No observed
miscompile in this session, but the load is undefined and the side it
falls on changes between runs (`load of value 40` / `load of value 152`
on back-to-back invocations).

## Symptom

Almost every spice test invocation (`tur test`, `tur run`, `tur check`)
prints, before the normal output:

    src/compiler/emit_module.c:3170:40: runtime error: load of value 40,
    which is not a valid value for type 'bool'
    SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior
    src/compiler/emit_module.c:3170:40

The value varies (observed: 40, 152) -- classic uninitialized read.

## Site

`src/compiler/emit_module.c:3170`:

```c
bool saved_suppress = ctx->abi_scan_suppress_construct_byvalue;
if (e->as.call_.fn_binding &&
    e->as.call_.fn_binding->is_construct_template) {
    /* ... */
}
```

`ctx->abi_scan_suppress_construct_byvalue` is being read before it has
been initialized at the call-context level. It is later set to control
the "nested-construct-byvalue (Gap #5)" suppression -- so a garbage read
on the first scan can flip the suppression on or off for unrelated work.

## Repro

Any spice test against a recently-built debug `tur` will trip it on the
first emit pass. Smallest repro on hand:

    cd turmeric-spices/spices/c-dsl
    /path/to/turmeric/build/tur check tests/c-dsl/core_test.tur

(The file's contents do not matter; the trip is in the compiler.)

## Fix direction

Initialize `abi_scan_suppress_construct_byvalue` to `false` at
context-construction time (wherever `EmitCtx` -- or whatever the struct
name is -- is allocated). A `calloc`/`memset` of the parent struct, or
an explicit `ctx->abi_scan_suppress_construct_byvalue = false;` on
construction, both fix it.

The field appears to be set elsewhere as a saved/restored guard around
nested-construct scans (line 3170's `saved_suppress` looks like a save).
Whatever code path enters the scan for the first time today is doing so
before any prior assignment, so the initial value is whatever the
allocator gave back.
