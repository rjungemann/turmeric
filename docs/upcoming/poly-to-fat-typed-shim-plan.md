---
title: Typed poly-to-fat Shim Plan
category: Planning
description: Close the remaining int64-only fat-shim gap in the Typed Closure Invocation ABI -- generalise __tur_poly_to_fat1 (the EX_POLY_TO_FAT boxing thunk for typeclass-method closures) to speak the method's declared C types, so a non-int64 typeclass method handed to a ^fat sink no longer carries a silent register-class ABI mismatch.
---

# Typed poly-to-fat Shim -- Plan

> **Status:** Implemented (capturing-closure path); follow-up reported
> **Last Updated:** 2026-06-04
> **Type:** compiler ABI -- closure invocation
> **Sibling plans:**
> - [closure-typed-invocation-abi-plan.md](closure-typed-invocation-abi-plan.md) -- the parent plan; this finishes the one shim family it left on the int64 carrier
> - [stdlib-type-erasure-cleanup-plan.md](stdlib-type-erasure-cleanup-plan.md) -- the "dict-field-as-`void *`" / method-returning-method erasure this protects against

---

## Problem

The Typed Closure Invocation ABI work generalised `EX_FN_TO_FAT`'s shim
(`__tur_fatshim<arity>`) to a per-signature `__tur_fatshim_<R>_<A_i...>`
keyed off the closure's declared C types. One shim family was left on the
int64 carrier: **`__tur_poly_to_fat1`**, the thunk `EX_POLY_TO_FAT` uses
to box a typeclass-method closure (`tur_poly_fn_t`) into the fat-closure
protocol.

```c
/* preamble, emit_module.c */
typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;

static int64_t __tur_poly_to_fat1(void *__e, int64_t a0) {
    int64_t *__b = (int64_t *)__e;
    return ((int64_t (*)(void *, int64_t))(intptr_t)__b[1])
               ((void *)(intptr_t)__b[2], a0);
}
```

`EX_POLY_TO_FAT` (`elab_call.c:2392`, lowered in `emit_expr.c`) builds a
3-slot box `{ __tur_poly_to_fat1, fn, env }` and hands back a `TY_PTR_VOID`
handle -- structurally identical to `EX_FN_TO_FAT`'s 2-slot box. The
handle is later invoked at a call site that emits the **typed-thunk cast**
(`emit_expr.c:1750`, the `TY_PTR_VOID` fat-dispatch path), which casts
slot 0 to `tur_thunk_<R>_<A0>_t = R (*)(void *, A0)` whenever
`use_typed_thunk_abi` holds for the call's result/argument types.

For an int64-carrier method that cast is `int64_t (*)(void *, int64_t)`,
which is exactly what `__tur_poly_to_fat1` is -- so today's code works.
The moment a typeclass instance method with a **non-int64** signature
(returning `:float`, or taking a `:float`) is handed to a `^fat`
parameter and invoked through the typed-thunk path, slot 0 is cast to,
say, `double (*)(void *, double)` while the actual function in slot 0 is
`int64_t (*)(void *, int64_t)`. On every mainstream ABI a `double`
argument/return lives in a different register class (xmm0) than the
int64 carrier (rax/rdi), so the values are silently read from the wrong
registers.

This is **not** a crash. It is a wrong-number bug that surfaces far from
its cause, only for the specific instance/method combination that trips
it. That is a time bomb, not a "wait for a consumer" item: the parent
plan's own `parsec-tutorial` regen showed the analogous `EX_FN_TO_FAT`
case was already silently mismatched for pointer returns (masked only
because pointers share the int64 register class). The `:float` case has
no such cover.

### Why it has not fired yet

The typeclass-method closures that reach `EX_POLY_TO_FAT` today
(Functor/Applicative/Monad instances over int64-sized containers, schema
combinators) all happen to carry int64 or pointer payloads, which are
register-class-compatible with the int64 carrier. Nothing structural
prevents a `:float`-carrying instance method from reaching this path; it
simply has not been written yet.

## Goal

`EX_POLY_TO_FAT` boxes a typeclass-method closure with a slot-0 shim whose
ABI matches the typed-thunk cast the call site will apply, exactly as
`EX_FN_TO_FAT` now does. A non-int64 poly method round-trips correctly;
the int64 case keeps using the preamble `__tur_poly_to_fat1` so existing
fixtures stay churn-free.

## Non-goals

- Changing `tur_poly_fn_t`'s storage layout. The `.fn` field stays
  declared `int64_t (*)(void *, int64_t)`; it is a carrier, and the
  producer already stores the method's real (typed) function pointer
  there via a pointer cast that preserves the address. The typed shim
  re-casts slot 1 back to the true `R (*)(void *, A0)` -- the same
  "carrier erases, shim re-types" pattern `EX_FN_TO_FAT` uses.
- Arities beyond unary. `tur_poly_fn_t` is inherently unary
  (`(env, arg) -> result`); a single `(R, A0)` shim family suffices.
- Polymorphic-at-runtime method handles (out of scope in the parent plan
  too).

## Design

### Phase 0 -- defuse first (compile-time guard)

Before the full typed shim lands, convert the silent mismatch into a loud
compile error. At the `EX_POLY_TO_FAT` lowering site, compute the inner
method's `(R, A0)` and check `use_typed_thunk_abi`. If the signature is
non-int64 (the bomb condition) **and** Phase 1's typed shim is not yet
selected, emit a hard `TUR-E` diagnostic ("typeclass-method closure with
a non-int64 signature cannot yet be boxed into a ^fat consumer") instead
of silently emitting the int64 shim.

This is independently mergeable and immediately removes the time bomb: a
program that would have miscompiled now fails to compile with a clear
message. Phase 1 then replaces the guard with real support.

### Phase 1 -- typed poly shim

Add `ensure_typed_poly_to_fat(EmitCtx *ctx, Type result, Type arg)`,
mirroring `ensure_typed_fatshim`:

- Return `NULL` when `!use_typed_thunk_abi(result, &arg, 1)` or when both
  `type_c_name`s are `int64_t` (the preamble `__tur_poly_to_fat1` already
  matches -- churn-free).
- Otherwise dedupe per TU (new `ctx->poly_fatshim_names` list, freed at
  both `emit_program` / `emit_implementation` cleanup sites) and emit:

```c
static R __tur_poly_to_fat1_<R>_<A0>(void *__e, A0 a0) {
    int64_t *__b = (int64_t *)__e;
    return ((R (*)(void *, A0))(intptr_t)__b[1])
               ((void *)(intptr_t)__b[2], a0);
}
```

Name-mangle with the same `append_sanitized_c_token` scheme as the typed
thunk typedefs and `ensure_typed_fatshim`, so the shim name is
deterministic and fixture diffs are reproducible.

`EX_POLY_TO_FAT` selects the typed shim for slot 0 when
`ensure_typed_poly_to_fat` returns non-NULL, else keeps
`__tur_poly_to_fat1`. The 3-slot box layout (`{ shim, fn, env }`) is
unchanged.

### Sourcing `(R, A0)` at the lowering site

The open feasibility question. `EX_POLY_TO_FAT`'s `inner` is the
`is_poly_fn` argument expression (`elab_call.c:2394`); its `inner->type`
must carry the method's true `result_kind` / `arg_kinds[0]` (or full
types) for the shim to be typed correctly. Phase 1 must confirm this:

- If `inner->type` is a `TY_FN` with concrete result/arg kinds, build the
  `(R, A0)` Types via `emit_type_from_kind` (or `*result_full_type` /
  `*arg_full_types[i]` when set), exactly as `EX_FN_TO_FAT` now does.
- If the poly-fn binding has already been erased to a bare
  `tur_poly_fn_t` with int64 kinds at this point, the type must be
  threaded onto `poly_to_fat_` at elaboration time (carry the method's
  `Type` on the `EX_POLY_TO_FAT` node when it is created in
  `elab_call.c`). This is the likely-needed change and the main risk;
  Phase 0's guard will already have proven whether the type is reachable
  here.

### Consistency invariant

The shim's `type_c_name(R)` / `type_c_name(A0)` tokens must be
**identical** to the tokens the call-site typed-thunk cast uses. Both go
through `type_c_name`, so primitives (`double`, `bool`, `const char *`,
`void *`) agree by construction -- the same guarantee that makes
`ensure_typed_fatshim` correct.

## Phasing

1. **Phase 0 (this repo).** Compile-time guard at `EX_POLY_TO_FAT` for
   non-int64 poly signatures. Defuses the bomb. Negative fixture under
   `tests/fixtures/errors/` proving the diagnostic fires.
2. **Phase 1 (this repo).** `ensure_typed_poly_to_fat` + per-signature
   shim emission; `EX_POLY_TO_FAT` selects it; guard removed. Thread the
   method `Type` onto the node if Phase 0 shows it is not already there.
3. **Phase 2 (this repo).** Round-trip fixture: a typeclass instance
   method with a `:float` signature handed to a `^fat` sink and invoked
   through the typed-thunk path. Fixture preamble/shim regen.

## Validation

- **Phase 0**: `bash tests/run.sh` clean; the new negative fixture's
  `expected.diag` matches; no existing fixture regresses (int64 poly
  boxes still compile).
- **Phase 1**: full fixture regen; int64 poly fixtures churn-free
  (still emit `__tur_poly_to_fat1`); the Phase 0 negative fixture flips
  to a passing positive fixture.
- **Phase 2**: the `:float` instance-method round-trip prints the
  expected native value; `grep` confirms slot 0 holds
  `__tur_poly_to_fat1_double_double` (or equivalent), not the int64 shim.
- Whole effort runs under the Debug ASan/LSan build with leak detection
  on (the new dedup list must be freed -- see
  [CLAUDE.md leak policy](../../CLAUDE.md#leak-detection-asanlsan-policy)).

## Risks

- **Type not reachable at the lowering site.** If `inner->type` is
  already erased, Phase 1 must carry the method `Type` on the
  `EX_POLY_TO_FAT` node from `elab_call.c`. Mitigation: Phase 0's guard
  computes the same `(R, A0)` and will fail to find non-int64 types if
  they are unreachable, flagging the need early.
- **Constructing a test.** A `:float`-carrying instance method passed to
  a `^fat` sink may not be expressible with current stdlib; Phase 2 may
  need a purpose-built typeclass + instance in the fixture. Acceptable --
  the fixture is the proof the path exists at all.
- **Carrier-store assumption.** The design assumes the producer stores
  the method's real typed function pointer into `tur_poly_fn_t.fn` via a
  plain pointer cast (address-preserving), so the shim's re-cast recovers
  the ABI. Phase 1 must verify the producer does not insert an int64 ABI
  thunk between the method and the carrier; if it does, that thunk is the
  erasure point and must be typed instead.
- **Fixture churn.** Per the
  [CLAUDE.md fixture rule](../../CLAUDE.md#fixture-snapshots----strict-rule),
  regenerate any snapshot whose poly box changes in the same PR.

## Files this plan will touch

- `src/compiler/emit_module.c` -- `ensure_typed_poly_to_fat`, dedup-list
  init + cleanup in both ctx setups (mirroring `fatshim_names`).
- `src/compiler/emit_internal.h` -- `EmitCtx.poly_fatshim_names` fields +
  the new function declaration.
- `src/compiler/emit_expr.c` -- `EX_POLY_TO_FAT` selects the typed shim.
- `src/compiler/elab_call.c` -- (Phase 0) the guard; (Phase 1, if needed)
  carry the method `Type` onto the `EX_POLY_TO_FAT` node.
- `tests/fixtures/errors/poly-to-fat-non-int64/` -- Phase 0 negative
  fixture (becomes a positive round-trip fixture after Phase 1).
- `tests/fixtures/**/expected.c` -- regenerated where a poly box changes.

## Acceptance checklist

- [x] `ensure_typed_poly_to_fat` lands, keyed by `(R, A0)`, deduped, and
      freed at both cleanup sites (leak detection on).
      (`src/compiler/emit_module.c`, `EmitCtx.poly_fatshim_names`.)
- [x] `EX_POLY_TO_FAT` selects the typed shim for non-int64 signatures
      and keeps `__tur_poly_to_fat1` for the int64 case (churn-free).
      The `(R, A0)` is sourced from the sink's declared `^fat` fn signature
      (`fn_type.arg_full_types[idx]`, a concrete `TY_FN`), threaded onto the
      node as `poly_to_fat_.sink_fn_type` in `elab_call.c`.
- [x] `:float` instance-method round-trip fixture passes; slot 0 carries
      the typed poly shim (`tests/fixtures/poly-to-fat-float-roundtrip/`,
      prints `7`, slot 0 = `__tur_poly_to_fat1_double_double`).
- [x] `bash tests/run.sh` zero `FAIL` (1349 passed).

### Implementation notes / deviations

- **Phase 0 guard subsumed.** Rather than land a compile-time guard first
  and replace it in Phase 1, the typed shim was implemented directly, so
  the silent mismatch is *fixed* (not merely defused) for the path the
  plan targets. No separate negative fixture is needed for that path.
- **`(R, A0)` source.** The plan's "source from `inner->type`" is not
  reachable inside the (signature-erased) generic method body -- the
  method param is erased to `ptr<void>`. The robust source is the **sink's**
  declared `^fat` fn signature, which is exactly the typed-thunk cast the
  sink applies on invocation (the plan's "Consistency invariant"). This is
  threaded onto the `EX_POLY_TO_FAT` node at the box site.
- **Named-fn / non-capturing path (Risk #3, reported).** The typed shim is
  correct when the producer stores the method's real typed thunk into the
  carrier -- true for the *capturing-closure* pass-through. The
  *named-function* / non-capturing path instead routes through
  `make_poly_wrapper`, which forces the wrapper's argument params to
  `int64_t`; that is a second erasure point and miscompiles a non-int64
  argument regardless of slot 0. Documented in
  [docs/reported/poly-wrapper-forces-int64-args-non-int-fat-sink.md](../reported/poly-wrapper-forces-int64-args-non-int-fat-sink.md)
  with repro, root cause, and fix directions.
