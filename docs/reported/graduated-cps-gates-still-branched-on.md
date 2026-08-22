# Two graduated experiments left their `g_opt_*` gate bits and ~70 dead branches behind

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
