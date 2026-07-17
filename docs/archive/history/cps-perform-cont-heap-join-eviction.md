# Fix: perform-continuation heap-join eviction + the full effect-reopen DK slice

Resolves `docs/archive/cps-perform-cont-heap-join-eviction.md`.

## Summary

`effect-reopen` (a compound corpus fixture: nested handle, effect re-opening,
a colored call inside a perform continuation) evicted `BODY-STRUCT-OR-TAINT`
under `--enable=cps-tramp-resume` and ran on the fiber.  Root-causing its
admission uncovered THREE gaps, fixed together so the whole fixture DK-lowers.
All changes gated on `g_opt_cps_tramp_resume` -- flag-off codegen is
byte-identical (verified: `dk_hgroup`/`hgroup` absent from default emit).

## The three gaps (in the order the admission trace surfaced them)

1. **`println` in a perform continuation was rejected.**  `perform_body_ok` and
   `perform_cont_reset_ok` (`emit_cps_ir.c`) each did
   `if (... || is_println_shape(...)) return false;` -- yet `handle_case_ok`
   ADMITS println (emit_lifted emits the frame body through the same `emit_term`
   path a handler case uses).  Stale conservatism from before the case-body
   println path.  Fix: admit println under the flag (a multi-shot resume re-runs
   the frame and prints again -- the correct semantics, same as a re-run handler
   case).

2. **A heap join whose jbody PERFORMS emitted an undeclared `__kont`.**  A
   non-tail cps->cps call in a perform continuation reifies a `CT_LETCONT` heap
   join; `emit_heap_join` lifts it as a value-only `LH_PERFORM_CONT` frame
   `(env, value)` UNLESS `needs_kont` -- which was
   `jbody_has_cps_tailcall(jbody)` only.  A jbody that PERFORMS (rather than
   tail-calls) also needs `__kont` (the interior `dk_perform` threads cur_k), so
   the perform lowered `dk_perform(..., __kont)` inside a frame with no `__kont`
   param -> `'__kont' undeclared`.  Fix: `jbody_has_perform` -> `needs_kont` also
   true when the jbody contains a `CT_PERFORM`, promoting the frame to
   `LH_RESUME_CONT` (which receives `__kont`).  Verified on the report's minimal
   single-effect repro (`142`) and a single-handle Log+Counter shape
   (`start`/`done`/`142`).  This is an ungated-safe correctness fix (no default
   fixture reaches a performing-jbody heap join -- one would already emit broken
   C), but it only pays off with gap 1 (the shapes all print).

3. **Effect re-opening broke when the re-opening perform's continuation was
   multi-suspension.**  With gaps 1-2 fixed, `effect-reopen` ADMITTED but aborted
   at runtime (`unhandled effect`).  Bisected to a pre-existing defect in the
   re-opening DK runtime (the `dk_case_enclosing` machinery from
   `docs/archive/cps-handler-case-effect-reopening-needs-emission.md`), dormant
   because `effect-reopen` had always evicted: it never ran the re-opening DK
   path.  Minimal repro = two sequential re-opening performs (`Test C`): the
   FIRST works, the SECOND's re-opened effect escapes.

   Root cause: `dk_case_enclosing` skips "the maximal run of consecutive
   `DKK_HANDLER` nodes" to find the handlers OUTSIDE this handle.  But
   `dk_perform`'s deep re-install (and `dk_copy_enclosing_handlers`) FLATTENS the
   chain -- it drops the continuation frames BETWEEN distinct handles -- so after
   a resume the inner handle's handler and the enclosing handle's handler become
   ADJACENT `DKK_HANDLER` nodes.  `dk_case_enclosing` then skips BOTH, so a
   re-opened outer effect performed in the resumed continuation looks PAST its
   handler -> unhandled.  (Single-perform re-opening works because the chain
   still has the intervening frame.)

   Fix: a per-handle-instantiation group id.  `dk_hgroup(chain)` (runtime, gated)
   stamps the maximal consecutive-`DKK_HANDLER` run -- exactly one handle's
   sibling cases, since the run ends at that handle's continuation frame -- with
   a fresh shared `hgroup`.  `dk_copy_node` copies `hgroup`.  `dk_case_enclosing`
   and `dk_perform`'s re-install run then skip only handlers with H's OWN
   `hgroup`, stopping at a different-group handler (an enclosing handle).  Robust
   for any nesting depth.  `emit_handle` wraps the installed chain in
   `dk_hgroup(...)` under the flag.

## Verification

- `effect-reopen`: no `eff=1` eviction (fully DK); output `start`/`done`/`142`
  matches `expected.stdout`.
- Bisection fixtures (all correct): `Test A` (single handle, heap-join-perform,
  no re-opening), `Test B` (nested re-opening, Counter inline), `Test C` (two
  sequential re-opening performs), minimal single-effect repro (`142`).
- Regression fixture `tests/fixtures/cps-tramp-resume-reopen` (flag-on).
- Default suite green (flag-off byte-identical).  Flag-on soundness sweep clean.

## Known follow-up (NOT this fix)

The re-opening DK path leaks DK nodes O(N) per re-opened perform (the trampoline
yield path's `sub` chain is not freed for a re-opened perform hitting a
tail-resume handler).  Pre-existing in the re-opening machinery (the earlier
`ffd878897` commit); surfaced now that `effect-reopen` runs the path
end-to-end.  Compiled fixture binaries are not ASan/leak-checked by the suite,
so it blocks no gate.  Filed separately:
`docs/reported/cps-reopen-perform-onode-leak.md`.
