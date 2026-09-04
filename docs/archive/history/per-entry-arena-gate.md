---
title: Per-entry arena in the refinement solver -- gate result
category: History
description: Measured whether resetting an arena per obligation would bound the solver's memory. It would not -- the only part that grows with obligation count is the one the compiler retains on purpose, and the resettable part is a constant SX3 already removes.
---

# Per-entry arena (compiler side) -- gate result

**Verdict: do NOT build it.** Measured 2026-08-25 with temporary instrumentation
on a Debug build; the instrumentation was reverted and is not in the tree.

**Scope, because the name is ambiguous.** This is about a *compiler-internal*
arena reset, one region per discharged obligation. It is **not** about the
emitted-code arena that
[benchmarks/adt-alloc/RESULTS.md](../../../benchmarks/adt-alloc/RESULTS.md)
row G prices at 7.64x. That one is untouched by this result and remains the
live idea; see the caveat at the end.

## Why it looked promising

The refinement solver threads the **whole compilation unit's** `Arena *` through
every stage -- `refine_vc_build`, `euf_new`, `la_new`, `refine_cubes_build`,
`refine_model_search` all allocate into it -- and nothing is reclaimed until the
compile ends. Obligations are discharged eagerly during elaboration (from
`elab_fns.c` and `elab_typeclasses.c`, not from the end-of-unit
`refine_discharge_all` sweep, which by then finds everything already
`discharged`), so solver allocation is interleaved through the entire run.

One obligation is an obvious region: it enters, builds a VC, runs S0-S3, yields
a verdict. So: reset per entry, and solver memory becomes O(1) in obligation
count instead of O(n).

## What was measured

A wrapper around `refine_discharge_one` (so all ~58 of its exits are covered
without touching any of them), plus per-file byte counters distinguishing the
three allocation populations. Workload: generated files with N guarded call
sites, each producing one crossing obligation, plus the heaviest in-tree
refinement fixture.

| workload | entries | theory (EUF/LA) | VC | cubes+model | peak entry | unit arena |
|---|---:|---:|---:|---:|---:|---:|
| 25 call sites | 30 | 266,560 | 80,880 | 6,720 | 68,567 | 4.79 MB |
| 100 call sites | 105 | 266,560 | 273,480 | 6,720 | 68,567 | 5.68 MB |
| 400 call sites | 405 | 266,560 | 1,043,880 | 6,720 | 68,567 | 9.17 MB |
| `refine-crossing-path-conditions` | 22 | 612,928 | 63,144 | 12,416 | 74,997 | 4.97 MB |

## The result, and why it is a NO

**Only VC construction grows with obligation count** -- 80 KB -> 273 KB ->
1044 KB across 25/100/400 sites, about **2.57 KB per obligation**, unbounded.

**And the VC is retained on purpose.** `refine_discharge_one` does
`ob->vc = pvc` with the comment "so a caller can follow up (e.g. ask for a
witness)", and `--dump-refine=json` renders `vc_smtlib` out of it after the
whole unit is elaborated. A per-entry reset would free it underneath both.

**Everything that IS safely resettable is a bounded constant.** Theory state
(EUF + LA) measures 266,560 bytes at 25 sites and *exactly the same* 266,560 at
400 -- it does not grow with obligation count at all, because only a handful of
obligations reach S1-S3 and the rest are decided by S0 or the memo. Cubes and
the model total 6,720 bytes and are likewise flat.

So a per-entry arena would **cap a constant, not change a growth curve** -- and
it would do so on the one population that is already flat, while leaving the
linear term exactly where it is. That is the opposite of the shape that makes
region reclamation worth it.

Peak single-entry usage is 68.6-75.0 KB and is itself constant, which is the
same fact from the other side: no individual obligation is expensive; the total
is just never given back.

## What to do instead

**SX3 removes most of the resettable constant for free.** The theory number is
what it is because `no_cube_unsat` calls `euf_new` and `la_new` **per cube**, up
to 64 cubes per obligation. SX3 (incremental EUF) replaces that rebuild with
mark/undo over one state, which is already specced, already gated, and deletes
the same allocations as a side effect of work being done for a different reason.
See [solver-extension-plan.md](../../upcoming/solver-extension-plan.md) SX3.

**Nothing needs doing about the VC term.** 2.57 KB per obligation is real but
small: 400 obligations -- far more than any file in the tree -- costs 1 MB on a
9 MB unit arena, in a batch process that exits. Releasing it would mean giving
up the retained VC, which is what makes witness follow-up and the JSON dump
possible. That is a bad trade for a megabyte.

## The caveat that keeps this from over-generalising

This says nothing about the **emitted-code** arena. Row G in the ceiling
harness -- region reclamation over today's boxed ADT layout, 7.64x, no ABI
change -- is a different mechanism in a different program, and the reason it
looks good is exactly the reason this one does not: there, the growing term
(every `ctor_*` box, never freed) *is* the resettable one. The open question
there remains the region boundary, not the payoff.
