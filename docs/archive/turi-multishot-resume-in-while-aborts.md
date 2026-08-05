---
status: RESOLVED 2026-08-05
severity: was medium
discovered: 2026-08-05
area: interpreter (work-stack driver + fiber fallback, src/turi/eval.c)
---

# turi aborts on a multishot resume from the second iteration of a `while`

> **RESOLVED 2026-08-05.** The driver now descends `while` on its work-stack
> (`DK_WHILE`), so the loop is no longer a black box and a handle containing
> one stays on the multi-shot path. The report's hypothesis below was wrong
> about the mechanism -- nothing was per-iteration, and the fiber's snapshot
> logic was never involved. See [Resolution](#resolution-2026-08-05).

## Summary

Under `tur --interpret`, a `^multishot` handler clause that resumes its
continuation from inside a `while` loop aborts the interpreter on the loop's
**second** iteration. The same resumes written straight-line work, and a loop
that performs only **one** resume works, so this is specifically
loop-iteration-two-onward.

The abort site is `eval_body_thunk` (`src/turi/eval.c:1208`): the `abort()`
guard after `swapcontext(&cont->body_ctx, &cont->handler_ctx)`, i.e. a
completed body fiber was context-switched into again and fell off the end of
its thunk.

Found 2026-08-05 while landing compiled-path support for exactly this shape
(CT_LOOP in a handler case; see
[handler-clause-statement-if-ices-emitter](handler-clause-statement-if-ices-emitter.md)).
The compiled path runs all of the repros below correctly, so this is now a
**compiled/interpreted divergence** on the multi-shot fold -- the direct
expression of bounded nondeterminism. `tests/fixtures/effect-multishot-resume-in-loop`
carries `requires.compiled` because of this report; removing that marker is
part of fixing it.

## Repro

Aborts (prints nothing, exits via `abort()`):

    (defeffect Choose [lo : int hi : int] : int)
    (defn main [] : int
      (println (handle (perform (Choose 1 3))
                       (Choose [lo hi] ^multishot k)
                       (let [^mut a 0 ^mut i lo]
                         (while (<= i hi) (set! a (+ a (resume k i))) (set! i (+ i 1)))
                         a)))
      0)

    $ tur --interpret repro.tur    # abort() in eval_body_thunk

The pinpointing pair -- all of these work:

    ;; same three resumes, straight-line: prints 6
    (Choose [lo hi] ^multishot k) (+ (resume k lo) (+ (resume k 2) (resume k hi)))

    ;; resume in a while, but only ONE iteration: prints the value
    (Choose [lo hi] ^multishot k)
      (let [^mut a 0 ^mut i lo]
        (while (< i 2) (set! a (+ a (resume k i))) (set! i (+ i 1)))
        a)

So multishot itself is fine, resume-in-while is fine once, and sequential
resumes are fine three times. The abort needs `while` + a resume on an
iteration after the first.

## Root cause (hypothesis, not pinned)

`eval_body_thunk` runs the handled body on its own ucontext fiber and
swapcontexts back to the handler when done; the `abort()` fires only if
something re-enters the finished fiber. A multishot resume is supposed to
re-run the body against a fresh (or snapshotted) fiber. The first in-loop
resume works and the second aborts, which smells like per-iteration state --
the loop's second iteration resuming through a continuation whose fiber was
already driven to completion by the first, where the straight-line case
re-snapshots correctly. The `while` evaluation in the clause presumably
routes the second resume at the *same* saved continuation object rather than
a fresh snapshot. Not verified beyond the symptom table above; start at how
`TuriEffectCont` snapshots interact with the loop special form in
`eval_expr`.

## Impact

The multi-shot fold -- fold a continuation over a range -- silently cannot be
run under `--interpret`, `tur repl`, or anywhere else turi backs. "Silently"
is the problem: the failure is a bare `abort()` with no message, which under
the test harness reads as a crash and at a REPL kills the session.

Even before a fix, a located interpreter error ("multishot resume from a loop
is not supported under --interpret") would turn the divergence from a crash
into a stated limitation.

## Resolution (2026-08-05)

### The hypothesis above was wrong

"Per-iteration state ... the loop's second iteration resuming through a
continuation whose fiber was already driven to completion by the first" reads
the symptom table correctly and then guesses the wrong mechanism. Nothing here
is per-iteration, and `TuriEffectCont`'s snapshotting was never involved. The
pinpointing pair that *looked* like "iteration two" was really "one resume vs.
two" -- a one-iteration loop performs exactly one resume, which any single-shot
continuation survives.

The real split is **which of turi's two effect engines ran the handle**, and it
is decided statically, before the loop executes at all:

- turi has a work-stack driver (`eval_drive_ex`) where a `handle` installs a
  `DK_PROMPT` and a captured continuation is a heap-owned slice that
  `clone_ws_slice` copies per resume. **That path is genuinely multi-shot.**
- It also has a ucontext fiber fallback (`eval_handle`) where the continuation
  *is* the fiber. That path is **single-shot by construction**: once the body
  runs to completion there is no state left to re-enter.

`ws_capturable` picks between them, and it had no `EX_WHILE` arm. So it fell to
`default: return !ws_has_perform(e)`, and `ws_has_perform` had no `EX_WHILE`
arm either -- its default answered "conservatively assume a perform may run
synchronously". A `while` anywhere in the handle's body or in any clause
therefore forced the whole handle onto the one-shot fiber. The straight-line
triple resume worked because that clause has no `while` in it at all.

The conservatism was correct as written: `EX_WHILE` really *was* a black box.
It was evaluated only in `eval_expr_impl`, as a plain C `while` calling
`eval_expr` per iteration, so a `perform` or `resume` inside the loop never
reached the driver's descending switch and never saw the enclosing
`DK_PROMPT`.

### The fix

`EX_WHILE` is now driven on the work-stack. `DK_WHILE` holds the loop
expression, the enclosing frame, and a one-bit phase (`index`): phase 0 means
the value that just came back is the condition's, phase 1 means it is the
body's. Every iteration re-descends from that one frame, so the loop costs O(1)
work-stack depth however long it runs. The two static analyses follow: both
`ws_capturable` and `ws_has_perform` now answer for a `while` from its parts,
exactly as they do for an `if`.

The point was never depth -- the C loop was already flat -- it was
transparency. With the loop descended, a `resume` inside it lands in the driver
with the prompt visible, and the multi-shot fold runs.

`DK_WHILE` needed no `clone_ws_slice` support: it carries no owned frame and no
heap accumulator, so a slice containing one copies correctly by the existing
rules. That is not an assumption -- it is covered by a fixture case where the
`perform` is *inside* the loop in the handle body and a `^multishot` clause
resumes twice, so the captured slice really does contain a loop frame and
really is cloned per resume. Compiled and interpreted both print 44.

The `DK_WHILE` transitions mirror `eval_expr_impl`'s loop exactly, including
its asymmetry: the condition bails on any signal (`env_signaled`), the body
bails only on an error or a `return`, so a `throw` / `abort` / `panic` raised
in the body propagates through the enclosing frames instead of being swallowed
by the loop. Verified against the compiled path on early-`return`-out-of-loop,
nested loops, zero-trip, and a `panic` caught by an enclosing `catch-unwind`.

### The fallback still exists, and no longer aborts

Making `while` transparent removes the reason *this* program hit the fiber, but
the fallback is still reachable -- a handle whose body or clause reaches its
`perform` through a native higher-order call, a `catch-unwind` thunk, or a
match scrutinee is still not capturable. Resuming such a continuation twice
still has nowhere to go, and `swapcontext` into the finished fiber still lands
past the end of `eval_body_thunk`, whose only recourse was `abort()`.

`eval_resume_cont` now refuses up front when `cont->done`, with an error naming
the situation and the shapes that stay on the multi-shot path.
`tests/fixtures/errors/turi-multishot-resume-past-fiber-body` pins it.

That fixture is `requires.interp-only`, which `run_negative` in `tests/run.sh`
did not previously honor (the happy path did) -- one guard added, mirroring the
`requires.spices` / `requires.posix-apis` pair already duplicated between the
two functions. It matters here because the compiled path rejects the same
program for an entirely unrelated reason (a `perform` inside a `catch-unwind`
thunk is outside the CPS backend's subset), so its `expected.diag` is not a
claim the compiled suite should be checking.

An earlier draft of that error message told the reader the compiled path "has
no such restriction". Checking it against `tur run` showed that is false for
the very program the fixture uses -- the two paths have *different* admissible
subsets, not nested ones. The shipped message says what keeps a handler
multi-shot instead of promising anything about the other backend.

### Not fixed (pre-existing, unrelated)

`(set! x (+ x (perform ...)))` -- a `set!` whose VALUE performs -- is not
capturable, because `ws_capturable`'s `EX_SET` arm requires a perform-free
value. This has nothing to do with loops (it reproduces with no loop at all)
and the compiled path rejects the same shape too. It was noticed while testing
this fix and left alone.

### Coverage

`tests/fixtures/effect-multishot-resume-in-loop` lost its `requires.compiled`
marker and now runs under both harnesses, byte-identical: the fold over a
range, the fold through a non-`main` function called twice, and the
capture-inside-a-loop case above.
