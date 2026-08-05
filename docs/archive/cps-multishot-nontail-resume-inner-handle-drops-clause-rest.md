---
status: resolved 2026-08-05 (layer 1: dk_invoke trampoline scoping; layer 2: handle-chain spine unification)
severity: high (silent wrong answer, no diagnostic)
discovered: 2026-08-05
area: compiler (CPS/DK runtime: dk_invoke trampoline scope; handle-continuation frame envs)
---

# A non-tail multishot resume whose continuation re-enters an inner handle drops the rest of the clause

> **Resolved 2026-08-05, in two layers.** The title defect -- the rest of the
> clause being discarded -- was one of **two stacked defects**. Layer 1:
> `dk_invoke` now scopes the tail-resume trampoline instead of letting a yield
> longjmp past it to the program entry. Layer 2 (the defect layer 1 unmasked --
> the outer handle's continuation running once per resume): the handle chain's
> two spines are unified. The handle-continuation frame is now a
> `DKK_RESUME_FRAME` (it receives its downstream chain at run time instead of
> jumping to a pointer baked into its env), and its `next` is the **actual**
> enclosing chain, borrowed (`dk_frame_resume_borrow`; `dk_free` stops at the
> borrow). One spine then serves `dk_perform`'s handler search, its capture
> boundary, and its `H->next` delivery alike -- so the outer continuation runs
> exactly once, however many times the clause resumed. See
> [Execution](#execution-2026-08-05). The original root-cause section below is
> superseded -- its guess about `dk_invoke`'s boundary "not being sealed
> against a fresh prompt" was directionally right about layer 1 and silent
> about layer 2. The repro and every boundary variant now print the
> hand-evaluated answers on both paths, pinned by
> `tests/fixtures/effect-multishot-nontail-resume-inner-handle/`.

## Summary

On the compiled path, when a `^multishot` handler clause resumes in NON-tail
position and the resumed continuation runs back through an **inner `handle`**,
the first resume's value is delivered as the outer handle's value directly --
the remainder of the clause (its arithmetic and any further `resume`) never
runs. The program compiles cleanly and prints a wrong number.

Found 2026-08-05 while executing
[turi-ws-capturable-stale-black-box-arms](../archive/turi-ws-capturable-stale-black-box-arms.md):
the interpreter, once its work-stack driver could run these shapes at all,
produced a *different answer* from the compiled path -- and hand-evaluation
says the interpreter is right.

## Repro

```turmeric
(defeffect Ask [] : int)
(defeffect Log [n : int] : int)
(defn main [] : int
  (println (handle
             (handle (let [v (perform (Ask))] (perform (Log v)))
                     (Log [n] k2) (resume k2 (* n 2)))
             (Ask [] ^multishot k) (+ (resume k 1) (resume k 10))))
  0)
```

Expected **22**: `resume k 1` runs the continuation -- `Log 1` through the
inner handler, doubled, `2` -- and returns 2 to the `+`; `resume k 10` returns
20; the clause's value is 22. `tur --interpret` prints 22.

`tur run` prints **2**.

The weighted variant pins down what happens:

```turmeric
(Ask [] ^multishot k) (+ (* 1000 (resume k 1)) (resume k 10))
```

turi: **2020** (= 1000*2 + 20, correct). Compiled: **2** -- not 2000, not
2002. So the first resume's value (2) did not return into the clause's `(*
1000 _)` at all; it became the outer handle's value, and everything after the
first resume in the clause was discarded.

## Boundary conditions (all verified)

| Variant | Compiled | turi | Verdict |
| --- | --- | --- | --- |
| the repro (double resume, inner handle) | **2** | 22 | compiled wrong |
| single `resume k 7`, same inner handle | 14 | 14 | both right |
| double resume, NO inner handle (`(* 2 (perform (Ask)))` body) | 22 | 22 | both right |
| double resume, inner handle, perform in arg position (`(perform (Log (perform (Ask))))`) | 2 | 22 | compiled wrong (same mechanism) |

So each ingredient is individually fine; the failure needs all three: a
multishot clause, a resume in **non-tail** position, and an **inner handle**
re-entered by the resumed continuation.

## Root cause (hypothesis, not pinned)

The symptom -- "the resumed continuation's final value becomes the outer
handle's value instead of returning to the `dk_invoke` call site" -- is
exactly the semantics of a TAIL resume (`dk_tail_resume` yields to the entry
driver and the value is delivered by it, nothing follows in the clause). The
non-tail path (`emit_cps_ir.c`'s `emit_resume`, the `dk_invoke` branch) is
supposed to return the value inline. A plausible mechanism: re-entering the
inner handle re-installs a prompt/handler node whose delivery routes the
continuation's completion to the outer prompt chain rather than back out of
`dk_invoke` -- i.e. `dk_invoke`'s "run until this sub-chain completes" boundary
is not sealed against a fresh prompt installed *during* the resumed run. Not
verified; start by tracing which DK node receives the inner handle's
completion value in the repro vs. the no-inner-handle variant.

## Where the truth is pinned

`tests/fixtures/turi-ws-driven-operands` asserts the correct values on the
interpreter (22 and 2020 among them). While the defect was open there was
deliberately no compiled fixture for this shape (it would have had to assert
the wrong number); with the fix landed, the both-paths fixture is
`tests/fixtures/effect-multishot-nontail-resume-inner-handle/`.

## Impact

High: silent wrong answers, in the composition the effects guide recommends
(nested handlers instead of transformer stacks). A user who follows
`effects-vs-monads.md`'s "nest handlers -- no lifting" advice and folds a
multishot continuation over work that touches an inner handler gets a number,
not an error.

## Execution (2026-08-05)

### Layer 1 -- the tail-resume yield escaped `dk_invoke` (FIXED)

Reading the emitted C for the repro made the mechanism exact. The inner `Log`
handler is installed with `dk_handler_tail`, so its case ends in
`dk_tail_resume`, which is `longjmp(*g_dk_driver, 1)`. The outer `Ask` case
runs its resumes through `dk_invoke`. `g_dk_driver` named the **program entry**
landing, so that longjmp unwound straight past `dk_invoke` *and past the
handler case that called it* -- the second `resume` and the `+` never ran, and
the value the driver eventually produced became the program's result. That is
precisely "the rest of the clause is discarded".

`dk_invoke` now installs its own landing and runs the trampoline bounded by the
meta-stack watermark it captured at entry, restoring the previous landing on
exit (`__dk_drive_bounded`, `src/compiler/emit_dk_runtime.c`). Deliveries
queued during the invoked run drain inside the invoke; anything an outer level
queued stays for that level. A nested trampoline must not steal an outer
landing -- that is true regardless of anything below, so this fix stands on its
own and would still be needed after layer 2 is fixed.

The E7 fast path is untouched: a tail resume reached with **no** intervening
`dk_invoke` still yields all the way to the entry driver, so deep effectful
recursion stays flat. Verified by `cps-tramp-resume-deep-1m` (the 1e6-deep
fixture) still passing.

Because the emitted preamble changed, 141 `expected.c` snapshots were
regenerated in the same change, per CLAUDE.md, along with the three
`src/runtime/generated/` split artifacts (`tools/gen-runtime-split.py`).

### Layer 2 -- copied chains escape through baked frame envs (FIXED)

With layer 1 fixed the clause runs to completion -- and the repro prints **two
lines**, `2` then `20`. The outer `handle`'s continuation (the `println`) runs
**once per resume**.

The emitted chain says why:

```c
DK *__h0 = dk_hgroup(dk_handler(2, main_hc0_0, 0,
               dk_frame(main_hk0, (intptr_t)__kont, ...)));
DK *__h1 = dk_hgroup(dk_handler_tail(3, main_hc1_0, 0,
               dk_frame(main_hk1, (intptr_t)__h0, ...)));   /* <- env = __h0 */
```

A handle's continuation is a `DKK_FRAME` whose **env is a raw pointer to the
enclosing chain**, and the frame body jumps to it explicitly
(`main_hk1` is `return dk_run(__kont /* = __h0 */, v)`); the node's own `next`
carries only transparent handler markers. So the continuation is an *explicit
jump*, not a chain link.

`dk_copy_range` copies `fn` and `env` verbatim. A resumed copy of the chain
therefore still jumps to the **original** `__h0` -- escaping the delimiter and
re-entering the real outer continuation, printing, once for every resume.

This is **pre-existing and independent of E7**, established by making the inner
handler resume in NON-tail position (`(+ 0 (resume k2 ...))`), which takes
`dk_perform`'s inline path and never touches `dk_tail_resume`:

| | before layer-1 fix | after |
| --- | --- | --- |
| inner handler tail-resumes (the repro) | `2` | `2` `20` |
| inner handler resumes non-tail | `2` `20` | `2` `20` |

The non-tail variant is unchanged by the fix -- it was already exhibiting layer
2 alone. What the fix did is make the tail-resume path agree with it, which is
the correct convergence: the E7 escape had been *masking* the deeper defect by
unwinding before the second resume could expose it.

### The two-spine problem, and the fix that resolved it

A first attempt (make the continuation a chain link by giving the DK *node*
the jump -- `dk_frame_kont` with the jump cleared by a reifying copy) was
built, measured, and reverted: it computed the right value (22) but severed
normal delivery, because the obstacle is structural --

**The chain had two spines that did not agree.** `main__cps` built

```c
__h0 = hgroup(handler(Ask, case, frame(main_hk0, ..., dk_copy_enclosing_handlers(__kont))))
__h1 = hgroup(handler_tail(Log, case, frame(main_hk1, ..., dk_copy_enclosing_handlers(__h0))))
                                                          /* ^ MARKER COPIES */
```

`__h1`'s tail was not `__h0` -- it was a copy of `__h0`'s handler *markers*,
terminated by `dk_done()`. So:

- the `next` spine, which `dk_perform` walks to find `H` and which
  `dk_copy_range` follows, reached only marker copies and dead-ended at `done`;
- the real continuation (`main_hk0`, which prints) was reachable **only** via
  the baked `dk_run(__h0, v)` jump in the frame's body.

Before any fix, the outer continuation ran *inside the resumed
sub-continuation* by falling through `main_hk1`'s jump into `__h0` -- which is
precisely the same jump that re-ran it once per resume. **The bug and the
mechanism that made the normal case work were the same line.** Removing the
jump alone fixes the multishot case and severs the single-shot one. The
marker copies existed for **ownership**: each handle chain is
`__dk_reap_keep`'d and `dk_free` walks `->next`, so linking a live enclosing
chain in as the tail would have freed it twice.

**The landed fix unifies the spines** (`emit_dk_runtime.c` +
`emit_cps_ir.c`'s `emit_handle`), and turned out to need no new frame kind --
the runtime already had the right one:

1. **The handle-continuation frame is now a `DKK_RESUME_FRAME`** (lifted as
   `LH_RESUME_CONT`, installed with `dk_frame_resume_borrow`). A resume-frame
   receives its downstream chain **at run time** -- `dk_run_impl` passes the
   node's own `->next` -- instead of reading a pointer baked into its env at
   install time. So the ORIGINAL frame threads the real enclosing chain, and
   a `dk_copy_range` COPY threads the copy's own marker-terminated tail:
   exactly the property that makes a resumed sub-continuation stay inside its
   delimiter while normal completion still reaches the rest of the program.
   The env carries scalar captures only -- no `__k` slot to go stale.
2. **The frame's `next` is the actual enclosing chain, borrowed.** A new
   `borrow_next` flag on the DK node marks "->next belongs to another chain":
   `dk_free` frees the node and stops (and never reads the possibly-dead
   tail), while every walk (`dk_perform`'s search, `dk_copy_range`,
   `dk_copy_enclosing_handlers`, `__dk_delivery_noop`) crosses it like any
   link. `dk_copy_node` deliberately does not copy the flag -- a copy owns
   everything it copied, including nodes past the original's ownership
   boundary.

With one spine, `dk_perform` finds the **real** `H`, captures
`dk_copy_range(k, H)` up to the real boundary, and delivers
`dk_run_impl(H->next, r)` through the **real** rest of the program -- exactly
once, after the clause has finished all its resumes. The E7 flatness guarantee
survives: only the first perform (against the real chain) queues a real
delivery; every subsequent perform inside a resumed copy sees the copy's
marker-terminated tail, so its delivery is a no-op and stays elided
(`cps-tramp-resume-deep-1m` still passes).

The repro prints **22** and returns 0; the weighted variant prints **2020**;
the non-tail-inner and arg-position variants print **22**; the single-resume
variant stays **14** -- all agreeing with `tur --interpret` and hand
evaluation. Because the emitted preamble changed, all 141 `expected.c`
snapshots were regenerated in the same change, along with the
`src/runtime/generated/` split artifacts. Both suites are green:
`tests/run.sh` 2571/0, `tests/run-turi.sh` 1758/0/705.

### Where the truth is pinned now

`tests/fixtures/effect-multishot-nontail-resume-inner-handle/` runs the repro
and all four boundary variants on **both** paths (the both-paths fixture this
report asked for). `tests/fixtures/turi-ws-driven-operands` continues to pin
the interpreter-side driving specifics.
