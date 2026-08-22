# Two graduated experiments left their `g_opt_*` gate bits and ~70 dead branches behind

> **RESOLVED 2026-08-22.** Both bits are deleted and all 69 reads folded to
> `true` with their unreachable arms removed, across the eight files below plus
> `emit_dk_runtime.c` (see "What the fix actually touched"). `tests/run.sh`
> 2693 passed, 0 failed, and no `expected.c` snapshot moved -- the folds are
> emission-identical, which is the check the plan below asked for.

**Severity: medium (no runtime defect; carrying cost + a misleading invariant).**
Found 2026-08-22 while aging out the `GRADUATED[]` shims.

## Summary

`cps-tramp-resume` (graduated 2026-07-19) and `owning-cloneable-capture`
(graduated 2026-07-20) both retired their `EXPERIMENTS[]` rows but kept their
enable bits:

```c
/* src/runtime/globals.c */
bool g_opt_cps_tramp_resume       = true;
bool g_opt_owning_cloneable_capture = true;
```

Nothing ever writes either one. `experiment_enable` assigns `*d->opt_global`
only for names present in `EXPERIMENTS[]`, and both rows are gone, so there is
no CLI, manifest, or user-config path that can reach them -- the initializer is
the only assignment in the tree. Both are therefore compile-time constants, and
every conditional reading them is a constant test with one unreachable arm.

This is the opposite of what the other graduations did. Every sibling in
`globals.h` records the bit as deleted -- "the `g_opt_refined` enable bit and
its elaboration gates are gone", and likewise for `sealed-opaque`,
`write-frames`, `checked-reads`, `cycle-gc`, `jit`, `closure-drop-glue`. These
two are the exceptions, and they are the two with by far the most gate sites.

## Scale

**64** live reads of `g_opt_cps_tramp_resume` and **5** of
`g_opt_owning_cloneable_capture`, across eight files:

```
src/passes/cps_ir.c          src/compiler/emit_module.c
src/passes/cps.c             src/compiler/emit_expr.c
src/compiler/emit_cps_ir.c   src/compiler/emit_effects.c
                             src/compiler/elab_effects.c
                             src/compiler/elab_toplevel.c
```

## Why it is worth doing

The dead arms are not inert -- they actively mislead, because they read as live
alternatives:

- `src/compiler/emit_cps_ir.c:1716` excludes a println shape when
  `!g_opt_cps_tramp_resume`, and its comment explains the exclusion exists "so
  the default (shipping) config's perform-continuation admission stays
  byte-identical". There is no such config any more: the branch it protects is
  unreachable and the comment describes a world that ended at graduation.
- `emit_cps_ir.c:1773` says the predicate is "already flag-gated ... so no extra
  gate needed -- but keep the historical exclusion for the flag-off path".
  There is no flag-off path.
- `emit_cps_ir.c:8733` computes `bool experimental_surface = g_opt_cps_tramp_resume;`
  and exempts that surface from a hard error. Spelled as written, the exemption
  is unconditional -- a hard error nothing can now reach. This one is worth
  reading closely during the cleanup rather than folding mechanically.

Similar stale "under `--enable=...`" prose sits at `emit_cps_ir.c:2738`,
`emit_module.c:11708`, `types.h:686`, and four places in `stdlib/httpd.tur`
(the last describing `closure-drop-glue`, whose bit *was* correctly deleted).

## Fix directions

Constant-fold both bits to `true` and delete the unreachable arms, then remove
the two declarations from `globals.{c,h}` and update the `experiments.c`
tombstones, which currently say "defaults true" where every other graduated row
says the bit is gone.

The fold is provably behaviour-preserving -- the value cannot vary at runtime --
so the suite is a strong check on the mechanics. Two cautions:

1. Do not sed it. `!g_opt_X ||`, `g_opt_X &&`, and the ternary at
   `emit_cps_ir.c:1319` / `:6322` each fold differently, and
   `emit_cps_ir.c:8733` above changes what a reader thinks the code guarantees.
2. Expect `expected.c` snapshot churn only if a fold changes emission. It should
   not: the taken arm is the one that already runs today.

Best done as its own change, not as a rider -- it touches the two hairiest files
in the backend and wants the suite green on its own diff.


## What the fix actually touched (2026-08-22)

Beyond the 69 sites, three things the report did not anticipate:

1. **`emit_cps_runtime_prelude_ex(Buf *, bool tramp)`.** The only caller passed
   the always-true bit, and the `tramp == false` wrapper
   (`emit_cps_runtime_prelude`) had no callers at all -- so the parameter was
   dead by the same argument as the bit. Collapsed into a single
   `emit_cps_runtime_prelude(Buf *out)` with the trampoline machinery
   unconditional, 11 `if (tramp)` sites unwrapped and 4 `else` arms deleted.

   One of those `else` arms is worth recording, because hoisting it out was a
   real regression caught only by the snapshots: the `tramp == false` arm also
   emitted an `__dk_abort_body` helper that the `tramp == true` arm did not.
   Lifting it unconditionally added a new function to every preamble and moved
   146 fixtures. It is referenced nowhere in the emitter and appears in zero
   `expected.c` files -- i.e. it has not been emitted since the graduation --
   so it was deleted with its arm. **A dead-arm deletion is not always a
   subset: check whether the arm you are dropping emits anything the surviving
   arm does not.**

2. **`emit_cps_ir.c:8733`'s `experimental_surface`,** flagged in the report as
   needing eyes. Folding it proves the N6.5 hard error is unreachable: the
   exemption was `= g_opt_cps_tramp_resume`, always true, so
   `!sig_perm_route && !experimental_surface` never held. The diagnostic and
   the now-unread `sig_perm_route` flag are deleted, with a comment recording
   how to restore it (narrow the exemption to SIG-AWAIT-RECURSE rather than the
   whole CPS surface). `TUR_TRACE_EVICT` still prints every eviction with its
   category, so nothing became invisible.

3. **A second-order cascade, left open.** Folding the `EX_HANDLE` arm in
   `cps_ir.c` removed the only writer of `g_wbd_handled`, which leaves the P5
   whole-body handle-delegation subsystem inert. That is a bigger deletion than
   a flag fold and wants its own analysis, so it is filed separately as
   `docs/reported/wbd-handle-delegation-subsystem-inert.md`.
