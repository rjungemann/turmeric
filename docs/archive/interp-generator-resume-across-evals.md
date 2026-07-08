# Generator resume is corrupted by intervening top-level evals

**Status:** RESOLVED. The root cause was top-level-form *re-evaluation* in
`turi_eval`, not a coroutine/context problem. `turi_eval` re-elaborates the
accumulated source each call and skips the already-run forms, but it keyed the
skip boundary off the *parsed-form* count (`prior_toplevel`). A `(load ...)`
form expands inline to ~10 extra *program items*, so parsed-form index and
program-item index diverge: the boundary `n_fsd + prior` landed short of the
truly-new form and previously-run top-level forms were evaluated again. Once a
prior `(gen-next g)` fell inside that re-run window, each drain advanced the
suspended generator an extra step, producing `10 30 0` for a 10/20/30
generator. Fix: track already-run *program items* in a new
`env->prior_prog_items` (updated to `total - n_fsd` on each successful eval)
and use `[n_fsd + prior_prog_items, total)` as the new-forms range. Regression
test: `test_gen_drain_across_evals` in `tests/turi/env-longlived.c`. Touched
`src/turi/{eval.c,env.c,env.h,repl.c}` and `src/web/wasm_glue.c`.

**Severity:** medium (wrong results, silent). Pre-existing; **independent of
scratch promotion** (reproduces with promotion off).

## Summary

A generator defined in one `turi_eval` top-level call and drained across later
calls yields correct values only if nothing else is evaluated in between. If any
unrelated top-level eval runs before/between `gen-next` calls, the generator's
suspended coroutine resumes at the wrong point: the first yield is still correct,
but subsequent yields are skipped/garbled.

## Repro (libturi, `turi_eval`)

```c
TuriEnv *e = turi_env_new();                       /* promotion OFF (default) */
turi_eval(e, "(load \"stdlib/gen.tur\")\n"
             "(def g (gen [] (yield 10) (yield 20) (yield 30)))\n0");

/* no intervening eval -> correct */
turi_eval(e, "(gen-unwrap (gen-next g))");   /* 10 */
turi_eval(e, "(gen-unwrap (gen-next g))");   /* 20 */
turi_eval(e, "(gen-unwrap (gen-next g))");   /* 30 */

/* BUT with an intervening eval before draining: */
for (int i = 0; i < 50; i++) turi_eval(e, "(let [z 0] z)");
turi_eval(e, "(gen-unwrap (gen-next g))");   /* 10  (ok) */
turi_eval(e, "(gen-unwrap (gen-next g))");   /* 30  (WRONG, want 20) */
turi_eval(e, "(gen-unwrap (gen-next g))");   /* 0   (WRONG, want 30) */
```

Observed sequences (sequenced statements, so not an arg-eval-order artifact):

| promotion | intervening evals | drain |
| --- | --- | --- |
| off | none | `10 20 30 0` (correct) |
| off | 50   | `10 30 0 0` (wrong) |
| on  | none | `10 20 30 0` (correct) |
| on  | 50   | `10 30 0 0` (wrong) |

Promotion on/off are identical, confirming this is a base-interpreter defect, not
a value-pool issue.

## Likely cause (not yet root-caused to a line)

The generator's suspended `ucontext`/coroutine stack is not surviving intervening
top-level evaluation. Candidates: the coroutine stack or `caller_ctx` is
clobbered by the next `turi_eval`'s use of the main C stack, or the
accumulated-source re-evaluation (`env->src_acc` replay each `turi_eval`)
disturbs the generator's captured frame or the `g_current_gen` /
`g_pending_gen` thread-local side channels. The first-yield-correct,
later-yields-wrong shape points at a resume-context problem rather than a
value-copy problem.

## Impact / relationship to other work

- Blocks a clean end-to-end fixture for generator *drain* across churned evals in
  `tests/turi/env-longlived.c`; the carrier-relocation Part 2 test there asserts
  the relocated generator's **first** yield and the started-generator conservative
  bail instead, precisely because full cross-eval drain is unreliable here.
- Orthogonal to scratch promotion
  (`docs/upcoming/turi-value-pool-carrier-relocation-plan.md`) -- promotion
  neither causes nor worsens it.

## Fix directions

1. Root-cause with a two-eval minimal repro (one intervening `(let [z 0] z)`),
   bisecting between the src_acc replay path and the coroutine save/restore.
2. If it is the coroutine context, ensure `gen_ctx`/`caller_ctx` and the stack
   are fully independent of the main eval C stack across `turi_eval` boundaries
   (they should be, given the dedicated mmap stack -- so re-check the thread-local
   `g_current_gen`/`g_pending_gen` handoff and any src_acc re-run of the `(gen
   ...)` defining form, which would rebind `g` to a fresh generator).
