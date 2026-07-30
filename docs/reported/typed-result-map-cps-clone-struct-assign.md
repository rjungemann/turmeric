# typed/result-basic: CPS clone assigns Result struct to int64 -- cc rejects

**Severity: medium.** A latent MISCOMPILE (hard cc error) in a fixture no
compiling harness ever ran: tests/run.sh scans only `tests/fixtures/*/`,
so nested `typed/*` fixtures were interpreter-covered (run-turi.sh) only.
Found 2026-07-30 by the J3 jit harness (tests/run-jit.sh), which scans one
subdirectory deep like run-turi.sh does -- and compiles.

## Repro

```sh
tur build tests/fixtures/typed/result-basic/input.tur -o /tmp/rb
#   error: incompatible types when assigning to type 'int64_t' from type
#   'tur_adt_Result__int__int'
#     __t0 = __t238;
#   in function 'test_hyresult_hymap_hypreserves_hytag__cps'
```

Fails identically under `tur jit` (c2mir: "incompatible types in assignment
to an arithmetic type lvalue", both split and full preamble) and under the
jit's cc fallback -- every COMPILING engine agrees the C is ill-typed. The
interpreter runs the fixture correctly, which is why the suite stayed
green.

## Where to look

The `__cps` clone of `test-result-map-preserves-tag`: a by-value
`(Result int int)` temporary is assigned to an int64 carrier slot without
the boxing step the direct emitter performs. Likely the same
byvalue-carrier handoff family as the S1 typed-temp work, on the cps-clone
path.

## Fix directions

Route the clone's carrier temp through the same tur_box_*/carrier bridge
the direct path uses, or type __t0 by the sig side table
(emit_sig_lookup_ret_ctype) as the cps->direct path already does.
Add `typed/*` (and `typed-slots/*`) to a compiling harness's default scan
so the class cannot re-hide -- tests/run-jit.sh now does this.
