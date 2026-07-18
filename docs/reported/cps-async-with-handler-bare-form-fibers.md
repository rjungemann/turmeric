# `(async (with-handler ...))` bare form fibers its interior handle

**Severity:** low (experimental `--enable=cps-tramp-resume` fiber-liveness residual;
correctness is fine -- runs on the fiber, output 15).

## Summary

`async-with-handler` -- `(async (with-handler (perform (AddTen 5)) (AddTen [x] k)
(resume k (+ x 10))))` -- keeps its interior handle on the FIBER machine
(`__handle_body_*` / `__dispatch_*` / `tur_effect_cont_resume`) under
`--enable=cps-tramp-resume`, so it emits one `tur_effect_perform` call site.

Its sibling `effects-async` -- the SAME program with an explicit `(fn [] ...)`
wrapper: `(async (fn [] (handle (perform (AddTen 5)) (AddTen ...))))` -- now
DK-lowers to perform=0 (self-handling-lambda fix, commit 8501075).

## Root cause

`(async EXPR)` stores `EXPR` verbatim as `async_.fn_expr` (elab_concurrent.c
`elab_async`).  When `EXPR` is already a lambda (`effects-async`), that lambda is
a lifted colored fn that CPS-emits its interior handle on the DK.  When `EXPR` is
a BARE handle / with-handler (`async-with-handler`), the async region is delegated
wholesale to the direct emitter (`cps_ir.c` EX_ASYNC -> `build_letraw`), which
emits the handle inline via the fiber effect runtime -- there is no colored lambda
to CPS-lower.

Verified equivalence: rewriting the fixture to `(async (fn [] (with-handler
...)))` DK-lowers to perform=0, output 15 -- so the ONLY gap is the missing
explicit thunk wrapper.

## Fix directions

Normalize `(async EXPR)` to `(async (fn [] EXPR))` when `EXPR` is not already a
fn/closure, so the bare form takes the same colored-lambda path.  This is a BROAD
async-lowering change, not a local one:

- It changes async codegen for EVERY bare-expr async site (flag-off included
  unless gated), churning async fixture snapshots.
- The synthesized lambda's captures would then flow through the async Send-check
  (`elab_concurrent.c`, TUR-E0010 / TUR-E0017 / TUR-E0022).  The bare-expr path
  currently skips that check; wrapping would apply it, which may turn currently-
  passing async bodies into `not Send` / continuation-escape errors (a latent
  soundness gap the wrap would also CLOSE, but a behavior change nonetheless).

Because of the Send-check interaction and snapshot breadth, this belongs with the
`cps-async` experiment (`g_opt_cps_async`) rather than a local B5 slice -- the
async region itself wants a coherent CPS lowering, at which point the interior
handle composes with it.  Flag-off / fiber behavior is correct today.
