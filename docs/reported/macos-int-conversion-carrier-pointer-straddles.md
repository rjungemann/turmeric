# Residual carrier<->pointer straddles are hard errors on modern toolchains

**Severity: medium (build breakage on any sufficiently new toolchain, five
fixtures).** The affected programs fail to compile at the `cc` step. They build
fine on the Linux CI leg, so the suite is green there and this is invisible to
the release gate.

Found 2026-07-30 on arm64 macOS (Apple clang 21.0.0, macOS 27.0) while checking
`claude/j0-jit-engine-plan-znqibo` for regressions after
`66c3bb7c4` (merge of `origin/main` into the JIT engine branch).

**Confirmed 2026-07-31 on Windows** (MSYS2/UCRT64, gcc 16.1.0, main at
`f630230e5`) during a Windows-support sweep. This is NOT a macOS/clang quirk:
it is "any toolchain new enough to promote the diagnostic." See the Windows
confirmation section at the end -- it also shows one case listed below as fixed
is still failing.

## Why Linux CI cannot catch this

`-Wint-conversion` (assigning/passing an `int64_t` where a pointer is expected,
or the reverse) has been an **error by default** since clang 15, and Apple clang
21 enforces it. GCC promoted `-Wint-conversion` and
`-Wincompatible-pointer-types` to errors in GCC 14. The gcc on the CI Linux leg
is older than that, so every one of these emits a warning there and compiles
clean. This class has bitten before -- see the "Apple clang 17
`-Werror=int-conversion`" entry in `CHANGELOG.md` -- and it will keep recurring
until a leg that enforces them builds fixtures.

`.github/workflows/ci.yml:30` does run a `macos-latest` leg, so a real fix here
is to let that leg fail on these rather than to chase them by hand.

Note that the two `-Wno-error=` downgrades that used to hide this were
deliberately removed (`src/main.c:5231-5241`), on the stated grounds that
"every straddle is now bridged at emit time" and "the whole fixture tree emits 0
-Wint-conversion / -Wincompatible-pointer-types hard errors." That claim does
not hold on gcc 16.1.0. Do not re-add the downgrades; the removal is correct and
these are the genuine remaining straddles it exposed.

## Status

Seven fixtures failed on a clean macOS build of `66c3bb7c4`. **Three are fixed**
by the `ret_ct` recording change in `src/compiler/emit_expr.c` that accompanies
this report (`data-literal-nested`, `hkt-inline-c-heap-result-type`,
`vec-push-heap-struct-element-carrier-cast`). The remaining **four are open**
and are recorded below.

Of the seven, five were **regressions** introduced by the merge (verified by
building the pre-merge tip `1edcbd3c9` and compiling each fixture); two
(`defalias-composite`, `fn-value-matrix-ok-rows`) already failed before it.

## Repro

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON -DTUR_DEBUG_SANITIZE=OFF
cmake --build build -j
./build/tur build tests/fixtures/hkt-ap-fn-in-container/input.tur
```

(`-DTUR_DEBUG_SANITIZE=OFF` avoids the unrelated macOS ASan startup deadlock
documented in `CLAUDE.md`. `TUR_JIT` is not required to reproduce.)

## Open case A -- monomorphized ctor carrier field (regression)

`conv-defstruct-option-fn-element`, `hkt-ap-fn-in-container`.

```
error: incompatible pointer to integer conversion passing 'void *'
       to parameter of type 'int64_t'
  tur_adt_Option__fn1_float__float __ps_213 =
      (ctor_Option__fn1_float__float(true, x));
```

An ABI-specialized clone spells its monomorphized fn-typed parameter `void *`
(`some__spec__...Option__fn1_float__float_void__(void * x)`), but the ctor slot
it feeds is the int64 carrier.

Three things conspire, all in the ctor-argument block of
`emit_value_dispatch` (`src/compiler/emit_expr.c:5378-5514`):

1. `resolved_arg_type` (`:5383`) is `emit_resolve_type(ctx, arg->type)`, which
   inside a spec clone yields the **erased `TY_TYVAR`**, so `is_ptr_like`
   (`:5385`) under-fires. Resolving the var through the active spec with
   `emit_var_spec_arg_type` (as the let-binding bridge at `:1878` does) fixes
   this half and correctly yields `TY_FN`.
2. The pointer->int64 cast at `:5396` is gated on `!suffix`, i.e. it skips every
   **monomorphized** ctor -- which is exactly where this occurs.
3. The comment at `:5469-5471` asserts a tyvar carrier field is always the
   `int64_t` slot. **That premise is false across monomorphs:**
   `ctor_Option__fn1_float__float` takes `int64_t` while
   `ctor_Option__fn1_int__int` takes `void *` for the same field. So a blanket
   int64 cast fixes the float monomorph and breaks the int one.

Fix direction: route the carrier-field case through the block at `:5472`, whose
cast target already follows the field's actual C type via
`adt_field_type_for_app`. Attempted; `adt_field_type_for_app` did **not**
resolve to `void *` for `Option__fn1_int__int` (it fell through to the
`int64_t` default at `:5502-5506`), so that resolution needs investigating
before this lands. Reverted rather than shipped half-correct.

## Open case B -- return-site straddle (pre-existing)

`defalias-composite`:

```
error: incompatible integer to pointer conversion returning 'int64_t'
       from a function with result type 'tur_adt_Cons__int *'
  return cons(..., ...);
```

`fn-value-matrix-ok-rows` is the same shape in the other direction (`return v;`
and `return __env___env_1376->c;` from a `void *`-returning function).

The function's declared result was upgraded to a typed pointer, but the tail
expression is still the int64 carrier and no bridge is applied at the return
site. The binder-init path has this bridge
(`src/compiler/emit_expr.c:1989-1992`); the return path needs the equivalent.

## Fixed here, for reference

The temp hoist at `src/compiler/emit_expr.c:3160` declares `__ps_N` with
`ret_ct` -- read from the callee's own forward declaration, and by construction
what `__auto_type` would deduce. The side-table recording immediately below it
then **ignored** `ret_ct` and re-derived the type from the source type via
`emit_binding_repr_c_name`. Since increment 4 stage 3 those disagree: `(HBox
int)` c-names to `tur_adt_HBox__int *`, so a temp DECLARED `int64_t` was
RECORDED as a pointer. Every downstream straddle bridge keys on the recorded
representation, so all of them concluded "pointer -> pointer, nothing to do"
and emitted `tur_adt_HBox__int * m = __ps_N;`.

Recording `ret_ct` when it is known restores the agreement the bridges assume.
It is value-preserving; the only codegen movement in the whole corpus was
`van-laarhoven-lens-wide-functor-show`, where two now-redundant
`(const char *)(intptr_t)` casts dropped out (snapshot regenerated).

## Windows / gcc 16 confirmation (2026-07-31)

Sweep of `TUR=./build-win/tur.exe bash tests/run.sh` on main at `f630230e5`
(MSYS2/UCRT64, gcc 16.1.0): `summary: 2445 passed, 54 failed`. Five of the 54
are this report's class, and they line up with the open cases above:

```
conv-defstruct-option-fn-element   error: passing argument 2 of
    'ctor_Option__fn1_float__float' makes integer from pointer without a cast
hkt-ap-fn-in-container             (same)                      <- open case A
defalias-composite                 error: returning 'int64_t' from a function
    with return type 'tur_adt_Cons__int *'
fn-value-matrix-ok-rows            error: returning 'int64_t' from a function
    with return type 'void *'                                  <- open case B
data-literal-nested                error: returning 'tur_adt_Vec__int *' from a
    function with return type 'tur_adt_Vec__Map__sym__int *'   <- SEE BELOW
```

Two things this adds:

1. **The class is not macOS-specific.** Same defects, different vendor, same
   cause (diagnostic promoted to an error). Any fix should be validated against
   a promoting toolchain, not against Linux CI.

2. **`data-literal-nested` is listed above as FIXED by the `ret_ct` change, but
   it still fails.** Its symptom on gcc is also a different shape from the other
   four: not an int<->pointer straddle at all, but a *pointer-to-pointer* type
   mismatch between two different monomorphs (`Vec__int *` vs
   `Vec__Map__sym__int *`), i.e. `-Wincompatible-pointer-types` rather than
   `-Wint-conversion`. That is consistent with the `ret_ct` change having fixed
   the straddle half while leaving a wrong monomorph selection underneath, which
   clang would not have flagged once the int/pointer confusion went away.
   Worth re-opening specifically: a wrong element-type monomorph reaching codegen
   is a more serious bug than a missing cast, and the fix note above may have
   closed it prematurely.

Reproduce without Windows by building with gcc >= 14 and compiling any of the
five fixtures.
