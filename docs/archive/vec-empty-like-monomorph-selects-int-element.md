---
status: resolved
severity: medium
discovered: 2026-07-31
resolved: 2026-08-01
area: codegen / monomorph selection (ABI specialization)
---

# Both monomorphs of `vec-empty-like__` call the `int`-element `vec-new` spec

**RESOLVED 2026-08-01.** One-line fix in the ABI-spec family recovery
(`emit_module.c`): it consulted `type_has_concrete_codegen_layout` alone, which
returns false for **every** `TY_APP` *by design*, so it declined any
ADT-application element and synthesized no tyvar binding for it.

Neither of the two candidate sites this report named was the cause. See
[Root cause](#root-cause-2026-08-01-neither-candidate-was-right).

Split out of
[`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md)
on 2026-08-01, when the four straddles that report owned were fixed and this --
which was never a straddle -- was all that remained.

## Repro (historical)

```sh
./build/tur emit-c tests/fixtures/data-literal-nested/input.tur > /tmp/dln.c
grep -o 'vec_new__spec__[A-Za-z0-9_]*' /tmp/dln.c | sort -u
# vec_new__spec__tur_adt_Vec__int__          <- the ONLY one
```

The `Map`-element monomorph called the `int`-element monomorph:

```c
static tur_adt_Vec__Map__sym__int *
vec_empty_like____spec__tur_adt_Vec__Map__sym__int___tur_adt_Map__sym__int__(
        tur_adt_Map__sym__int * witness) {
    tur_adt_Vec__int * __ps_257 = (vec_new__spec__tur_adt_Vec__int__());
    if (tur_panicking) return ((tur_adt_Vec__Map__sym__int *)0);
    return __ps_257;
}
```

`vec_new__spec__tur_adt_Vec__Map__sym__int__` was never interned, never
forward-declared, never emitted -- **both** monomorphs of `vec-empty-like__`
(`stdlib/vec.tur:433`) called the `int` one.

## Toolchain visibility

| Toolchain | Result |
| --- | --- |
| Apple clang 21 (macOS 27) | `warning: incompatible pointer types returning 'tur_adt_Vec__int *' ... from a function with result type 'tur_adt_Vec__Map__sym__int *'` -- fixture built and passed |
| gcc 16.1.0 (MSYS2/UCRT64) | **error** -- fixture failed to build |
| gcc on the CI Linux leg | warning |

clang 15+ promoted `-Wint-conversion` to an error but **not**
`-Wincompatible-pointer-types`; gcc 14 promoted both. That asymmetry is why this
hid behind the straddle report: on macOS the straddles were hard errors and this
was a warning, so it never appeared in a failing-fixture list.

## Root cause (2026-08-01) -- neither candidate was right

Found by instrumenting `emit_abi_register_call`. Both of this report's original
candidates (`emit_module.c:4396-4460`, the G7 ascription override; and
`emit_core.c:2551-2559`, the cross-spec fallback) are innocent -- the first is
the `#{Construct}`-template path and `vec-new` is an ordinary defn.

What actually happens. `(vec-new)` is a **zero-argument, return-only-polymorphic**
call, so elaboration records no `abi_bindings` for it -- there is no argument to
infer `A` from. `emit_abi_register_call` then has `n_bindings == 0` and bails at
a gate that only lets `#{Construct}` templates through. The one thing that saves
such a call is the *family recovery* just above it: walk the active spec's result
type and the callee's declared result to their head ADT + element args, and
synthesize `{A -> elem}` when the heads match.

That recovery gates each element on `type_has_concrete_codegen_layout`. Measured,
with both specs reaching it with matching heads (`Vec`) and arity 1:

| active spec | recovered element `ae1[0]` | `type_has_concrete_codegen_layout` | outcome |
| --- | --- | --- | --- |
| `vec_empty_like____spec__tur_adt_Vec__int___int64_t` | `int64_t` | **true** | `{A -> int}` synthesized; `vec_new__spec__tur_adt_Vec__int__` interned |
| `vec_empty_like____spec__tur_adt_Vec__Map__sym__int___...` | `tur_adt_Map__sym__int *` | **false** | no binding; falls through the `n_bindings == 0` gate; **no spec interned** |

`type_has_concrete_codegen_layout` returns false for a `TY_APP` **deliberately**,
and its own comment says why and names the companion predicate:

```c
case TY_APP:
    /* structdef-retirement DS-D: a struct-headed TY_APP can never form ...
     * Concrete parametric ADTs are recognised separately by
     * type_app_is_concrete_adt. */
    return false;
```

So the recovery was asking the wrong question for exactly the inputs it exists to
handle. The `int` clone worked only because `int` is not a `TY_APP`; every
ADT-application element -- `(Map sym int)`, `(Vec int)`, any parametric monomorph
-- was silently declined.

### Fix

Accept an element that satisfies **either** predicate:

```c
(type_has_concrete_codegen_layout(&ae1[k]) ||
 (ae1[k].kind == TY_APP && type_app_is_concrete_adt(&ae1[k])))
```

The either/or pairing is the established idiom for this question; compare
`field_read_emits_byvalue_aggregate` in `emit_expr.c`, which spells the same
disjunction.

Result: `vec_new__spec__tur_adt_Vec__Map__sym__int__` is now interned, emitted,
and called by the `Map` clone -- and the `-Wincompatible-pointer-types` warning
is gone, not cast away.

### Why a cast would have been the wrong fix

The original report said so and was right: a cast at the return site would have
silenced the diagnostic while leaving the call routed to the wrong callee. It was
runtime-benign *only* because `vec_new`'s body mallocs `{data,len,cap}` and is
element-agnostic. The selection mechanism was not, and the same recovery path
serves every return-only-polymorphic generic.

## Regression coverage

`tests/fixtures/data-literal-nested` gains an `expected.c` codegen snapshot,
which pins `vec_new__spec__tur_adt_Vec__Map__sym__int__` by name. **Verified to
catch a revert**: with the disjunction removed, `tests/run.sh` reports
`FAIL data-literal-nested -- codegen mismatch`.

A snapshot rather than a runtime assertion because the miscompile is
runtime-benign in this fixture, and rather than a smaller purpose-built fixture
because the shape needs `:heap` parametric ADTs to reproduce -- a plain `defdata`
container collapses its spec result to the int64 carrier one step earlier, before
family recovery is ever consulted.

## Verification (arm64 macOS, Apple clang 21, macOS 27)

| Check | Result |
| --- | --- |
| `data-literal-nested`, `tur build` | 0 errors, **0 warnings** (was 1 warning) |
| `bash tests/run.sh` | `2502 passed, 0 failed` |
| `tests/run-jit.sh`, Debug+JIT+ASan | `2416 passed, 0 failed, 48 skipped` |
| `tur run regen-snapshots -- --check` | up to date -- **zero** drift on the other 140 |
| `check-monomorph-name-collision.sh`, `check-typekind-mangle-exhaustive.sh` | all passed |

Zero drift on the other snapshots is the notable one: the disjunction only
changes behavior where the old predicate returned false for a concrete ADT app,
which previously meant "give up", so nothing that already worked moved.
