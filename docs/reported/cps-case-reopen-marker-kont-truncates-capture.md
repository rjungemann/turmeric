---
status: open
severity: high (silent wrong answer in one shape; silently lost output + wrong exit code in another)
discovered: 2026-08-05
area: compiler (CPS/DK runtime: dk_case_enclosing marker chain; dk_perform inline delivery)
---

# A handler case that re-opens an outer effect gives that effect a marker-copy continuation, truncating its capture and stranding the pending delivery on the C stack

## Summary

This is the **remaining instance of the two-spine problem** after
[cps-multishot-nontail-resume-inner-handle-drops-clause-rest](../archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md)
unified the handle chain's spines. Found 2026-08-05 by auditing the runtime
for other places where a stand-in spine substitutes for the real chain,
immediately after landing that fix. **Pre-existing** -- verified byte-identical
behavior on the pre-unification compiler; the unification neither caused nor
fixed it.

When a handler case **re-opens an outer effect** (performs an effect its own
handle does not handle), the case's `__kont` is
`dk_case_enclosing(g_dk_case_reopen_hnode)` -- a fresh, done-terminated copy of
the enclosing **handler markers only**. Two consequences, both observed:

1. **Multishot truncation** (wrong number): the outer effect's `dk_perform`
   finds its handler H as a *marker copy*, so `dk_copy_range(k, H)` captures
   only the case-local frames. Everything between the re-opening handle and
   the outer handle -- the inner handle's delivery, any interposed frames --
   is NOT in the capture and runs **once** (by C-stack return propagation)
   instead of once per resume.
2. **Stranded delivery** (lost output, wrong exit code): the inner
   `dk_perform`'s own pending delivery (`dk_run_impl(H_in->next, r)` after the
   case returns) exists only as a C-stack continuation. If the outer handler
   **tail-resumes**, `dk_tail_resume` longjmps to the entry driver and that C
   frame is unwound; the outer perform's own queued delivery was elided as a
   no-op (its marker chain is `[done]`), so nothing ever runs the real rest.
   The program produces **no output** and the in-flight value escapes as
   `main`'s exit code.

The C stack is acting as a third spine, and both failure modes are it
disagreeing with the chain.

## Repro

```turmeric
(defeffect In [] : int)
(defeffect Out [] : int)
(defn main [] : int
  (println (handle
             (+ 1000 (handle (+ 7 (perform (In)))
                       (In [] k2) (resume k2 (perform (Out)))))
             (Out [] ^multishot k) (+ (resume k 1) (resume k 10))))
  0)
```

`k_out = (fn [v] (+ 1000 (+ 7 v)))`, so the answer is `1008 + 1017 = 2025`.
`tur --interpret` prints **2025**. `tur run` prints **1025**: the case-local
part (`7 + v`) ran per resume (8 and 17, summed to 25 in the clause), but the
`(+ 1000 _)` continuation ran **once**, on the final value.

The tail-resume variant is worse -- silent lost output:

```turmeric
;; same program, but: (Out [] ^multishot k) (resume k 7)
```

Expected `1014` printed, exit 0 (interpreter does exactly that). Compiled:
**nothing printed, exit code 14** -- the Out clause's `dk_tail_resume`
longjmped to the entry driver, unwinding the In-`dk_perform` C frame that was
the only owner of the `+ 1000`/`println` delivery.

## Boundary conditions (all verified, both before and after spine unification)

| Variant | Compiled | turi | Verdict |
| --- | --- | --- | --- |
| multishot re-open, frames between the handles (repro) | **1025** | 2025 | compiled wrong |
| weighted clause `(+ (* 1000 (resume k 1)) (resume k 10))` | **9017** | 1009017 | compiled wrong (truncation model: `1000*8 + 17`, one final `+1000`) |
| single TAIL resume of the re-opened effect | **no output, exit 14** | prints 1014, exit 0 | compiled loses the program's output |
| multishot re-open, NOTHING between the handles | 25 | 25 | both right (truncated capture happens to equal the real one) |

So the wrongness needs frames between the re-opening handle and the outer
handler; the lost-output mode additionally needs the outer case to tail-resume.

## Root cause

`emit_lifted` (LH_HANDLER_CASE, `case_reopens`) declares
`__kont = dk_case_enclosing(g_dk_case_reopen_hnode)` at case entry -- a
transparent marker chain built so the case's own value still "returns to the
H->next boundary for dk_perform to thread exactly once" (its design comment).
That single-delivery discipline is load-bearing, and it is exactly what
truncates the capture: the real continuation of a re-opened effect extends
from the perform site through the rest of the case, through the inner
handle's H->next delivery, out to the outer handler -- but the H->next part
is not chain-reachable from the case's `__kont`; it lives in the suspended
`dk_perform` C frame.

Handing the case the REAL enclosing chain was **measured during the audit**
(a throwaway `dk_case_enclosing_real` returning `ge` -- the walk
`dk_case_enclosing` already does, minus the copy; the spine unification makes
`ge` the real spine now). The result is sharp:

- the repro prints **2025** -- the re-opened effect's capture crosses into the
  real chain and its clause delivers through the real `H_out->next`, all
  correct -- **followed by a spurious `1000`**: the inner `dk_perform` still
  runs its own `dk_run_impl(H_in->next, r)` after the case returns, so the
  `+ 1000` continuation runs a second time on the returned-up value;
- the tail-resume variant prints **1014, exit 0** -- fully CORRECT. The
  E7-queued delivery is now a copy of the real `H_out->next` (not an elided
  `[done]`), so the driver runs the real rest, and the longjmp discards the
  In-`dk_perform` C frame whose pending delivery is now redundant anyway.

So the real chain fixes both failure modes and introduces exactly one defect:
the inline path's **second delivery**. The truncation and the single-delivery
guarantee are the same mechanism, just like the parent report's baked jump.

## Fix directions

Move the inline post-case delivery in-chain, at least for re-opening cases
(`case_reopens` is known at emit time): the case's `__kont` becomes the real
`ge` (borrowed, per the measured experiment), every exit of a re-opening case
delivers its value via `dk_run(__kont, r)` (the `LH_RESUME_CONT`-style
discipline the handle continuation now uses) instead of returning it for
`dk_perform` to deliver, and `dk_perform`'s inline branch skips its own
`dk_run_impl(H->next, ...)` for such a case (a `case_delivers` flag on the
handler node, set by a re-opening-aware ctor). Tail-resume cases need no
change on their own path -- their delivery is already queued by `dk_perform`,
and the measurement shows it becomes correct as soon as the chain is real.
Watch: a re-opening case with a non-performing branch (an `if` arm that never
reaches the perform) must still deliver through `__kont` on that arm; and the
`dk_case_enclosing` copy is what today guarantees the case cannot mutate or
free the enclosing chain, so the borrow must stay read-only. This changes the
case-call protocol and touches every existing re-opening fixture -- its own
plan, but a well-scoped one: the measured experiment plus one flag and one
emission-mode change away.

## Where the truth is pinned

`tests/fixtures/turi-case-reopen-outer-capture/` (requires.interp-only)
asserts 2025, 1009017, 1014, and 25 on the interpreter. There is deliberately
no compiled fixture for the broken shapes -- it would have to assert 1025 /
9017 / silent exit 14. When this is fixed, fold those cases into a both-paths
fixture next to `effect-multishot-nontail-resume-inner-handle`.

## Related latent instances (checked, not currently reachable)

- **`emit_reset`** still builds the pre-unification layout (baked `__k` env +
  marker `next`). Not currently exposable: `perform` inside a `reset` under a
  `handle` leaves the CPS subset and the direct emitter reports a located
  "no lowering here" error (verified single- and multi-shot) -- an
  expressiveness gap, not a miscompile. If that admission ever widens, the
  reset frame needs the same `LH_RESUME_CONT` + borrowed-next conversion
  first.
- **`emit_await`** (bounded-continuation path) bakes `__k` with `next =
  dk_done()`. Benign by construction: an await continuation is resumed
  exactly once by the future's completion and no user-visible `k` exists, so
  the baked pointer can never be re-entered through a copy.
- **Plain `shift`/`reset`** never resumes its captured sub (`__dk_abort_body`
  -- Turmeric shift is abortive); multishot shift is the `__Shift` effect
  desugar, which rides the handle chains the unification fixed.
