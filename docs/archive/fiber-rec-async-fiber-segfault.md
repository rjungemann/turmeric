---
status: resolved
severity: high
discovered: 2026-07-24
resolved: 2026-07-24
area: runtime (async fiber stack / deep catch-unwind inside a fiber)
---

# `fiber-rec` probe SIGSEGVs: deep catch-unwind inside an async fiber crashes

## Resolution (2026-07-24)

Not a fiber-stack-size issue -- a **codegen eviction**. The crash was depth-
dependent (fine to ~1,000 deep, SIGSEGV by ~10,000), i.e. an O(N) native-stack
overflow of the fiber's bounded (1 MB) stack, not an immediate context bug. The
same `cu-rec` that lowers to the flat `tur_cont` stackless trampoline standalone
(and runs to 1,000,000 on a 256 KB stack) was instead lowered to **native
recursion** (`tur_catch_unwind_box` -> `TUR_APPLY0` -> self-call) inside the
async program.

Root cause: the CPS-marking pass (`src/passes/cps.c`, `cps_mark_expr`, which runs
over the whole program once it contains `async`) shallow-clones every `FnDef`
(`*new_fd = *fd`) but left the binding's canonical-defn link
(`binding->source_fn_def`) pointing at the pre-clone `fd`. The stackless
catch-unwind eligibility check (`gs_basic_ok` in `emit_fns.c`) gates on
`binding->source_fn_def == fd` to identify the canonical top-level defn, so the
emitted clone failed it and every catch-unwind function in an async program
silently lost its flat-stack trampoline. Fix: re-point `source_fn_def` at the
clone when it replaces the canonical defn (one guarded assignment in the
`EX_FN_DEF` case).

Verified: `fiber-rec` passes at 1,000,000 depth under `ulimit -s 256` (the async
`cu-rec` now emits the `tur_cont` trampoline); depth sweep clean through 100,000;
full `bash tests/run.sh` is 2278 passed / 0 failed (the change is codegen-neutral
for every snapshot fixture -- it only affects async + deep-catch-unwind programs,
which are not snapshotted). `fiber-rec` was removed from the `xfail` set in
`tests/stackless-signoff-probes.sh` (which is now empty -- all probes pass).

## Summary

The `fiber-rec` sign-off probe (`tests/probes/stackless-signoff/fiber-rec.tur`)
crashes with SIGSEGV (exit 139) on the sanitized Debug build. It runs a
1,000,000-deep trampolined `catch-unwind` recursion inside an `async` fiber and
awaits the result; it is expected to print `1000000` but never prints anything.

The crash is deterministic and stack-size-independent (reproduces at
`ulimit -s 256`, at the default stack, and at `ulimit -s 1024`), so it is a real
runtime defect, not a tuned-stack overflow.

Unlike the sibling `effect-rec` bug, this one **fails fast** -- it does not hang,
so it did not by itself stall CI. It is quarantined alongside `effect-rec` as an
`xfail` in `tests/stackless-signoff-probes.sh`; this report tracks the crash.
Remove `fiber-rec` from the `xfail` set once fixed (it will XPASS and re-arm).

## Minimal repro

```turmeric
(defn cu-rec [n : int] : int
  (if (= n 0)
    0
    (do (catch-unwind (fn [] : int (cu-rec (- n 1)))) n)))

(defn worker [] : int
  (cu-rec 1000000))

(defn main [] : nil
  (let [fut (async worker)]
    (let [result (await fut)]
      (println result))))
```

```sh
tur build repro.tur -o /tmp/fr && (ulimit -s 256; /tmp/fr); echo "rc=$?"   # rc=139
```

Note the standalone `cu-rec` probe (same recursion, NOT inside a fiber) passes at
the same depth and stack, so the crash is specific to running the deep
catch-unwind trampoline **inside an async fiber** body.

## Root cause -- direction

No ASan report is emitted before the crash (the fault kills the process first),
consistent with a jump to a bad address or a blown fiber stack rather than a
heap error. Likely candidates: the async fiber is allocated with a fixed stack
that the sanitized (larger-frame) catch-unwind trampoline overruns, or the
catch-unwind trampoline's saved-context handoff is not fiber-aware. Start in the
fiber setup / context-switch path (`src/async/fiber.c`,
`src/async/fiber_ctx_x64.S`, `src/async/scheduler.c`) and the catch-unwind
trampoline's interaction with a fiber stack. Confirm the fiber stack size and
whether it is honored under the Debug (`-fsanitize=address,undefined`) build.

## Impact

`async` + deep `catch-unwind` is a supported composition (the probe header notes
"G7 verified this interoperates"), so this is a regression in a graduated shape.
Fixing it is v1 runtime work; the probe stands as the regression guard.
