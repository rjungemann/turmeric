# CPS backend miscompiles core reset/shift (continuation-substrate)

**Severity:** high (a core delimited-control program produces no/wrong output
under the CPS backend; the direct emitter compiles it correctly).  Surfaced by
the `cps-backend` graduation making the CPS path the default.

## Summary

`tests/fixtures/continuation-substrate` -- a battery of base `reset`/`shift`/
`shift0`, nested reset, and multiple-shift tests -- runs correctly through the
**direct** emitter but produces empty output / segfaults through the **CPS**
backend.  This is a genuine CPS lowering bug on the core (scalar-typed)
delimited-control subset, NOT an emittable-subset ABI gap: there is no
non-scalar signature or poly-fat value to evict on, so the eviction-gate work
(which closed the ABI-divergence class of graduation failures) does not touch it.

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
