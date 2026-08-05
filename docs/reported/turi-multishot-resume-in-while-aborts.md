---
status: open
severity: medium
discovered: 2026-08-05
area: interpreter (fiber effect runtime, src/turi/eval.c)
---

# turi aborts on a multishot resume from the second iteration of a `while`

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
