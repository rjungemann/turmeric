---
status: open
severity: medium
discovered: 2026-07-31
area: codegen / monomorph selection (ABI specialization)
---

# Both monomorphs of `vec-empty-like__` call the `int`-element `vec-new` spec

**Severity: medium.** A spec clone monomorphized for one element type calls the
callee monomorph of a *different* element type. Runtime-benign in the one
fixture that surfaces it, purely by luck; the selection mechanism is not.

Split out of
[`macos-int-conversion-carrier-pointer-straddles`](../archive/macos-int-conversion-carrier-pointer-straddles.md)
on 2026-08-01, when the four straddles that report owned were fixed and this --
which was never a straddle -- was all that remained. It was found during that
report's Windows/gcc-16 sweep, where gcc's promoted
`-Wincompatible-pointer-types` turns it into a build failure;
`data-literal-nested` had been listed there as "fixed" and was not.

## Repro -- no build required

```sh
./build/tur emit-c tests/fixtures/data-literal-nested/input.tur > /tmp/dln.c
grep -o 'vec_new__spec__[A-Za-z0-9_]*' /tmp/dln.c | sort -u
# vec_new__spec__tur_adt_Vec__int__          <- the ONLY one
```

The `Map`-element monomorph calls the `int`-element monomorph:

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
(`stdlib/vec.tur:433`) call the `int` one.

Re-verified 2026-08-01 on arm64 macOS (Apple clang 21, macOS 27) at
`54fef281d` + the straddle fixes. Unchanged.

## Toolchain visibility

| Toolchain | Result |
| --- | --- |
| Apple clang 21 (macOS 27) | `warning: incompatible pointer types returning 'tur_adt_Vec__int *' ... from a function with result type 'tur_adt_Vec__Map__sym__int *'` -- fixture builds and passes |
| gcc 16.1.0 (MSYS2/UCRT64) | **error** -- fixture fails to build |
| gcc on the CI Linux leg | warning |

clang 15+ promoted `-Wint-conversion` to an error but **not**
`-Wincompatible-pointer-types`; gcc 14 promoted both. That asymmetry is the
whole reason this hid behind the straddle report: on macOS the straddles were
hard errors and this was a warning, so it never showed up in a failing-fixture
list. The macOS warning half was inferred in the original report and is now
measured (see the table above).

## Do not "fix" this with a cast

A cast at the return site would make the diagnostic go away while leaving the
call routed to the wrong callee. It is runtime-benign *here* only because
`vec_new`'s body just mallocs `{data,len,cap}` and is element-agnostic.

## Two candidate sites, not yet distinguished by reading alone

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

## Not a regression

`849731d85` (the `ret_ct` recording change) did fix a real int/pointer straddle
in this fixture; what it left is this pointer/pointer mismatch. Latent and
clang-invisible, likely predating both `849731d85` and the `66c3bb7c4` merge.

## Fix directions

1. Instrument which of the two candidate sites drops the `A` binding -- dump the
   active spec's bindings at the `vec-empty-like__` clone's `(vec-new)` call.
2. If it is the G7 ascription override, the substitution needs to run *through*
   `result_type_override` rather than being skipped by it.
3. Validate against a promoting toolchain (gcc >= 14), not against Linux CI --
   `tests/run.sh` on the CI Linux leg is green on this fixture today.
