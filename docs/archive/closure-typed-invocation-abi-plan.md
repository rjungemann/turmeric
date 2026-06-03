---
title: Typed Closure Invocation ABI Plan
category: Planning
description: Compiler-level plan to thread the declared argument and return types of a Turmeric closure all the way to the C invocation site, retiring the int64-only TUR_APPLYn / fat-shim / __arrow_call surface. Lets fn closures take and return any C-representable type the language already supports for top-level defns.
---

# Typed Closure Invocation ABI -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** compiler ABI -- closure invocation
> **Sibling plans:**
> - [stdlib-type-erasure-cleanup-plan.md](stdlib-type-erasure-cleanup-plan.md) -- Tier 2 names `__arrow_call1/2` as falling out once fat-closure dispatch matures; this plan is that maturation
> - [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md) -- "no bespoke fat-closure casts in stdlib" goal; this plan provides the typed alternative
> - [signal-phase-0-spike.md](signal-phase-0-spike.md) -- the spike that produced this plan
> - [signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md) -- one of several downstream consumers

---

## Problem

Anonymous `fn` closures in Turmeric carry their declared argument and
return types through the type system and through `emit_fn_def`'s thunk
emission, but every C-level invocation path discards that information
and forces both axes through `int64_t`:

- `TUR_APPLY{0..4}` (`emit_module.c:1991-2005`) cast `__fn` to
  `int64_t (*)(void *, int64_t, ...)`.
- `__tur_fatshim<n>` (`emit_module.c:2057-2068`) hard-code `int64_t`
  return and `int64_t` parameters.
- `__tur_poly_to_fat1` (`emit_module.c:2075-2077`) does the same.
- Hand-written inline-C in stdlib and spices follows the same shape:
  `((int64_t(*)(int64_t))(intptr_t)f)(x)`.

Top-level `defn`s with declared `:float`, `:bool`, `:cstr`, `:ptr<T>`,
sized-int, or by-value-struct returns already emit C functions with
the corresponding C return type. Closures of the same shape cannot.
Anywhere that gap shows up, callers fall back to the `int64_t`
carrier and pay for it with one of:

- `int64_t sig_val; memcpy(&x, &sig_val, 8)` bit-cast dances
  (signal SF bodies; see [signal-phase-0-spike.md](signal-phase-0-spike.md)).
- `(int64_t)(intptr_t)p` round trips for pointer-typed closures
  (stdlib `select.tur`, `httpd.tur` curried middleware -- see the
  type-erasure plan, Tier 2/3).
- `:bool` closures losing their explicit type at the boundary.
- Typeclass methods that return another instance method getting
  type-erased to `int64_t` (the dict-field-as-`void *` bug noted in
  [stdlib-type-erasure-cleanup-plan.md](stdlib-type-erasure-cleanup-plan.md)).

These are all the same problem.

## Goal

Closure invocation -- both inline-C macro form and Turmeric defn form
-- speaks the closure's declared C types. A `(fn [x :cstr] :ptr<T> ...)`
call site emits a `T *(*)(void *, char *)` cast; a
`(fn [t :int] :float ...)` site emits a `double (*)(void *, int64_t)`
cast; the existing `(fn [x :int] :int ...)` site stays exactly as
today. No type is privileged; the legacy int64 path is the special
case for "no declared type."

## Non-goals

- New runtime *behaviour* for closures (dispatch is still a direct
  call through a function pointer; nothing tagged).
- Boxed sum types as a return surface (`:Option<T>`, `:Result<T,E>`,
  `:Either<L,R>`). The carriers are already `:ptr<box>`, so this
  plan covers their pointer form; ergonomic sugar for them lives in
  [stdlib-type-erasure-cleanup-plan.md](stdlib-type-erasure-cleanup-plan.md).
- ABI changes for top-level `defn`s.
- Polymorphic-at-runtime closures (a single handle whose return type
  varies). Out of scope; see "Future directions."

## Design

### One macro family, parameterised by C type

`emit_module.c` emits a single token-pasting macro family covering
return type and per-argument type:

```c
/* General form. R is the closure's C return type; A0,A1,... are its
 * C argument types in order. */
#define TUR_APPLY0_T(R, f) \
    (((R (*)(void *))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f)))
#define TUR_APPLY1_T(R, A0, f, a) \
    (((R (*)(void *, A0))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (A0)(a)))
#define TUR_APPLY2_T(R, A0, A1, f, a, b) \
    (((R (*)(void *, A0, A1))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (A0)(a), (A1)(b)))
/* ... up through TUR_APPLY4_T */
```

`R` and the `A_i` are C type tokens. The compiler already knows them:
`result_full_type` plus the param-type array on `FnDef`. At every
call site the compiler emits, `emit_type_c_name` produces the tokens
to paste.

The legacy `TUR_APPLY{0..4}` macros are retained as
literal-equivalent shorthands for the all-`int64_t` case (so existing
hand-written inline-C in stdlib keeps compiling unchanged):

```c
#define TUR_APPLY1(f, a) TUR_APPLY1_T(int64_t, int64_t, f, a)
```

There are no per-type `_F` / `_P` / `_B` aliases in the C surface.
The compiler picks the right tokens at emission time and humans
writing inline-C either use the `_T` form (typed) or the legacy
`int64_t`-only form (untyped). Two macros, not a dozen.

### Typed fat-shims

`__tur_fatshim<n>` converts a non-capturing bare `fn` into the
fat-closure protocol. It's emitted once per closure-type signature
the program actually uses. Today it's parameterised only by arity;
generalise to parameterise by `(R, A0, A1, ...)`:

```c
/* X-macro form, instantiated once per (arity, sig) pair the program
 * actually reaches. */
#define TUR_FATSHIM(name, R, A_LIST_DECL, A_LIST_CAST, A_LIST_FWD)  \
    static R name(void *__e A_LIST_DECL) {                          \
        return ((R (*)A_LIST_CAST)(intptr_t)((int64_t *)__e)[1])    \
                   A_LIST_FWD;                                      \
    }
```

The compiler emits one shim per concrete `(R, A0, ...)` signature
that an `EX_FN_TO_FAT` lowering needs, naming them
`__tur_fatshim_<sig-hash>` (mangled deterministically from the type
tokens). The existing `__tur_fatshim_keep[]` "unused" trick keeps
`-Wunused-function` quiet over whichever subset a given TU touches.

`__tur_poly_to_fat1` is generalised the same way: one shim per
distinct `(R, A0)` pair that typeclass-method dispatch needs.

### Codegen dispatch

Every call site in the compiler that today emits `TUR_APPLYn(...)`
now consults the callee's `Type` and emits `TUR_APPLYn_T(R, A0, ...,
...)`. Sources of the type at each site:

- Inline-C lowering: `result_full_type` plus `param_types[1..]`
  off the closure's `FnDef`.
- Auto-generated typeclass dispatch: the resolved method type.
- `EX_FN_TO_FAT`: the source `fn`'s declared signature, which picks
  the matching shim mangling.

When the type is exactly `int64_t -> int64_t -> ... -> int64_t`, the
emitted form collapses to the legacy macro (or stays as `_T(int64_t,
int64_t, ...)`, which is literal-equivalent). Existing fixtures
that exercise only int64 closures see no behavioural diff -- only
preamble churn from the new macro definitions.

### Stdlib `arrow.tur`

`__arrow_call1` / `__arrow_call2` stay, doing what they do today:
the all-`int64_t` case, hand-written. Plans that want typed
invocation grow typed helpers next to them as they need them:

```turmeric
;;; __arrow_call1_typed -- placeholder name; real signature is per-caller.
;;; Each consumer writes its own typed defn against TUR_APPLY1_T.
```

The reason there is no "blessed" typed `__arrow_call*` in this plan
is that the typed call site needs *concrete* types (signal wants
`:float -> :float`; select wants `:ptr<T> -> :ptr<T>`). A single
generic helper would erase them again. The plan provides the macro;
each consumer writes the one-line `defn` that wraps it.

The type-erasure cleanup plan and the inline-C de-workaround plan
both call out their own arrow-shaped surfaces; once this plan
lands, those plans grow their typed helpers without further
compiler work.

### Consumers in scope

This plan delivers the infrastructure plus migrates the call sites
called out below. Each migration is small (one to five inline-C
blocks), and together they exercise enough of `(R, A_i)` space to
prove the design.

| Consumer | Closure shape today | After |
|---|---|---|
| signal spice SF bodies (`dsp.tur`, `synth.tur`, `envelope.tur`) | `int64 -> int64` carrier of `double -> double` | `double -> double` |
| signal spice `__signal_call1` | `:int -> :int` | `:float -> :float` |
| `stdlib/select.tur` `seq-call-fn{0,1,2}` | `int64`-erased trampolines | typed per-iterator instantiation |
| `stdlib/httpd.tur:1688` `mw-cors` curry | `:int` carrier for `:ptr<Middleware>` | typed pointer return |
| `stdlib/list.tur:185-205` `__cons-fmap` | hand-rolled fat-closure call | typed `TUR_APPLY1_T` |

The signal spice consumer also unblocks Phase 0 of
[signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md);
the stdlib consumers are the proof that the design is general, not
the only justification for it.

## Phasing

Phases land in order; each is independently mergeable.

1. **Macro infrastructure (this repo).**
   `TUR_APPLY{0..4}_T` emitted by `emit_module.c`. Legacy
   `TUR_APPLY{0..4}` redefined as `_T(int64_t, ...)` aliases.
   Templated fat-shim emission keyed by closure-type signature.
   Fixture preamble regeneration only (no call-site changes yet).
   Smoke fixture: a closure with a non-int64 return type
   round-trips a value end-to-end through `TUR_APPLY*_T`.

2. **Codegen dispatch (this repo).**
   Inline-C lowering, typeclass dispatch, and `EX_FN_TO_FAT` emit
   `_T` calls keyed off `result_full_type` and `param_types`.
   Fixture call sites change wherever a closure had non-int64
   declared types. Validation gate: no regenerated `expected.c`
   contains a legacy `TUR_APPLY[0-9]+(` whose surrounding
   `defn`/closure signature declares non-int64 types.

3. **Stdlib migrations (this repo).**
   `select.tur` `seq-call-fn{0,1,2}`, `httpd.tur` `mw-cors`,
   `list.tur` `__cons-fmap` move to typed helpers. Each gets a
   one-line `defn` over `TUR_APPLY*_T` and drops its bespoke cast.
   Stdlib tests must pass unchanged.

4. **Signal-spice migration (turmeric-spices repo).**
   `__signal_call1` returns `:float` and takes `:float`. SF bodies
   in `dsp.tur` / `synth.tur` / `envelope.tur` drop the
   `int64_t sig_val; memcpy(&x, &sig_val, 8)` blocks. The
   `(:: x :int)` / `(:: x :float)` boundary casts around sample
   and time values go away. Grep gate from
   [signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md#validation-gate)
   passes. Phase 0 of that plan is then complete.

5. **Downstream consumers proceed (other repos / other plans).**
   The signal plan's Phase 1+ resumes. The type-erasure plan grows
   typed arrow helpers wherever it wants them. The inline-C
   de-workaround plan reaches for `TUR_APPLY*_T` instead of
   hand-rolled casts.

## Validation

- **Phase 1**: `bash tests/run.sh` clean. Smoke fixture for a
  non-int64 closure round-trip passes. Preamble-only fixture diff.
- **Phase 2**: full fixture regen. Invariant grep:
  `grep -rE 'TUR_APPLY[0-9]+\(' tests/fixtures/` finds zero hits
  inside functions whose enclosing closure signature declares
  non-int64 types (only the legacy alias survives, and only for
  truly int64 closures).
- **Phase 3**: stdlib tests pass; each migrated site's diff is
  "delete bespoke cast, add one-line typed defn."
- **Phase 4**: signal spice tests pass; signal plan's Phase 0
  validation gate (`grep -n "memcpy(&" src/signal/` empty in
  sample contexts; `grep -nE "::\s+:(int|float)\b" src/signal/`
  empty in sample expressions) passes.
- **Phase 5**: nothing this plan owns; the downstream plans
  validate themselves.

## Risks

- **Per-signature shim explosion.** Today one shim per arity; now
  one per `(R, A0, ...)` signature in use. Mitigation: emit shims
  on demand (keyed by mangled signature), not preemptively. The
  fixture preamble grows with the closure-signature diversity of
  the program, which is bounded and visible.
- **Mangling stability.** Signature-mangled shim names appear in
  fixtures, so adding a closure type can produce a fixture diff
  even when no test changes. Mitigation: pick a deterministic,
  stable mangling (e.g. `__tur_fatshim_<arity>_<typehash>`);
  document the algorithm so fixture diffs are reproducible.
- **Hidden invocation paths.** Effect handlers, reactor callbacks,
  spawn dispatch -- anything that today reaches the `int64_t`
  carrier through a code path other than the four sites named in
  "Codegen dispatch." Mitigation: Phase 2 includes an audit of
  every `int64_t (*)(void *` occurrence in `emit_*.c`. If a path
  is missed, its callers stay on the int64 carrier and the typed
  invariant in Phase 2 catches it as a fixture grep failure.
- **Thin-vs-fat cast discrepancy in current stdlib.**
  `__arrow_call1` casts as `int64_t(*)(int64_t)` (thin), but the
  fat-closure contract is `int64_t(*)(void*, int64_t)`. Today's
  code works for reasons not fully understood from a static read;
  Phase 1 implementation must reconcile the thin form with the
  fat-shim protocol before generalising it. Worst case: the typed
  helpers ship in fat form only, and stdlib's thin-cast sites stay
  on the legacy macro until a separate cleanup migrates them.
- **Fixture churn.** Per
  [CLAUDE.md fixture rule](../../CLAUDE.md#fixture-snapshots----strict-rule),
  every snapshot that depends on the closure preamble changes.
  Regenerate in the same PR; do not split.

## Open questions

- **Argument-type coercion at the call site.** `TUR_APPLYn_T`
  casts each argument with `(A_i)(value)`. For pointer-typed
  arguments this is the right thing; for `:cstr` it's
  `(char *)(value)`, which suppresses a warning if the caller
  already has a `const char *`. Audit during Phase 1 whether
  `(A_i)(value)` is always safe or whether we want
  argument-position-specific reinterpret rules.
- **Mangling scheme.** Deterministic and short matter; choose
  before Phase 1 lands so fixtures don't churn twice.
- **Typed `__arrow_call*` in stdlib.** This plan deliberately does
  not ship one (the typed call sites want concrete types, and a
  blessed helper would erase them). Reopen if a real consumer
  needs a one-shot typed call without writing its own defn.
- **Polymorphic closures.** A single handle whose return type
  varies at runtime (tagged in the fat box). Not pursued here;
  pay the cost when a real consumer asks.

## Files this plan will touch

This repo:

- `src/compiler/emit_module.c` -- `TUR_APPLY*_T` macros, signature-
  keyed fat-shim emitter, `__tur_poly_to_fat1` generalisation.
- `src/compiler/emit_expr.c`, `src/compiler/emit_fns.c`,
  `src/compiler/emit_effects.c` -- emit `_T` at every closure
  invocation site, pulling types off `result_full_type` and
  `param_types`.
- `stdlib/select.tur`, `stdlib/httpd.tur`, `stdlib/list.tur` --
  migrated to typed helpers (Phase 3).
- `tests/fixtures/**/expected.c` -- regenerated.
- One new fixture per non-int64 axis (`:float` return, `:ptr`
  return, `:bool` return, `:cstr` argument) round-tripping a
  closure end-to-end.

Sibling spice repo (turmeric-spices):

- `spices/signal/src/signal/core.tur`, `dsp.tur`, `envelope.tur`,
  `synth.tur` -- typed `__signal_call1`, SF body cleanup, drop
  boundary casts.
- `spices/signal/tests/signal/arrow_tests.tur` -- `__f_*`
  int64-bit-pattern helpers become real `:float` literals.

## Acceptance checklist

- [ ] `TUR_APPLY{0..4}_T(R, A_i..., f, args...)` macros emitted.
- [ ] Legacy `TUR_APPLY{0..4}` redefined as int64 aliases of `_T`.
- [ ] Signature-keyed fat-shim and `__tur_poly_to_fat1` emitters
      land, keyed by `(arity, R, A_i...)`.
- [ ] Every closure invocation in compiler-emitted C goes through
      `_T` keyed off the closure's declared types.
- [ ] Fixture invariant grep passes (no legacy `TUR_APPLY[0-9]+(`
      inside non-int64 closure signatures).
- [ ] Round-trip fixtures pass for `:float`, `:ptr`, `:bool`
      returns and a `:cstr` argument.
- [ ] `select.tur`, `httpd.tur`, `list.tur` cited sites migrated;
      stdlib tests pass.
- [ ] Signal spice migrated; Phase 0 grep gates from
      [signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md)
      pass; existing examples run with native sample values.
- [ ] `bash tests/run.sh` zero `FAIL`.
