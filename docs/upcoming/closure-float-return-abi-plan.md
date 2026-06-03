---
title: Closure :float-Return ABI Plan (Option A)
category: Planning
description: Compiler-level plan to teach Turmeric closures and inline-C invocation paths to return :float, unblocking Phase 0 of the signal-primitives-expansion plan and removing the int64-bit-pattern-of-double idiom from DSP code.
---

# Closure `:float`-Return ABI -- Plan

Companion to [signal-phase-0-spike.md](signal-phase-0-spike.md). This is the
"Option A" path identified there: extend the closure / inline-C invocation
ABI so an anonymous `(fn [t] :float ...)` can actually return a `double` at
runtime. Goal is to unblock Phase 0 of
[signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md)
without re-defining what Phase 0 means.

## Why this is needed

`signal/dsp.tur` SF bodies all carry an `int64_t sig_val; memcpy(&x,
&sig_val, 8)` bit-cast dance at every SF→SF boundary because:

- `__arrow_call1` (`stdlib/arrow.tur:84`) returns `:int`.
- `__signal_call1` (`signal/src/signal/core.tur:112`) returns `:int`.
- The TUR_APPLY{0..4} fat-closure-invocation macros emitted by
  `src/compiler/emit_module.c:1988-2005` are hardcoded to
  `int64_t (*)(void *, int64_t, ...)`.
- The `__tur_fatshim<n>` auto-shims emitted at
  `src/compiler/emit_module.c:2057-2068` are hardcoded to `int64_t` return.
- Inline-C bodies in signal SFs hand-write the cast
  `((int64_t(*)(int64_t))(intptr_t)sv)(t)` -- thin function-pointer ABI,
  always int64.

Even though `(fn [t] :float ...)` already type-checks and `emit_fn_def`
already builds a typed `__fn` field via `ensure_typed_thunk_typedef`
(`src/compiler/emit_fns.c:313-318`), the *invocation* paths all force
the result back through `int64_t`. The runtime cannot observe the
declared `:float` return type.

## Good news up front

The compiler already does most of the type-system work:

- `Type` carries `result_full_type` for `TY_FN` (`emit_fns.c:313`).
- Closures already emit typed thunk function-pointer typedefs and
  store `__fn` as that typedef (`emit_fns.c:330-334`,
  `emit_expr.c:2236-2241`).
- Top-level `defn`s with `:float` return already emit C functions
  with `double` return (existing `__dsp_sin`, `__sample-mul`, etc.).

The hole is purely at **invocation**: TUR_APPLY macros, fat-shims,
poly-to-fat shims, and the hand-written casts in stdlib/signal.

## Scope

In scope:

1. Compiler emits `TUR_APPLYn_F` macros (and any other arity-shaped
   helpers) that invoke a fat closure expecting a `double` return.
2. Compiler emits `__tur_fatshim<n>_f` and `__tur_poly_to_fat1_f`
   variants for `:float`-returning closures.
3. `stdlib/arrow.tur` gains `__arrow_call1_f`. Existing
   `__arrow_call1` stays for `:int` returns (no breaking change).
4. The signal spice's `__signal_call1` is rewritten to return
   `:float` (or a sibling `__signal_call1_f` is added; see "Open
   questions" below).
5. Signal SF bodies switch to the new helpers, dropping every
   `memcpy(&x, &sig_val, 8)` block. Phase 0 of the signal plan ships
   from this work.

Out of scope:

- Making `Time` flow as `:float` end-to-end. Time stays `:int`
  (int64-bit-pattern of a double, by convention) for now -- separate
  cleanup, lower priority. See "Future work".
- Changing the `__arrow_call1` ABI for non-signal arrow consumers.
- Anything past Phase 0 of the signal plan -- this plan is only the
  unblock.

## Background -- how invocation works today

### Layer 1: typed `__fn` (already typed)

`emit_fns.c:312-345`: when emitting a closure's env struct, the
compiler picks `thunk_typedef` from the closure's `result_full_type`
and emits `struct __envN { typed_thunk_t __fn; <captures...>; };`.
For a `(fn [t :int] :float ...)` closure with no captures, this
becomes roughly:

```c
typedef double (*__thunk_double_int64)(void *, int64_t);
struct __env17 { __thunk_double_int64 __fn; };
```

So the `__fn` field already has the right type. The thunk's body
also has `double` return.

### Layer 2: invocation macros (int64 only)

`emit_module.c:1991-2005`:

```c
#define TUR_APPLY1(f, a) \
    (((int64_t (*)(void *, int64_t))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (int64_t)(a)))
```

This *re-casts* the typed `__fn` back to `int64_t (*)(void *, int64_t)`,
discarding the declared return type. Inline-C blocks call through
this macro, and they only know `int64_t` results.

### Layer 3: hand-written casts in stdlib + signal

```c
// stdlib/arrow.tur:84-86
return ((int64_t(*)(int64_t))(intptr_t)f)(x);

// signal/core.tur:112-115
return ((int64_t(*)(int64_t))(intptr_t)sig)(t);

// signal/dsp.tur:110-117
int64_t sig_val = ((int64_t(*)(int64_t))(intptr_t)sv)(t);
double x; memcpy(&x, &sig_val, 8);
```

The hand-written *thin* function-pointer cast in
`__arrow_call1`/`__signal_call1` is suspicious -- the fat-closure
invocation contract is `int64_t(*)(void*, int64_t)`, not
`int64_t(*)(int64_t)`. Either:

- The signal spice's SFs degenerate to non-capturing top-level
  `defn`s by the time they reach these helpers (so a thin cast is
  valid), or
- The thin cast happens to work because the captures struct begins
  with `__fn` and an x86_64 calling convention forgives the missing
  env argument when it goes unread.

This is worth confirming in implementation; either way the existing
behaviour is the baseline to preserve.

## Design

### New compiler output

`emit_module.c` gains, for each arity it currently emits:

```c
#define TUR_APPLY0_F(f) \
    (((double (*)(void *))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f)))
#define TUR_APPLY1_F(f, a) \
    (((double (*)(void *, int64_t))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (int64_t)(a)))
// ... up to TUR_APPLY4_F
```

And `:float`-returning fat-shims:

```c
static double __tur_fatshim1_f(void *__e, int64_t a0) {
    return ((double (*)(int64_t))(intptr_t)((int64_t *)__e)[1])(a0);
}
// ... and the other arities + poly-to-fat
```

These can ship behind the same `__tur_fatshim_keep[]` "unused" array
trick to avoid `-Wunused-function`.

### Codegen change at the call site

Where the compiler today emits a `TUR_APPLY1(...)` call, it should
emit `TUR_APPLY1_F(...)` when the callee's declared return type is
`:float`. The information is already in `result_full_type` (see
`emit_fns.c:313`). The dispatch is in the inline-C lowering path and
in any auto-generated call site (e.g. typeclass method dispatch via
`__tur_poly_to_fat1`).

### Inline-C ergonomic helper

Add a documented macro that DSP inline-C blocks can use directly:

```c
#define TUR_SF_CALL(sv, t) /* invoke a Time -> :float SF at time t */ \
    TUR_APPLY1_F((sv), (t))
```

So an SF body becomes:

```c
double x = TUR_SF_CALL(sv, t);
double y = av * x + (1.0 - av) * (*prev);
*prev = y;
return y;  // declared :float return
```

vs. today's:

```c
int64_t sig_val = ((int64_t(*)(int64_t))(intptr_t)sv)(t);
double x; memcpy(&x, &sig_val, 8);
double y = av * x + (1.0 - av) * (*prev);
*prev = y;
int64_t ret; memcpy(&ret, &y, 8);
return ret;
```

### Stdlib changes

`stdlib/arrow.tur` adds:

```turmeric
;;; __arrow_call1_f -- call a function stored as int64 with one argument,
;;; returning a :float result.
(defn __arrow_call1_f [f x] :float
  ```c return TUR_APPLY1_F(f, x);
  ```)
```

`__arrow_call1` is unchanged. Existing arrow consumers (`>>>`,
`__arrow_pair_*`, etc.) keep their `int64_t` behaviour. Only signal-spice
call sites that want a `:float` sample use the `_f` variant.

### Signal-spice changes

In `signal/core.tur`:

```turmeric
;;; __signal_call1 -- call a Time -> :float signal at time t.
(defn __signal_call1 [sig t] :float
  ```c return TUR_APPLY1_F(sig, t);
  ```)
```

Then `dsp.tur`, `envelope.tur`, `synth.tur` SF bodies stop bit-casting:
they call `__signal_call1` (or use `TUR_SF_CALL` in inline-C), use the
returned `double` directly, and return `double` from their own thunks.
The `(:: x :float)` / `(:: x :int)` boundary casts inside DSP go away;
only the gate-signal boolean cast stays.

## Phasing

Each phase merges independently and the next builds on it.

1. **Closure ABI extension (this repo).**
   - Add `TUR_APPLYn_F` macros to `emit_module.c`.
   - Add `__tur_fatshim<n>_f` and `__tur_poly_to_fat1_f`.
   - Codegen: dispatch to `_F` variants when callee's
     `result_full_type` is `:float`.
   - Fixture regeneration per the
     [CLAUDE.md fixture rule](../../CLAUDE.md#fixture-snapshots----strict-rule).
   - Smoke test: a one-file fixture with
     `(defn f [] :float ```c return 1.5; ```)` and
     `(defn g [] :float ((fn [] :float (f))))` returning `1.5`
     end-to-end through the closure path.

2. **Stdlib `__arrow_call1_f` (this repo).**
   - Add the `:float`-returning helper next to `__arrow_call1`.
   - No existing callers affected.
   - Add stdlib test exercising it.

3. **Signal spice migration (turmeric-spices repo).**
   - Rewrite `__signal_call1` to `:float`.
   - Rewrite SF bodies in `dsp.tur`, `envelope.tur`, `synth.tur`.
   - Drop the `(:: ... :int)` / `(:: ... :float)` boundary noise in
     SF construction sites.
   - Verify Phase 0 grep gates from
     [signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md#validation-gate)
     pass.
   - Existing examples (`01_basics.tur`, `02_signals.tur`,
     `03_dsp.tur`) must continue to run and print real floats (where
     they previously printed bit-cast garbage).

4. **(Optional) Time as `:float`.**
   - Audit every `(:: t :float)` callsite; teach SF bodies to take
     `:float` directly.
   - Requires `__arrow_call1_f` to also accept `:float` arguments,
     which means a second axis on the macro family
     (`TUR_APPLY1_FF` etc.) or making the argument type a per-call
     decision. Defer until a real consumer asks for it.

5. **Downstream: Phase 1+ of the signal plan.**
   - Once Phase 3 lands, the signal plan's Phase 1 / 1.5 / etc. all
     proceed as originally written. New primitives use `TUR_SF_CALL`
     / `__signal_call1` from day one; no new bit-cast idiom is
     introduced.

## Validation

Per phase:

- **Phase 1**: `bash tests/run.sh` clean. Smoke fixture asserts a
  closure with `:float` return propagates a value end-to-end.
  Fixtures regenerated.
- **Phase 2**: stdlib arrow test calls `__arrow_call1_f` on a
  trivial `(fn [_] :float 2.5)` and asserts the result.
- **Phase 3**: signal spice tests pass; grep gates from the parent
  signal plan empty; existing examples produce sensible floats.

## Risks

- **Fat-shim arity matrix doubles.** Today each arity has an int64
  shim; this plan adds a `_f` variant for each. Manageable -- mirror
  the existing `__tur_fatshim_keep` pattern.
- **Hidden invocation paths.** Effect handlers, reactor callbacks,
  and other auto-generated dispatch (search:
  `emit_effects.c`, `emit_module.c:2055-2077`) may need to know
  about `_f` returns. Audit during Phase 1 implementation. Likely
  scope creep, not blockers.
- **Thin-cast suspicion in current code.** The fact that
  `__arrow_call1` casts to `int64_t(*)(int64_t)` rather than the fat
  `int64_t(*)(void*, int64_t)` shape works today for reasons that
  aren't fully understood from a code read. Implementer should
  confirm whether SFs are actually invoked as fat or thin in current
  practice, and pick the matching `_F` cast shape. Worst case: an
  intermediate stop-gap where `__signal_call1_f` uses the same thin
  cast but with `double` return.
- **Fixture regeneration churn.** Per CLAUDE.md, all
  `tests/fixtures/*/expected.c` snapshots that depend on the closure
  preamble will change. Regenerate in the same PR per the strict
  rule.

## Open questions

- **Replace or add?** Should `__arrow_call1` / `__signal_call1`
  *change* their return type (breaking change inside signal) or get
  `_f` siblings (additive, no breakage)? Default: additive siblings
  for the stdlib; replace for `__signal_call1` since the signal
  spice owns its callers.
- **Macro naming.** `TUR_APPLY1_F` vs. `TUR_APPLYF1` vs. inferring
  from a return-type tag. Default: `_F` suffix, matches existing
  shim-keep convention.
- **Do we need a `TUR_APPLY1_F` if every call goes through
  `__arrow_call1_f`?** Probably yes -- inline-C blocks in DSP want
  to call SFs directly without a stdlib indirection. Keep the macro.
- **Polymorphism via tagged closure.** A future direction: store
  the return-type tag in the fat box and pick the cast dynamically.
  Out of scope here; mention so it isn't reinvented.

## Future work

- Time as `:float` (Phase 4 above).
- General "closure return type is observable at runtime" -- only
  worth pursuing if a second consumer (beyond signal) needs
  non-int64 closure returns.
- KissFFT vendoring still gated on
  [[spices-c-sources-plan]] -- unrelated to this work.

## Files this plan will touch

This repo:

- `src/compiler/emit_module.c` -- TUR_APPLYn_F macros, `_f`
  fat-shims, poly-to-fat `_f`.
- `src/compiler/emit_expr.c` and possibly `emit_fns.c` --
  dispatch to `_F` at call sites based on `result_full_type`.
- `stdlib/arrow.tur` -- add `__arrow_call1_f`.
- `tests/fixtures/**/expected.c` -- regenerated en masse.
- A new fixture exercising `:float`-returning closures end-to-end.

Sibling spice repo (turmeric-spices):

- `spices/signal/src/signal/core.tur` -- `__signal_call1` returns
  `:float`.
- `spices/signal/src/signal/dsp.tur` -- SF body cleanup.
- `spices/signal/src/signal/envelope.tur` -- SF body cleanup.
- `spices/signal/src/signal/synth.tur` -- SF body cleanup, drop
  the `(:: ... :int)` boundary noise.
- `spices/signal/tests/signal/arrow_tests.tur` -- update the float
  helpers; the `__f_*` int64-bit-pattern functions can become real
  `:float` literals once samples flow native.

## Acceptance checklist

- [ ] `TUR_APPLY{0..4}_F` macros emitted by `emit_module.c`.
- [ ] `__tur_fatshim{0..5}_f` shims emitted by `emit_module.c`.
- [ ] `__tur_poly_to_fat1_f` shim emitted by `emit_module.c`.
- [ ] Codegen picks `_F` variants when callee's declared return
      type is `:float`.
- [ ] Fixture snapshots regenerated; `bash tests/run.sh` zero
      FAILs.
- [ ] Smoke fixture asserts a `:float`-returning closure round-trips
      a non-trivial double through one call.
- [ ] `stdlib/arrow.tur` exports `__arrow_call1_f`; stdlib test
      passes.
- [ ] `signal/core.tur:__signal_call1` returns `:float`.
- [ ] No `int64_t sig_val; memcpy(...sig...)` in
      `spices/signal/src/`.
- [ ] No `(:: ... :int)` / `(:: ... :float)` around sample values
      in `spices/signal/src/` (gate-bool casts excepted).
- [ ] Existing signal examples and tests pass with native `:float`
      sample values.
- [ ] Phase 0 of
      [signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md)
      marked complete; Phase 1 unblocked.
