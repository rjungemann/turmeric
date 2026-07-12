# CPS backend miscompiles core reset/shift (continuation-substrate) -- RESOLVED

**Severity:** high (a core delimited-control program produced no/wrong output
under the CPS backend; the direct emitter compiled it correctly).  Surfaced by
the `cps-backend` graduation making the CPS path the default.

**Status: RESOLVED** -- a `shift`/`shift0` whose receiver is a *capturing
closure* now evicts to the direct shift lowering, which handles it correctly.
`continuation-substrate` and `continuation-advanced` are green; suite 2140/2142.

## Root cause (localized)

Only the `t-deep` sub-test crashed:
`(reset (let [a 1 b 2 c 3] (shift (fn [v] (+ a (+ b (+ c v)))) 10)))` -- the shift
receiver is a **capturing closure** (closes over `a b c`). The CT-IR backend
lowers a shift body by synthesizing the receiver application `(recv val)`
(`cps_shift_body_kf`, src/passes/cps_ir.c) and delegating it. When `recv` is a
capturing `EX_CLOSURE`, that synthesized call has `fn_binding == NULL` /
`fn_expr == EX_CLOSURE`, and the direct emitter's indirect-call block
(`emit_expr.c` ~2950, `if (e->as.call_.fn_expr)`) casts the callee VALUE -- which
for a fat closure is its **env pointer** -- to a *bare* function pointer and calls
it, jumping into the heap-allocated env struct as code:

```c
/* WRONG (was): env pointer cast to bare fnptr, env arg dropped */
((int64_t (*)(int64_t))(intptr_t)(__t174))(INT64_C(10));
/* RIGHT (direct standalone): dispatch through the closure's __fn, env first */
(*(tur_thunk_int64_t_int64_t_t *)(env))(env, 10);
```

A non-capturing receiver is a hoisted top-level fn (an `EX_VAR` -> real bare
fnptr), which the same path calls correctly -- so only *capturing* receivers
crashed. The normal elaborator never produces a raw `EX_CLOSURE` callee (it
hoists/binds closures before application), so the direct emitter's indirect
block never had to handle one; the synthesized shift-body call was the sole
producer.

## Fix

`indirect_callee_ok` (src/passes/cps_ir.c): a capturing `EX_CLOSURE` is not a
valid bare indirect callee, so both `cps_tail`/`cps_bind` `EX_CALL`
(`fn_binding == NULL`) sites return `CT_UNSUPPORTED`, evicting the enclosing
function to the direct shift lowering (`emit_cps.c`), which handles a capturing
receiver correctly. Only `continuation-advanced` re-snapshotted (its capturing
shift receiver now direct-emits).

## Latent follow-up (native coverage)

The direct emitter's indirect-call block still cannot emit a raw
capturing-`EX_CLOSURE` callee (it would need the fat-dispatch form
`(*(thunk*)env)(env, args)`). It is now unreachable (the CPS shift path was the
only producer, and it evicts), but teaching that block to fat-dispatch a closure
callee would let capturing shift receivers lower natively instead of evicting --
a coverage improvement, not a correctness fix.

## Original report

`tests/fixtures/continuation-substrate` -- a battery of base `reset`/`shift`/
`shift0`, nested reset, and multiple-shift tests -- ran correctly through the
**direct** emitter but produced empty output / segfaulted through the **CPS**
backend.

## Repro

```sh
# direct (pre-graduation default) -- correct:
#   t-shift=43 / t-shift0=107 / t-nested-reset=11 / t-multiple=6 /
#   t-ignores-k=42 / t-no-shift=123 / t-deep=16
./build/tur build tests/fixtures/continuation-substrate/input.tur -o /tmp/cs && /tmp/cs
# -> (CPS default, post-graduation) empty output, segfault (exit 139)
```

The failure reproduces identically on the pre-graduation tree under
`--enable=cps-backend`, confirming it is pre-existing in the CPS backend and not
introduced by the graduation flip or the eviction-gate tightening.

## Where to look

The base reset/shift lowering in `src/compiler/emit_cps_ir.c` (`emit_reset` /
`emit_shift` / `delim_ok` and the DK `dk_shift`/`dk_run`/`dk_prompt` sequence)
against the direct emitter's `emit_cps_reset` (`src/compiler/emit_cps.c`), which
produces the correct values.  Bisecting which of the seven sub-tests
(shift / shift0 / nested-reset / multiple / ignores-k / no-shift / deep) is the
first to diverge will localize the offending shape.

## Status

Carried red.  It is one of three residual `cps-backend`-graduation failures that
are NOT eviction-subset gaps (the other two: `contract-nested`, a lifted
heap-join that references the enclosing continuation `k` it never receives; and
`hkt-stdlib-parser-instances`, a delegated-binder naming desync).  All three want
CPS *lowering/emit* fixes rather than gate tightening.  Because this one is a
core-correctness behavior bug (not an ABI-edge case), it is the most important of
the three to close before, or alongside, relying on the CPS backend as the
whole-program default.
