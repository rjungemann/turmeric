---
title: stackless recursive await on heap continuations (F4)
category: Planning
status: superseded / declined (2026-07-19) -- the premise ("recursive await must be stackless ON the heap path, and eviction is the wrong end-state") was investigated and rejected as works-as-intended; see the 2026-07-19 progress note. The RA2 substrate trampoline shipped and graduated (cps-tramp-resume) for effects, but recursive await stays evicted to the direct emitter by design. Follow-up to F3 (compiled-async-heap-continuations-plan.md, archived).
description: Remove the last cps-async residual -- a recursive `await` currently EVICTS to the direct emitter instead of running on the heap-continuation representation, because the heap inline-resume recurses through dk_invoke in O(N) C stack. Make recursive await stackless ON the heap path so the representation is a strict superset (no shape falls back for stack reasons). The identical limitation affects recursive effects (perform/resume); the trampoline option closes both.
---

# stackless recursive await on heap continuations (F4)

## Progress (2026-07-19) -- premise declined; plan superseded

This plan's goal was rejected after investigation and is no longer the intended
end-state. Two things settled it:

1. **The residual is works-as-intended, not a bug.** The root-cause report this
   plan cites (`docs/reported/cps-async-recursive-await-eviction.md`) was
   re-investigated and archived 2026-07-18 to `docs/archive/` marked **RESOLVED /
   BY-DESIGN**: recursive `await` evicting to the direct emitter is the *correct*
   default. Because `(async fn)` is synchronous on the compiled path, a recursive
   await is *always* a ready future -- exactly the case the heap `dk_shift` path
   handles worst (O(N) C stack via a non-tail `dk_invoke`), while the direct
   emitter's inline readiness check + `goto __tur_tailcall` loop is O(1). The
   `term_core_ok` `CT_AWAIT` arm still DELIBERATELY rejects a cps->cps tail-call
   await continuation (with a comment pointing at the archived report), so
   recursive await stays on the direct path -- and that is the shipping decision,
   not a gap.

2. **RA2 (the recommended mechanism) shipped and graduated -- for effects, not
   this.** The trampolined DK tail-resume is live: `emit_dk_runtime.c` emits the
   "E7: trampolined tail-resume" prelude, and the gating experiment
   **`cps-tramp-resume` GRADUATED 2026-07-19** (now the default+sole effect
   lowering; `experiments.c`). So the substrate half of RA2 exists and made the
   *effect* path stackless. It did **not** re-admit recursive await onto the heap
   path: even with a trampolined `dk_invoke`, the dominant ready-future recursion
   still favors the direct TCO path (per the archived report), so RA.2 (reverse
   the `await_cont_reset_ok` rejection) and RA.3 (an `async-rec` heap-path probe)
   were not done, and `cps-async` remains an ungraduated experiment.

Net: the RA2 trampoline landed as part of the effect endgame, the F4-specific
re-admission of recursive await did not, and the stated goal is now considered
the wrong end-state. This plan is superseded -- keep it only as the record of why
recursive await is not colored onto the heap path. Nothing here is a v1 gate.

## Context

F3 ([compiled-async-heap-continuations-plan.md](../../archive/compiled-async-heap-continuations-plan.md),
archived) landed async/await on the DK heap-continuation representation behind
`--enable=cps-async`, and closed every admissibility gap **except one residual**:

> A **recursive** `await` evicts to the direct emitter rather than running on the
> heap representation. Admitting it onto the heap path (a prototype in the F3
> work) produced correct results but regressed deep **ready-future** recursion
> from O(1) (direct TCO loop) to **O(N) C stack** -- SIGSEGV at ~100k under a
> 256KB stack. So F3 left recursive await on the direct path "by design."

That residual is what this plan removes. "Stays on direct by design" is the wrong
end-state: the heap representation should be a strict superset of the direct one,
so no program shape falls back to the fiber path for stack-depth reasons. When a
user writes a recursive async function, it must be stackless on the heap
representation, not silently routed elsewhere.

### Root cause (measured -- docs/reported/cps-async-recursive-await-eviction.md)

The heap inline-resume of a ready future recurses through `dk_invoke`, which is
**not** a tail call (it `dk_free`s the copy after `dk_run_impl` returns):

```
f__cps(n,k) -> dk_run_impl [SHIFT] -> __tur_await_body(ready) -> dk_invoke
            -> dk_run_impl [frame]  -> f__cps(n-1,k) -> ...
```

Each recursion level retains at least the outer `dk_run_impl` frame (the SHIFT
case calls the shift body non-tail) and the `dk_invoke` frame (work after the
inner `dk_run_impl`). So a depth-N recursion is O(N) C stack, and C tail-call
optimization of `return f__cps(...)` alone cannot fix it -- the retained frames
are in the runtime, not the generated function.

### The identical limitation affects effects (grounding for approach RA2)

`tick-loop` in the `effect-rec` stackless probe -- a recursive `perform`/`resume`
loop -- is **colored but EVICTED to the direct emitter** (no `tick_loop__cps`
entry; it emits `tur_effect_perform(...)` inside an explicit `goto __tur_tailcall`
loop). The probe passes at 1,000,000 depth under `ulimit -s 256` **because it runs
on the direct/fiber path**, not the DK heap path. So:

- There is **no** trampolined stackless resume in the DK machine today.
- Recursive control (await AND effects) is stackless only via direct-emitter
  eviction (the `goto`-loop + the fiber runtime).
- Making the DK machine itself stackless for a tail resume would close the residual
  for await and effects uniformly.

## Goal

A recursive `await` that colors onto the heap representation runs in **O(1) C
stack** for both ready and pending futures. Concretely: the `async-rec` probe
shape (`(defn go [n acc] (let [v (await ...)] (if (= n 0) acc (go (- n 1) (+ acc
v)))))`) runs at 1,000,000 depth under `ulimit -s 256` **on the heap path**
(`go__cps` + `dk_shift`), not by eviction to `tur_await_future`. No regression to
effects, to the F3.1/F3.2 fixtures, or to default (no-flag) codegen.

## Design -- two approaches

### RA1 -- inline readiness split (await-specific, lower risk)

Emit `await` with an inline readiness check, so a ready future never enters the
heap resume:

```c
if (tur_future_done(fut)) {
    int64_t v = tur_future_get(fut);
    /* continuation emitted INLINE with v bound -- a self-recursive tail call is a
       C tail call the emitter lowers with the direct emitter's `goto __tur_tailcall`
       loop (robust at any -O), so recursion is O(1). */
} else {
    return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body_park, fut, <lifted cont>), 0);
}
```

- **Ready branch**: continue inline. A recursive tail call is O(1) via the same
  self-tail-call loop the direct emitter already emits -- this is essentially
  bringing `tur_await_future`'s readiness check into the colored body while
  keeping the heap `k` in scope for the pending branch.
- **Pending branch**: `dk_shift` + park exactly as F3.1/F3.2 (heap continuation
  unwinds to the driver; the resume is flat). Consecutive pending awaits do not
  nest -- each park unwinds.
- **Cost**: the continuation is emitted twice (inline for ready, lifted for
  pending) -- code-size, not correctness.
- **Risk**: robust TCO of the inline recursive tail call. Mirror the direct
  emitter's explicit `goto` self-tail loop rather than relying on the C compiler's
  sibling-call optimization (which is absent at `-O0`/Debug). Mutual recursion
  still needs real TCO; scope RA1 to self-recursion first, or accept `-O2`.
- **Effects**: untouched (await-specific). Recursive effects stay on the direct
  path (a separate, pre-existing residual).

### RA2 -- trampolined DK tail-resume (substrate-wide, higher risk, complete)

Make a **tail** inline-resume in the DK machine not recurse. When a shift body /
effect handler's last action is to resume the captured continuation, hand
`(subk, value)` back to the `dk_run_impl` driver loop instead of calling
`dk_invoke`:

```c
/* thread-local resume signal */
static _Thread_local DK *g_dk_resume_k; static _Thread_local intptr_t g_dk_resume_v;
static _Thread_local int  g_dk_resume_pending;

static intptr_t dk_invoke_tail(DK *sub, intptr_t v) {   /* tail-position only */
    g_dk_resume_k = dk_copy_range(sub, NULL); g_dk_resume_v = v; g_dk_resume_pending = 1;
    return 0;  /* sentinel; the driver reads the signal */
}
```

`dk_run_impl`, after `bodyval = k->body(...)`, checks `g_dk_resume_pending`: if set,
`k = g_dk_resume_k; v = g_dk_resume_v; clear; continue;` -- re-entering the loop
with the resumed continuation, no stack growth.

- **O(1) for all heap recursion**: await AND effects, including mutual recursion
  and any tail-resume the C compiler could not sibling-call.
- **Preserves multi-shot / non-tail `dk_invoke`**: only a *tail-position* resume
  trampolines; `dk_invoke(a) + dk_invoke(b)` and any resume whose result is used
  keep the existing recursive `dk_invoke`. The emitter marks tail resumes
  (`emit_await` / the effect-case emitter already know when the resume is the tail).
- **Risk**: this is the graduated-effects substrate. Every effect fixture, the
  shallow-handler probes, and `cps_prompt_unit` must stay green; a mis-marked
  non-tail resume would drop a continuation. Land it substrate-first (RA.0) with
  an adversarial unit test before any emit change.

**Recommendation.** RA2 is the principled end-state -- it makes the heap
representation genuinely stackless and fixes recursive effects for free -- but it
touches the graduated effect runtime. Sequence: prove RA2 on the substrate (RA.0);
if the effects-safety bar is met, land RA2 and re-admit both recursive await and
recursive effects onto the heap path. If RA2's effect-safety proves too costly to
land now, ship RA1 (await-only, removes the user-visible residual) and keep RA2 as
the follow-up that also un-evicts recursive effects.

## Phased plan

### RA.0 -- substrate proof (no codegen)

- `tests/cps_prompt_unit.c`: a recursive await/shift chain resumed N times that,
  under the chosen mechanism, completes in **bounded** C stack. Instrument with a
  recursion-depth counter (a static max-depth watermark, or run under a small
  `ulimit -s` in a dedicated harness) so the test FAILS if the resume is O(N).
  For RA2, add an adversarial probe: a *non-tail* multi-shot resume
  (`dk_invoke(sub,10) + dk_invoke(sub,20)`) must still return 60 (the trampoline
  must not fire in non-tail position).
- Mirror in both DK copies: `src/runtime/cps_prompt.c` (unit-tested) and
  `src/compiler/emit_dk_runtime.c` (emitted). Keep them in lockstep.

### RA.1 -- mechanism

- **RA1**: extend `emit_await` (`emit_cps_ir.c`) with the inline readiness split;
  reuse the direct emitter's self-tail-call `goto` loop for the ready branch.
- **RA2**: add the tail-resume trampoline to `dk_run_impl` / a `dk_invoke_tail`
  entry in both DK runtimes; mark tail resumes in `emit_await` (and, if
  un-evicting effects, in the effect-case emitter).

### RA.2 -- re-admit recursive await onto the heap path

- Reverse gap-1's rejection in `await_cont_reset_ok` / the `term_core_ok`
  `CT_AWAIT` arm so a cps->cps tail-call await continuation is admitted **once the
  stackless mechanism is in place** (guard the change on the mechanism, not on a
  flag toggle, so it cannot re-introduce the O(N) path).
- Keep the `binding_in_s`-during-classification hazard in mind (gap 2): the
  admission predicate must not key a soundness decision on a not-yet-settled
  classification flag.

### RA.3 -- verify stackless on the heap path

- Re-point (or add) an `async-rec` heap-path probe that asserts `go__cps` +
  `dk_shift` is emitted (not `tur_await_future`) AND runs 1,000,000 deep under
  `ulimit -s 256`. Cover both a ready-future recursion and a pending-driven
  recursive variant (a reactor/driver loop fulfilling each level).
- For RA2: an effect analogue (recursive `perform`/`resume`) on the heap path at
  1M under the reduced stack.

### RA.4 -- no-regression

- `effect-rec` and all stackless-signoff probes stay green; `cps_prompt_unit`,
  `shallow_handler_probes`; F3.1 (`async-await-cps`), F3.2
  (`async-await-cps-pending`, `-two`, `-repark`) unchanged; `bash tests/run.sh`
  green; snapshots regenerated in the same change.
- Default (no `--enable=cps-async`) codegen byte-identical (RA1 is gated inside the
  cps-async await lowering; RA2's trampoline is inert unless a tail resume fires,
  which only the DK path produces).

### RA.5 -- hot-path neutrality (RA2 only)

- The trampoline adds a per-step check in `dk_run_impl` (the effect hot path).
  Measure effect-heavy fixtures neutral-or-better, matching the bar F2/F3 held.

## Graduation

This is the last cps-async residual. On landing (RA.3/RA.4 green), fold into the
`cps-async` graduation: the heap representation becomes a strict superset of the
direct path for async, the experiment can graduate (flag deleted, added to
`GRADUATED[]`), and -- if RA2 -- recursive effects un-evict onto the heap path in
the same line. Until then `cps-async` stays gated.

## Depends on / reuses

- The F3 landing (await-as-shift F3.1, deferred suspend/resume F3.2, bounded
  multi-await gap-2) -- this plan only changes how a *recursive* await continuation
  is lowered/resumed.
- The DK substrate (`src/runtime/cps_prompt.{h,c}`, `src/compiler/emit_dk_runtime.c`)
  and the CPS-IR backend (`src/compiler/emit_cps_ir.c`).
- The direct emitter's self-tail-call `goto` loop (RA1 reuses it for the ready
  branch).
- The stackless-signoff harness (`tests/stackless-signoff-probes.sh`) and
  `tests/cps_prompt_unit.c` for the depth guards.

## Out of scope

- Cross-thread continuation invocation beyond the single-owner handoff F3.2
  already provides.
- Removing the `ucontext` fiber runtime -- it stays the default `async` scheduler;
  this plan makes the *alternate* heap representation stackless for recursion, it
  does not retire the fiber path.
- The parked-`__root` leak (F3.2/gap-2) -- reclaiming it needs resume-completion
  tracking; tracked separately, non-blocking.
