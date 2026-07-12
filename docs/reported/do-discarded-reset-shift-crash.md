# Direct backend: `(do (reset (shift ...)) <tail>)` crashes (SIGILL)

> **Graduation status (2026-07-12):** The `cps-backend` experiment is **fully
> graduated** -- always-on since #657 (2026-07-11), flag removed in #658.
> `--enable=cps-backend` now hard-errors (TUR-E0310). This report is a
> **direct/fiber** (evicted-path) defect, still valid, but the flag-based framing
> is stale: there is no longer a flag to toggle backends. "The DIRECT backend
> (no `--enable=cps-backend`)" vs "the `--enable=cps-backend` path" now describes
> the always-on split -- a colored function lowers through the CPS-IR backend,
> an evicted one through direct/fiber. The bug lives on the **evicted** path, so
> reproduce it with a function shape that evicts (as the repro does); plain
> `tur run repro.tur` is correct as written. (`emit_cps_reset` still exists in
> the tree -- that reference is fine.)

**Severity:** medium (crash, direct backend; narrow shape)

## Summary

A `do`-sequence whose *non-final* item is a `(reset (shift ...))` whose value
is discarded crashes the compiled program with `Illegal instruction` (SIGILL,
exit 132) on the DIRECT backend (no `--enable=cps-backend`). Independent of
call/cc -- the tail item can be a plain literal.

## Minimal repro

```turmeric
(defn f [n : int] : int (do (reset (shift (fn [v] v) 1)) 7))
(defn main [] : int (println (f 0)) 0)
```

```
$ tur run repro.tur
Illegal instruction
```

Expected: prints `7` (the discarded reset delivers 1, then the do tail yields 7).

## Notes / scope

- Reproduces with NO call/cc present, so it is unrelated to the U7 callcc
  native-emit work -- found incidentally while testing colored functions that
  sequence a reset/shift ahead of a call/cc.
- The `--enable=cps-backend` path lowers the same program correctly (the reset
  is native `CT_RESET`; the discarded value is bound and the `do` tail runs), so
  this is a direct-emitter (`emit_cps_reset` / do-sequence discard) defect, not a
  CT-IR one.
- Root cause not yet localized; likely the discarded-reset value slot in the
  direct `do` lowering (the reset delivers via the DK machine but the discard
  path mishandles the result), producing an ill-formed branch. Worth a timeout/
  UBSan sweep on `emit_cps_reset` + the `do` discard path.
