---
status: resolved
severity: medium
discovered: 2026-07-24
area: interpreter (turi_eval accumulated-source re-parse + eval_arenas retention)
---

# A long-lived turi env re-parses all prior source every eval (O(N^2) time and memory)

> **RESOLVED 2026-07-25 by TR2** (incremental elaboration -- persistent
> `ElabSession`, caller-slices-forms, offset-aware reader), which shipped and
> was made the default in this same line of work. Re-measured against today's
> tree:
>
> | evals | RSS growth | per eval |
> |---|---|---|
> | 500 | 17.6 MB | 35.9 KB |
> | 1000 | 27.4 MB | 28.1 KB |
> | 2000 | 45.7 MB | 23.4 KB |
> | 4000 | 82.0 MB | 21.0 KB |
>
> **Linear, and the per-eval cost falls as the session grows** (a fixed startup
> component amortising), which is the opposite of the quadratic signature. For
> scale: the original measurement recorded ~4.1 GB of `eval_arenas` at 3000
> evals; 4000 evals now cost 82 MB of RSS in total.
>
> One measurement note for anyone re-running this: `mallinfo2` is useless for it.
> The probe links an ASan-instrumented `libturi`, and ASan replaces glibc's
> allocator, so `mallinfo2` reports 0 growth at every size. RSS from
> `/proc/self/statm` is allocator-independent. (The same trap is documented in
> `tests/run-gc-leak-gate.sh`.)

## Summary

Each top-level `turi_eval` call on a long-lived env rebuilds and re-parses the
**entire accumulated source of the session so far**, and retains the resulting
AST in a fresh per-eval arena that is never freed until env teardown. For a
session of `M` evals totalling `N` bytes of source, that is **O(N^2) parse time
and O(N^2) retained memory** -- a REPL / notebook-kernel / embedded-interpreter
session degrades quadratically in both. Execution correctly skips already-run
forms; **parsing and arena retention do not.**

This surfaced while measuring TR0 of
`docs/archive/turi-interp-incremental-reclamation-plan.md`: with scratch
promotion rewinding 100% (value pool steady at 0), a 3000-eval session still grew
`eval_arenas` to **~4.1 GB across 3001 nodes** while `src_acc` grew linearly
(0.1 -> 39 KB). The AST-retention term dominated everything else (the value pool
was 158 MB by comparison), and the wall-clock rose visibly as the session grew.

**Impact is scoped to long-lived envs.** A one-shot `tur build` / `emit-c` (a
single eval) and the compiler path are unaffected -- there is no accumulated
prior source. Short REPL sessions are fine; the cost only bites sessions with
many evals. So this is not v1-blocking, but it is the dominant cost for exactly
the use case (a persistent REPL/kernel) that the interp-reclamation plan targets.

## Root cause

In `turi_eval_impl` (`src/turi/eval.c`), every call:

1. **Rebuilds the combined blob** = all prior source + the new turn
   (`eval.c:9919-9926`): `buf_write(&combined, env->src_acc.data, env->src_acc.len)`
   then appends the new `src_body`.
2. **Allocates a fresh per-eval arena and never frees it** (`eval.c:9928-9932`):
   a new `ArenaNode` is malloc'd and prepended to `env->eval_arenas`, whose field
   comment is literally "never freed" (`env.h:165`). The whole combined blob's
   AST lives in it.
3. **Parses the whole blob** (`eval.c:9963-9965`): `read_all_with_registry`
   lexes/reads **all** `nforms` of the accumulated source into that arena, every
   call.
4. **Executes only the new tail** (`eval.c:9975-9978`, skip boundary at
   `eval.c:10094-10128`): `prior = env->prior_toplevel` and the run/elaborate
   logic starts past the already-run forms -- so execution is *not* redundant,
   but the parse and the arena that backs it already covered the entire blob.
5. **Appends the new source to `src_acc`** (`eval.c:10143-10144`), growing the
   blob that step 1 will re-copy and step 3 will re-parse next time.

So per eval N the work is O(size of accumulated source through N); summed over the
session that is O(N^2) in both parse time and retained arena bytes. The retained
memory overlaps the concern already captured in
`docs/archive/turi-interp-incremental-reclamation-plan.md` TR2; **the CPU
re-parse is the distinct, additional defect this report records.**

## Measured (2026-07-24)

Promotion-enabled env, 3000 evals of `(build 300)` after a small prelude
(harness in `scratchpad/promo/measure.c`):

| Term | After setup | After 3000 evals |
|------|------------:|-----------------:|
| `value_scratch` (promotion ON) | 0 B | 0 B (steady -- promotion works) |
| `value_perm` | 26 KB | 26 KB (steady) |
| `eval_arenas` | 1 node / 15 KB | **3001 nodes / ~4.1 GB** |
| `src_acc` | 131 B | 39 KB |

The `eval_arenas` growth is quadratic and independent of scratch promotion
(promotion never touches `eval_arenas`).

## Fix directions

1. **Parse only the new tail (the principled CPU fix).** The already-run boundary
   the executor uses (`prior_toplevel` / `prior_prog_items`) means prior forms are
   already parsed and run; re-reading them is pure waste. Parse only `src_body`
   for the new turn and reuse the prior forms/registry, rather than re-reading the
   whole `src_acc` blob each call. This removes the O(N^2) parse term.
2. **Do not retain every per-eval arena (the memory fix, = TR2).** After promotion
   has copied out anything the eval result reaches, free the completed eval's
   AST/elaboration arena instead of keeping it on `eval_arenas` forever. Removes
   the O(N^2) memory term.
3. **Stop growing `src_acc` unboundedly.** If forms are kept parsed (fix #1),
   `src_acc` no longer needs to accumulate raw text for re-parsing -- keep only
   what diagnostics/`base_dir` resolution genuinely require.

Fixes #1 and #2 are complementary (CPU vs memory) and both belong to the TR2 work
item; this report exists so the **parse-time** cliff is tracked explicitly and not
lost inside the memory-framed plan phase.

## Recommendation

Not v1-blocking (one-shot builds and the compiler are unaffected; short sessions
are fine). Do it as part of TR2, and prefer fix #1 (parse only the new tail) over
merely reclaiming arenas -- reclaiming memory alone leaves the quadratic parse
cost in place, which is a real interactive-latency regression for a long
REPL/kernel session.
