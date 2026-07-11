# cloneable-shift with a non-atomic context operand drops the context (silent miscompile)

**Severity: HIGH (silent wrong output on valid code; D3 regression).**

**RESOLVED (D6a).** The common case -- a non-atomic pure arithmetic operand
(`(+ (id 3) [])`) -- now lowers natively: `build_cloneable` admits it via
`env_expr` (emit_value'd once at the reset site, deep-cloned per resume), and
`term_core_ok` skips the operand-slot check for an env_expr frame. The whole
residual class of unsupported cloneable contexts (deep contexts, colored callees,
...) that the legacy setjmp path silently miscompiled is now rejected at codegen
with **TUR-E0710** (mirroring the serial TUR-E0706 fix), so nothing drops the
context silently. Regression fixture `cloneable-shift-nonatomic-context` and
negative fixture `errors/cloneable-context-not-capturable` close the blind spot.
Original report below.

---

## Summary

A `(cloneable-reset CTX[(cloneable-shift k v)])` whose delimited context has a
**non-atomic operand** (a function-call operand rather than a literal/variable)
silently drops the context: the captured continuation is lowered as the identity
instead of `CTX[]`. Valid programs that produced the correct answer before
Phase D3 now print the wrong number, with no diagnostic.

## Minimal repro

```turmeric
(defn id [x : int] : int x)
(defn k-r [k] : int (tur_cloneable_cont_resume k 100))
(defn run [] : int
  (cloneable-reset (+ (id 3) (cloneable-shift k-r 0))))   ; continuation is (+ 3 [])
(defn main [] : int (println (run)) 0)
```

- Correct: `resume(k, 100)` runs `(+ 3 100)` = **103**.
- D5 output: **100** (the `+3` context is dropped -> identity continuation).
- The atomic twin `(+ 3 (cloneable-shift k-r 0))` prints **103** (goes native).

Also miscompiled (same root cause, non-atomic operand in the context):
`(+ (f 3) (+ (f 4) (cloneable-shift k-r 0)))` -> `100` (should be `107`);
`(* (f 5) (cloneable-shift k-r 0))` -> `10` (should be `50`).

## Root cause

`build_cloneable` (`src/passes/cps_ir.c`) rejects a non-atomic non-hole operand
in a context frame (`if (!other || !is_atomic(other) ...) return NULL;`, ~L419 /
L482). Pre-D3 such a reject fell through the delimited-control carve-out to the
DK direct lowering `emit_cps_cloneable_reset` (`emit_cloneable_ctx` /
`collect_ctx`), which reified the context correctly. D3 deleted that lowering;
D4 removed the carve-out; the shape now evicts (`CT_UNSUPPORTED`) to the whole-
function direct emitter, which reaches `emit_effects_cloneable_reset` **Case-2**
(the legacy setjmp boundary) -- and that legacy path was only ever correct for
Case-1 (the shift IS the whole reset body). For a shift inside a context it emits
a trivial `__cont_fn` (identity) and drops the context.

## Why the D3 metric missed it

D3 was gated on "0 genuine `-emit` reaches corpus-wide." The corpus has **no**
fixture of the form `(op (non-atomic) (cloneable-shift))`, so the DK lowering
showed 0 emit reaches and was deleted -- but it was the sole correct emitter for
that shape. The metric had a **corpus-coverage blind spot**: "0 corpus reaches"
was read as "dead," when it was "untested."

## Fix directions

- **Preferred (D6a):** teach the native `build_cloneable` to admit a non-atomic
  but shift-free / pure context operand by hoisting it into a `CloneLet` prelude
  (the frame then references the hoisted binding), restoring the capability the
  deleted DK lowering had. Audit the other `build_cloneable` NULL-returns the
  same way (colored callee, 2-arg frames, ...) for further blind spots.
- **Stopgap (D6b):** make the eviction / legacy-Case-2 path a form-named hard
  error (`TUR-E07xx`) so the shape is rejected at compile time instead of
  silently miscompiled. Stops the wrong-answer, but rejects code that worked
  pre-D3.

Do **not** leave the silent miscompile in place.

## Repro fixtures

`tests/fixtures/` has no coverage of this shape -- add one
(`cloneable-shift-nonatomic-context`) as part of the fix so the blind spot
closes.
