# Cloneable continuation snapshot resumes to `0` under `--interpret`

> **RESOLVED 2026-06-13** (R5 of turi-interpret-flip-residual-plan). Fixed and
> on the run-turi.sh allowlist; verified under the harness + compiled suite.
>

**One-line summary:** Under `tur --interpret`, resuming a *snapshot* of a
cloneable continuation (`tur_continuation_snapshot` +
`tur_cloneable_cont_resume`) yields `0` instead of the value passed to resume --
the second, independent resumption is silently dropped.

**Severity:** High -- **silent miscompile** (rc=0, wrong stdout). The first
resume is correct, so the bug is specifically in snapshot/clone independence;
the compiled path is correct.

## Minimal repro

`tests/fixtures/multishot-snapshot/input.tur`:

```turmeric
(defn k-return [k] : int k)

(defn main [] : int
  (let [k (cloneable-reset (cloneable-shift k-return 0))]
    (let [snap (tur_continuation_snapshot k)]
      (println (tur_cloneable_cont_resume k 10))
      (println (tur_cloneable_cont_resume snap 20))))
  0)
```

Observed vs expected:

```
$ tur --interpret input.tur          expected.stdout
10                                    10
0                                     20
```

The original `k` resumes correctly (`10`); the cloned `snap` resumes to `0`
instead of `20`, i.e. the snapshot does not carry an independently-resumable
copy of the continuation -- it behaves as an already-consumed / empty
continuation.

## Root-cause analysis (hypothesis)

The interpreter's delimited-control path lives in `src/turi/eval.c` (the
ucontext fiber machinery at `:691+`; `async_fiber_thunk` at `:198`). One-shot
continuation accounting under the tree-walker differs from the compiled
`cps_prompt` semantics (a known divergence class noted in the gap-closure plan's
W3, "Continuation already resumed"). `tur_continuation_snapshot` is meant to
*clone* the captured continuation so each copy can be resumed once; the
interpreter most likely either (a) does not deep-clone the captured frame/value
state, so the snapshot shares (and loses) the original's one-shot budget, or (b)
lacks a native for `tur_continuation_snapshot` and returns a degenerate value
that resumes to `0`.

This sits in the same family as the R4 effect/multishot fixtures
(`multishot-handler`, `multishot-copy-capture`, `fh-multishot-value`) and
overlaps the explicit-stack restructuring in
[docs/upcoming/v1/turi-eval-trampoline-plan.md](../upcoming/v1/turi-eval-trampoline-plan.md).

## Proposed fix directions

1. Confirm whether `tur_continuation_snapshot` /
   `tur_cloneable_cont_resume` have interpreter natives at all (grep
   `wk_register_*` / `eval.c`); if missing, that is the bug -- add them over the
   cloneable-continuation representation.
2. If present, ensure the snapshot performs an independent copy of the
   continuation's captured state (its own resume budget + frame), so resuming
   `snap` does not see `k`'s consumed state.
3. Use this fixture as the anchor for the broader R4 multishot triage -- a fix
   here likely informs `multishot-copy-capture` / `multishot-handler`.

## Validation

- `multishot-snapshot` prints `10` / `20` under `--interpret`; add to the
  `tests/run-turi.sh` allowlist.
- Re-check the other R4 multishot fixtures for incidental recovery.
- `tests/run.sh` unchanged (interpreter-only fix).

Tracked for the flip in
[docs/archive/history/turi-interpret-flip-residual-plan.md](history/turi-interpret-flip-residual-plan.md)
(Buckets R5/R4).
