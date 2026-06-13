---
title: Plan M3 ("delete `emit_carrier_bridge`") is gated on M4, not just M2
category: Codegen / ABI — monomorphization plan refinement
severity: Low. M3 is presented in `docs/upcoming/end-to-end-monomorphization-plan.md` as a 1-2-session phase to retire the carrier-bridge machinery after M2 lands. Empirical audit (this session) shows that claim was over-optimistic: even with the full M2b stdlib migration in tree, the bridge is still load-bearing for typeclass-method-dispatch call sites. The actual deletion needs M4 (per-method typeclass ABI) first.
description: I shipped instrumentation (`TUR_M3_AUDIT=1`) inside `emit_carrier_bridge` so the per-call-site cost of removing it is measurable. Running the full suite under the audit shows only 2 crossings (and both inside already-FAILing pre-existing fixtures), which initially looked like the bridge was dead. Attempting the M3 deletion, the suite regressed by ~6 fixtures with cc errors of the form "passing 'int64_t' to parameter of incompatible type 'Result__int__cstr'". Direct per-fixture re-audit then showed the bridge IS firing for those — `tests/run.sh` was swallowing per-fixture stderr in the snapshot/test phase, so the suite-wide audit count was misleadingly low. The bridge's remaining role is `carrier→concrete` at `EX_ASCRIBE` (when a typeclass-method return like `(:: (decode …) (Result int cstr))` produces an int64 carrier but the ascription's outer context wants the by-value struct) and `concrete→carrier` at the typeclass-instance dispatch arg-cast site (when a by-value `Tuple2__int__int` is passed to a dict-dispatched `__inst_Eq_eq_qu_Tuple2(int64_t, int64_t)`).
status: PARTIALLY RESOLVED 2026-06-13. Path A landed (see `docs/reported/m4c-path-a-cascades-into-stdlib-eq-instances.md`); `emit-abi-trace` now has zero bridge crossings under `TUR_M3_AUDIT=1`. Residual `carrier→concrete` crossings remain on `typeclass-return-dispatch-result-wrapped` and `typeclass-method-parameterized-result-decode` — those are the **return-type** Path A shape (typeclass dispatch returns int64 carrier, ascription pins it to a by-value Result struct). Path A's arg-side substitution mechanism extends naturally to result-type substitution; that's the next concrete slice ahead of the bridge's full deletion.
---

# Plan M3 deletion is blocked on M4's per-method typeclass ABI

## The design's claim

`docs/upcoming/end-to-end-monomorphization-plan.md` §M3:

> The work is removing the carrier-bridge machinery (Prereq 2's
> `emit_carrier_bridge` CK_CARRIER -> CK_CONCRETE path) that exists to deref
> carrier-shaped accessors. With M2 in tree the accessors operate on real
> by-value structs and the bridge becomes dead code; M3 deletes it.

"Becomes dead code" turns out to be true for stdlib accessor wrappers
themselves (`ok-val`, `err-val`, `unwrap`, `pair-fst`, `pair-snd` — all already
`(.field x)`), but the bridge is still called at call sites those accessors
appear inside, when the receiver's runtime ABI is int64 carrier (because the
producer is a typeclass-method dispatch, not a monomorphized constructor).

## How the audit misled me

I added a `TUR_M3_AUDIT=1` env-var probe inside `emit_carrier_bridge` and ran
the full suite under it. The audit reported only 2 crossings:

```
1 bridge concrete->carrier  type=(type-app SChan (type-app (type-app SRecv int) ptr<void>))
1 bridge carrier->concrete  type=(type-app (type-app Pair int) int)
```

Both inside fixtures that already FAIL on main (`scheduler-multithread`,
`schan-worker-pool` etc.). That looked like a clear path to deletion.

**The audit was wrong**, but in a subtle way: `tests/run.sh` redirects
per-fixture compile stderr to `actual.stderr` and only echoes a summary on
PASS. So `fprintf(stderr, "[m3-audit] …")` from inside the compiler doesn't
reach the audit log when the fixture compiles cleanly — it lands in the
fixture's `actual.stderr` and gets thrown away.

Direct per-fixture audit (running `tur build` outside the harness) tells the
real story. For example:

```
$ TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/typeclass-return-dispatch-result-wrapped/input.tur 2>&1 | grep m3-audit
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result int) cstr)
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result cstr) cstr)

$ TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/emit-abi-trace/input.tur 2>&1 | grep m3-audit
[m3-audit] bridge concrete->carrier type=(type-app (type-app Tuple2 int) int)
[m3-audit] bridge concrete->carrier type=(type-app (type-app Tuple2 int) int)
```

So at minimum these three fixtures (plus the related
`typeclass-method-parameterized-result-decode`) need the bridge to compile.

## Why these crossings can't be eliminated by M2 alone

The remaining crossings sit at the **typeclass-method dispatch boundary**.

`carrier→concrete` example: `(:: (decode doc handle) (Result int cstr))`.
`decode` is a typeclass method; the dispatch table's slot is uniformly
`int64_t (*)(int64_t, …)`. The ascription pins the static type to `Result int
cstr` (a by-value struct after M2). To bridge: deref the int64 handle as
`Result__int__cstr *` and load the by-value struct. That's exactly the
`CK_CARRIER → CK_CONCRETE` path the bridge implements.

`concrete→carrier` example: `__inst_Eq_eq_qu_Tuple2(t1, t2)` where t1/t2 are
by-value `Tuple2__int__int` values. The dict-resolved instance method's C
signature is `bool __inst_Eq_eq_qu_Tuple2(int64_t, int64_t)` because the
dispatch dict slot is uniform. To bridge: spill the by-value to a local and
pass its address as `int64_t`. That's the `CK_CONCRETE → CK_CARRIER` path.

Both go away only when the dispatch dict's slot type matches the instance's
real signature — i.e. when the dict carries `bool (*)(Tuple2__int__int,
Tuple2__int__int)` for the Tuple2 instance. That is exactly **Plan M4** in
the monomorphization plan ("non-HKT typeclass instances switch to per-method
ABI"). Until M4 lands, the carrier ABI is the dict's contract, and the
bridge is the only thing keeping by-value and dict-uniform views consistent
across the same call.

## What got shipped this turn

- `src/compiler/emit_core.c` — `TUR_M3_AUDIT=1` env-var probe inside
  `emit_carrier_bridge`. Off by default; one `fprintf(stderr, …)` per
  crossing when enabled. Permanent diagnostic — useful when M4 lands and
  someone wants to re-verify that the bridge has actually gone dead.
- This finding doc.
- No emit/runtime change. Suite unchanged: 172 FAIL, exact pre-M3 baseline.

## Validation when M4 lands

1. Apply the M4 dict-slot rework so non-HKT instance dict slots use the
   per-instance C signature (e.g. `bool (*)(Tuple2__int__int,
   Tuple2__int__int)` rather than `bool (*)(int64_t, int64_t)`).
2. Run the full suite under `TUR_M3_AUDIT=1` directly (not through
   `tests/run.sh`, or with the harness patched to surface per-fixture
   stderr). Expected: zero crossings.
3. Delete `emit_carrier_bridge` (the impl in `emit_core.c`, the
   `CarrierKind` enum + decl in `emit_internal.h`, the 4 call sites in
   `emit_expr.c`, the unit test
   `tests/compiler/test_emit_carrier_bridge.c`, and the CMake target
   `tur_codegen_carrier_bridge`).
4. Full suite must remain at baseline.

## Related

- [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  §M3 / §M4 — the plan this report refines.
- [docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md](m2b-stdlib-migration-blocked-on-carrier-fallback.md)
  — the M2b finding that pointed at M4 as the deeper unblocker.
- `src/compiler/emit_core.c` `emit_carrier_bridge` — the bridge function
  (now carrying a permanent audit hook).
