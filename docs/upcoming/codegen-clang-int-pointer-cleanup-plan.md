# Plan: Clean int<->pointer codegen so generated C builds on modern Clang

> **Status:** Draft Plan
> **Last Updated:** 2026-05-29
> **Type:** Compiler / codegen correctness
> **Related:** workaround in `tests/run.sh` (Clang-gated `-Wno-error=...`); see
> also the carrier-ABI work in KB-021 / KB-031.

---

## Overview

The Turmeric backend emits C that carries pointers through the `int64_t`
"carrier" ABI (heap handles, `rc`/`weak`, existential payloads, dictionary
method slots, ...).  In a handful of emit paths the generated C performs an
`int64_t <-> pointer` transition *without* the usual `(int64_t)(intptr_t)expr`
/ `(T *)(intptr_t)expr` round-trip, or declares a dictionary method-slot
function-pointer type that does not match the instance method's real C
signature.

Under **GCC** these are warnings (`-Wint-conversion`,
`-Wincompatible-pointer-types`) and the code compiles and runs correctly on
LP64 targets.  Under **modern Clang (macOS, Xcode 15+)** the same diagnostics
are **default-on errors**, so `cc` fails and the build aborts.

A CI-level workaround is already in place: `tests/run.sh` adds
`-Wno-error=int-conversion -Wno-error=incompatible-function-pointer-types` to
`TUR_CC_FLAGS`, gated to Clang.  That keeps CI green, but a standalone
`tur build` / `tur run` on macOS (with `TUR_CC_FLAGS` unset) still hits the
errors, because the compiler's own default flags (`src/main.c`) do not carry
the downgrade.

This plan tracks the **proper** fix: emit clean C so neither the downgrade nor
a macOS-specific build flag is needed.

## Symptom

Five fixtures fail to compile under Clang (and warn under GCC).  They build and
pass under GCC, and pre-date any recent change (reproduced identically on
`origin/main`):

| Fixture | Clang diagnostic | Generated C (essence) |
|---------|------------------|-----------------------|
| `derive-show-nested` | incompatible-function-pointer-types | `.show = __inst_Show_show_Triple` where the slot type is `const char *(*)(Triple)` but the fn is `const char *(const Triple *)` |
| `exg4-pack-into-struct` | int-conversion | `(Box){.payload = (tur_exists_t)(__t2)}` -- `payload` is `int64_t`, RHS is `void *` |
| `exg4-pack-into-struct-via-let` | int-conversion | `(Box){.payload = p}` -- `payload` is `int64_t`, `p` is `void *` |
| `exg5-exists-cycle` | int-conversion | `return (int64_t)(intptr_t)NULL;` from a function whose result type is `void *` |
| `exg5-rc-in-exists` | int-conversion | `__inst_Show_show_T(v)` passes an `int64_t` to a parameter of type `RcControlBlock *` |

## Root cause

Two distinct families:

### Family A -- missing carrier<->pointer cast

An emit path produces an `int64_t` carrier where the C declaration is a pointer
(or vice versa) without the established `(int64_t)(intptr_t)e` /
`(T *)(intptr_t)e` bridge.  Affected paths seen above:

- **Existential pack** (`exg4-*`): writing a carrier value into a concrete
  struct field whose C type is `int64_t`, while the value expression has type
  `void *` / `tur_exists_t` (or the reverse).
- **Existential / pointer return** (`exg5-exists-cycle`): returning an
  `int64_t` expression from a function whose C result type is `void *` (the
  `NULL` carrier case).
- **rc passed to a typed parameter** (`exg5-rc-in-exists`): passing an
  `int64_t` carrier to a parameter declared `RcControlBlock *` (the `rc`-typed
  Show instance method).

On LP64 these are value-preserving, which is why GCC's warning is harmless at
runtime -- but the emitter should still bridge explicitly.

### Family B -- stale dictionary method-slot typedef

For a struct large enough to be passed by pointer (Phase D `pass_by_ptr`,
sum-of-fields > 16 bytes; see `StructDef.pass_by_ptr` in
`src/compiler/types.h`), the instance method is emitted with a by-pointer
signature (`__inst_Show_show_Triple(const Triple *)`) and the *direct* call
sites correctly pass `&value`.  But the typeclass dictionary's method-slot
function-pointer **typedef** is still generated as by-value
(`const char *(*)(Triple)`).  Assigning the by-pointer function into the
by-value slot is the `-Wincompatible-function-pointer-types` error.  It is
benign today only because dispatch goes through the direct call, not the slot,
for these cases -- but it is a latent ABI mismatch, not just a warning.

## Proposed fix

### Family A

Route every carrier<->pointer transition through the existing bridge helper so
the cast is explicit and the C types line up.  Concretely, in the emit layer
(`src/compiler/emit_expr.c`, `emit_module.c`):

- struct-field initialisation of a carrier-ABI field from a pointer-typed
  value (and vice versa),
- `return` of a carrier where the function's C result type is a pointer (the
  `NULL`/exists case),
- argument passing where the parameter's C type is a concrete pointer
  (`RcControlBlock *`, `tur_exists_t`, ...) but the argument is a carrier,

should emit `(int64_t)(intptr_t)e` or `(T *)(intptr_t)e` rather than a bare
assignment/return/pass.  The KB-021 carrier-bridge plumbing
(`emit_carrier_bridge`, `expr_emits_byvalue_carrier_abi`) is the natural place
to hook this -- extend the "needs a bridge" predicate to cover these
field/return/arg positions.

### Family B

Make the dictionary method-slot function-pointer typedef reflect the instance
method's *actual* C signature, including `pass_by_ptr`.  Where the slot type is
synthesised (dict struct emission for `definstance`), consult the same
`pass_by_ptr` decision used to emit the method definition so a by-pointer
method gets a `const T *` slot parameter, not by-value `T`.  Alternatively,
emit dictionary method slots uniformly on the carrier ABI (`int64_t` params)
and bridge at the call site, matching the parametric path that already works
(this is the direction KB-021 took for struct-typed dispatch).

## Validation

- `tur emit-c` each of the five fixtures and compile with
  `clang -Werror=int-conversion -Werror=incompatible-function-pointer-types`
  (this reproduces the macOS failure on any platform with clang installed).
- Confirm all five build clean, then remove the Clang-gated `-Wno-error`
  downgrade from `tests/run.sh` and confirm the full suite is still green under
  both GCC and Clang.
- Re-check codegen snapshots (`expected.c`) for any fixture whose emitted C
  changes, and regenerate as needed.

## Affected files

- `src/compiler/emit_expr.c`, `src/compiler/emit_module.c` -- carrier<->pointer
  bridges (Family A) and dictionary slot typedefs (Family B).
- `src/compiler/types.h` -- `StructDef.pass_by_ptr` (referenced, not changed).
- `tests/run.sh` -- remove the Clang-gated `-Wno-error` downgrade once the
  codegen is clean.
- `src/main.c` -- optional: once the emitter is clean, the default `cc` flags
  need no change, so a standalone macOS `tur build` works without
  `TUR_CC_FLAGS`.

## Effort

Medium.  Family A is a localized set of cast insertions in the emit layer;
Family B is a contained dictionary-slot typedef fix (or a switch to uniform
carrier-ABI slots).  Both need careful `expected.c` snapshot review.

## Interim state

The Clang-gated `-Wno-error` downgrade in `tests/run.sh` keeps CI green on
macOS today.  This plan removes the need for it and unblocks standalone
`tur build` on modern Clang.
