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

2. **`data-literal-nested` is listed above as FIXED, but it still fails -- and it
   is not this report's bug at all.** See the next section; it needs re-opening
   separately.

Reproduce without Windows by building with gcc >= 14 and compiling any of the
five fixtures.

## `data-literal-nested` is a WRONG-MONOMORPH bug, not a straddle (2026-07-31)

Confirmed by reading `tur emit-c` output directly, no build required:

```sh
./build/tur emit-c tests/fixtures/data-literal-nested/input.tur > /tmp/dln.c
grep -o 'vec_new__spec__[A-Za-z0-9_]*' /tmp/dln.c | sort -u
# vec_new__spec__tur_adt_Vec__int__          <- the ONLY one
```

The emitted `Map`-element monomorph calls the `int`-element monomorph:

```c
static tur_adt_Vec__Map__sym__int *
vec_empty_like____spec__tur_adt_Vec__Map__sym__int___tur_adt_Map__sym__int__(
        tur_adt_Map__sym__int * witness) {
    tur_adt_Vec__int * __ps_257 = (vec_new__spec__tur_adt_Vec__int__());
    if (tur_panicking) return ((tur_adt_Vec__Map__sym__int *)0);
    return __ps_257;
}
```

`vec_new__spec__tur_adt_Vec__Map__sym__int__` is never interned, never
forward-declared, never emitted -- **both** monomorphs of `vec-empty-like__`
(`stdlib/vec.tur:433`) call the `int` one. A cast at the return site would
paper over a mis-selected callee. It is runtime-benign *here* only by luck
(`vec_new`'s body just mallocs `{data,len,cap}` and is element-agnostic); the
selection mechanism is not.

Two candidate sites, not yet distinguished by reading alone:

- `src/compiler/emit_module.c:4396-4460` -- the body is `(:: (vec-new) (Vec A))`,
  which takes the G7 ascription-override branch and passes the raw,
  unsubstituted `(Vec A)` as `result_type_override`. Every subsequent recovery
  path is gated `if (!result_type_override && ...)` (`:3181-3190`), so if the
  active spec's bindings carry no `A`, `result_type` stays `(Vec A)`, whose
  `type_c_name` is `tur_adt_Vec__int` (tyvar -> int64 -> int). This explains the
  *absence* of the Map spec and is the likelier of the two.
- `src/compiler/emit_core.c:2551-2559` -- the cross-spec fallback deliberately
  reuses an entry recorded under a *different* outer spec when none exists for
  the active one. A general "route to whatever monomorph was recorded first"
  hazard that fits the symptom.

**Not a regression.** The `ret_ct` change (`849731d85`) did fix a real
int/pointer straddle in this fixture; what it left is the pointer/pointer
mismatch. clang 15+ promoted `-Wint-conversion` to an error but not
`-Wincompatible-pointer-types`, whereas gcc 14 promoted both -- so the residual
was a warning on macOS and the fixture went green. It has been latent and
clang-invisible, likely predating both `849731d85` and the `66c3bb7c4` merge.
(The clang-still-warns half of that story is inferred, not measured -- worth one
check on a macOS box.)

## Corrections to the line refs above (2026-07-31)

`src/compiler/emit_expr.c` is unchanged since this report was written, but three
refs still do not match content -- they appear to have been written against the
uncommitted attempted-fix state:

| Reported | Actual |
| --- | --- |
| `:5469-5471` (tyvar-carrier comment) | `:5430-5439`; the blanket int64 assumption is the cast at `:5399-5406` |
| `:5472` (`adt_field_type_for_app` block) | `:5440-5482` |
| `:5502-5506` ("int64_t default") | `:5473-5474` |

Open case B also needs a correction: the return-site bridge is **not** missing.
It exists at `src/compiler/emit_fns.c:4264-4281`, and the two fixtures fall
through two specific gaps in its guard:

- `fn-value-matrix-ok-rows`: `emit_fns.c:4266` explicitly excludes `void *`
  return types (`strcmp(ret_ctype, "void *") != 0`). The reverse direction
  (`void *` value -> `int64_t` return) is handled at `:4282-4297`; the forward
  direction is not.
- `defalias-composite`: the value is a bare carrier-ctor *call* (`cons(...)`),
  which is neither `(int64_t)`-prefixed nor a bare recorded ident, so neither
  disjunct at `:4268-4271` matches.

## Why the open-case-A fix fell through (2026-07-31)

`adt_field_type_for_app` (`src/compiler/types.c:1379-1387`) does resolve
correctly -- it faithfully returns the substituted `TY_FN`. The loss is in the
consumer at `emit_expr.c:5470-5474`, which calls `type_c_name(fty)` and accepts
the result only if it ends in `*`.

`type_c_name` for `TY_FN` (`types.c:3147-3175`) branches on `cfnptr`/`boxed`,
which carry no type identity: the same fn type yields `void *` or `int64_t`
purely on the `boxed` flag. And `append_type_mangle` for `TY_FN`
(`types.c:907-915`) mangles only arity + arg kinds + result kind -- **not**
`boxed`/`cfnptr` -- and `type_eq` compares the same triple. That is exactly the
collision class the warning comment at `types.c:880-905` describes. So the two
`Option` monomorphs' payload slots genuinely differ (`void *` vs `int64_t`,
confirmed in the emitted forward declarations) while racing for one typedef.

Two fix levels:

- *Low risk:* stop re-deriving. The ctor's real param C type is known at
  registration (`types.c:1610`, `1648`, `1705`); record it in the signature side
  table the way `emit_sig_record_ret_ctype` already records ctor return types,
  and have `emit_expr.c:5470-5474` look it up. Calling `adt_field_c_type` from
  the call site does NOT help -- it bottoms out in the same `type_c_name`
  (`types.c:1348`) and returns `int64_t` again.
- *Proper:* put `boxed`/`cfnptr` into the `TY_FN` mangle and `type_eq`, so a
  boxed and an unboxed `(fn [int] int)` get distinct names rather than racing.
  Higher blast radius (corpus-wide symbol renames, large snapshot churn) but it
  removes the latent silent-layout merge, which is the actual defect.
