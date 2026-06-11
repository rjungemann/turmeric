# turi: serial-shift / cloneable-shift (context-capturing delimited control) unimplemented

**Summary:** The tree-walking interpreter (`turi`) implements the *abortive*
delimited-control operators -- `reset`, `shift`, `shift0`, and the
no-shift case of `serial-reset` / `cloneable-reset` (Phase TI3) -- but does
**not** yet implement the *context-capturing* variants `serial-shift` and
`cloneable-shift`, which hand a resumable continuation to their receiver. A
program that performs `serial-shift` or `cloneable-shift` runs correctly under
`tur build` / `tur run` (compiled) but errors under `tur --interpret`.

**Severity:** Ergonomics / parity gap (not a miscompile). The interpreter
errors out cleanly; it never produces a wrong answer. Tracked as the remaining
slice of TI3 in
[docs/upcoming/v1/turi-parity-post-v1-plan.md](../upcoming/v1/turi-parity-post-v1-plan.md).

## Background: two flavors of shift in Turmeric

The `EX_SHIFT` / `EX_SHIFT0` nodes are **abortive**. The compiled path lowers
`(shift f body)` to: evaluate `body` to `v`, compute `f(v)`, then abort the
computation up to the nearest enclosing `reset`, whose value becomes `f(v)`.
The captured sub-continuation is never resumed -- the emitted runtime body is
`__dk_abort_body`, which ignores the captured slice (see the generated C and
`src/runtime/cps_prompt.c`). `shift0` differs only in prompt re-installation
on resume; since the continuation is discarded, it behaves identically.

Because the continuation is discarded, a plain `setjmp`/`longjmp` prompt
boundary models these exactly -- this is what TI3 shipped in `src/turi/eval.c`
(`eval_reset_boundary` / `eval_abortive_shift`).

By contrast, `serial-shift` and `cloneable-shift` pass a **resumable
continuation** `k` to their receiver `f`. `f` may invoke `(k w)` to resume the
delimited context (the frames between the shift and its reset) with `w`:

```turmeric
;; serial-context-marshal: (+ 10 []) is captured; (k 5) resumes it -> 15
(serial-reset (+ 10 (serial-shift (fn [k] (k 5)) 0)))   ; => 15
```

`cloneable-shift` additionally supports **multi-shot** resume -- invoking `k`
more than once, each from the same capture point:

```turmeric
;; cloneable-context-multishot: (10+1) + (10+2) = 23
(cloneable-reset (+ 10 (cloneable-shift (fn [k] (+ (k 1) (k 2))) 0)))  ; => 23
```

## Observed vs. expected

Minimal repro (`/tmp/cap.tur`):

```turmeric
(defn main [] : int
  (println (serial-reset (+ 10 (serial-shift (fn [k] (k 5)) 0))))
  0)
```

- `tur run /tmp/cap.tur`        -> prints `15` (compiled path).
- `tur --interpret /tmp/cap.tur` -> exits non-zero; `serial-shift` hits the
  default arm in `src/turi/eval.c` ("unhandled expression kind ...").

Expected: parity -- the interpreter prints `15` as well.

## Root cause

`src/turi/eval.c` has no `case EX_SERIAL_SHIFT:` / `case EX_CLONEABLE_SHIFT:`
arm. They fall through to the default "unhandled expression kind" error. The
abortive `setjmp`/`longjmp` boundary that backs `reset`/`shift`/`shift0` cannot
express a resumable continuation, so these need genuine continuation capture.

Two relevant complications:

1. **The `(k w)` application lowering.** Applying a continuation value of type
   `serial-cont` / `cloneable-cont` is sugar (cps-transform-plan CC4) that the
   compiler lowers to the appropriate `*_resume` runtime call. The interpreter
   needs a matching resume path. The existing effect machinery already exposes
   `TURI_EFFECT_CONT` + `eval_resume_cont` (used by algebraic-effect handlers),
   which is the natural substrate to reuse for one-shot resume.

2. **Multi-shot capture (`cloneable-shift`).** Resuming the same continuation
   more than once requires either (a) cloning the suspended fiber stack
   (`ucontext` copy + register/stack-pointer fix-up -- notoriously fragile and
   non-portable), or (b) a replay strategy that re-evaluates the reset body with
   the shift site injecting each successive resume value (safe only for pure
   contexts; re-runs any side-effecting prefix). The compiled path sidesteps
   both by reifying the continuation as a heap `DK` chain (`dk_copy_range`).

## Proposed fix directions

- **One-shot (`serial-shift`):** reuse the fiber-backed `TuriEffectCont`
  substrate (`eval_handle` / `eval_resume_cont` in `src/turi/eval.c`). Model a
  `serial-reset` as a prompt that runs its body in a fiber; on `serial-shift`,
  suspend the fiber and call `f` with a `TURI_EFFECT_CONT` wrapping it; `(k w)`
  resumes the fiber once. This naturally runs any side-effecting prefix exactly
  once (the suspend/resume model, unlike replay), matching `serial-context-do`.
- **Multi-shot (`cloneable-shift`):** the harder corner. Either port a small
  heap-reified continuation chain (mirroring `src/runtime/cps_prompt.c`) into
  the interpreter, or restrict to pure-context replay and carve out
  side-effecting multi-shot. Decide as a follow-up; `cloneable-context-multishot`
  is the gating fixture.

## How to validate a fix

Run the context-capturing fixtures under the interpreter and compare to their
`expected.stdout`:

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/serial-context-marshal/input.tur
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/cloneable-context-multishot/input.tur
```

Then add the now-passing fixtures to the TI3 block of `tests/run-turi.sh`.
Candidate fixtures: `serial-context-*`, `workflow-roundtrip`, `cont-value-typed`
(serial half), `cloneable-*`, `context-call-frame`, `context-division`. Note
that `cont-flavors`, `callcc-*`, and `escape-*` additionally need `call/cc` /
`escape` (`EX_CALLCC`), which the plan tracks separately under the CPS-transform
category and is out of TI3 scope.
