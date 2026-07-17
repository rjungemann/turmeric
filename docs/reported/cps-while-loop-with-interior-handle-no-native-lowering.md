# effect-handler-capture-loop: a while loop with an interior control op has no native CPS lowering

**Severity:** medium (blocks `effect-handler-capture-loop` from the CPS/DK backend under
`--enable=cps-tramp-resume`; correctness is fine -- it runs on the fiber via whole-body
delegation, output `100`). This is a REAL fiber-live fixture (performs+handles `Ask`), so it
is a genuine migration target, unlike the `Unsafe`-marker or session-thread cases.

## The fixture

```turmeric
(defeffect Ask [] :int)
(defn run [] : int
  (let [^mut i 0
        ^mut total 0]
    (while (< i 5)
      (let [cur i]
        (set! total
          (+ total
             (handle                       ; <-- a control op (handle/perform/resume)
               (perform (Ask))             ;     INSIDE the loop body, per iteration
               (Ask [] k)
               (resume k (* cur 10))))))
      (set! i (+ i 1)))
    total))
(println (run))                            ; => 100  (10*(0+1+2+3+4))
```

Each iteration installs a fresh `Ask` handler that closes over the per-iteration snapshot
`cur` and resumes with `cur*10`.

## Exact eviction reason (pinned)

- **flag OFF:** `run` is SIG-TAINT (tainted, evicted to the fiber; runs correct).
- **flag ON:** `run` is **BODY-UNSUPPORTED, `unsupported form: EX_WHILE`**. The CPS IR dump
  (`tur emit-c --enable=cps-tramp-resume --dump-cps`) is:

  ```
  cps-fn run [] k:cont<int> entry
    let i = 0
    let total = 0
    <unsupported: unsupported form: EX_WHILE>
  cps-end
  ```

The CPS translator emits a `CT_UNSUPPORTED` for the `EX_WHILE`. There is NO native
`EX_WHILE` lowering in the CPS transform (`src/passes/cps_ir.c`): `EX_WHILE` appears only in
HELPER walks (`IFC` / `safe_to_delegate` / `expr_has_unsafe_control` / `body_calls_binding`)
and the P5 whole-body-delegation predicate -- never in the `cps_tail` / `cps_bind` transform
that would lower it, so it falls through to `unsupported_form(...)`.

## What IS handled today (and why it is not enough)

- A **control-op-free** while loop CPS-emits fine: `safe_to_delegate` admits it and the whole
  loop rides the direct emitter as a delegated region (`CT_LETRAW`) inside `run`'s DK body.
  Verified: a `while` with no interior `handle`/`perform` does not evict.
- A **self-contained-handle** loop is kept CORRECT on the fiber by P5 whole-body delegation
  (task-15 "native EX_WHILE lowering" is actually this DELEGATION -- it hands the loop to the
  direct/fiber emitter, so `run` runs on the FIBER, not the DK). That is why the fixture
  passes, but it does NOT put `run` on the DK.

Minimal trigger (isolated): a `while` with a self-contained `handle` in its body evicts
`BODY-UNSUPPORTED EX_WHILE`; move the same `handle` BEFORE the loop and `run` CPS-emits.
So the blocker is precisely: **a control op (handle/perform/resume) INSIDE a `while` body**,
on the CPS-split path (flag-on, once the effect is de-tainted and `run` becomes a genuine DK
candidate rather than a tainted fiber fallback).

## Why native DK lowering is a real slice (not a gate change)

The DK/CPS machine has no mutation and no loops -- it is tail calls + continuations. Lowering
this natively means:

1. **Loop-carried continuation args.** The `^mut` loop vars (`i`, `total`) cannot be C
   mutation in CPS. The `while` must lower to a tail-recursive loop JOIN -- a `CT_LETCONT
   loop(i, total)` whose body re-enters `loop(i', total')` -- so `set! i` / `set! total`
   become the next-iteration arguments. `(< i 5)` is the loop's `CT_IF` guard; the false arm
   delivers `total` to `run`'s return continuation.
2. **A nested per-iteration prompt.** The interior `handle` is a fresh DK prompt inside the
   loop body; `(perform (Ask))` threads to it, the case resumes with `(cur*10)`, and its
   result feeds `(+ total ...)` -- i.e. the join's `total'` argument. Each iteration's handler
   closes over the loop-local `cur`, so the prompt/case must capture `cur` from the current
   iteration's frame (a scalar capture -- collect_caps already handles scalars).
3. **Interaction with the E7 trampoline.** A loop that resumes through a per-iteration prompt
   must stay flat (the trampolined tail-resume already added for deep recursion); the loop
   join re-entry is itself a tail call, so this should compose, but verify stack flatness on a
   large iteration count.

This is a new transform (loop -> tail-recursive join with a nested prompt), not an admission
gate. It is the single native construct the CPS IR still lacks.

## Fix direction / verification

- Add an `EX_WHILE` case to the CPS transform that builds a loop-join `CT_LETCONT` with the
  `^mut` binders as loop-carried args, the cond as the guard, and the body lowered normally
  (so an interior `handle`/`perform` lowers as it does anywhere else). Watch: only the `^mut`
  vars written in the loop become loop-carried; a `^mut` read-only var can stay a capture.
- Companion minimal repros to iterate on: `while` + interior self-contained handle (must ->
  DK, print correct); `while` + interior handle whose effect ESCAPES to an outer handler
  (should still lower or cleanly evict -- do not miscompile); `while` with no control op (must
  stay delegated, unchanged).
- Gate: this is a shipping-backend transform (not necessarily flag-gated). Default suite
  (`bash tests/run.sh`, 12-min timeout) green; flag-off byte-identical if the transform only
  activates on the flag-on candidate path, else check snapshot churn. Full flag-on build sweep
  (known `-lturi`/turi false-positives only). Add a `cps-tramp-resume-...` regression fixture
  that asserts `run` emits `run__cps` (zero `eff=1` evictions) and prints `100`.

## Context

One of the compound BODY-UNSUPPORTED roots in
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md. Related landed work: the P5 whole-body
delegation (keeps it correct on the fiber) and E7 trampolined tail-resume (keeps deep resume
flat -- the loop join must compose with it).
