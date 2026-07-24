# Incremental Elaboration for the turi REPL -- Design (TR2 core)

> **Status:** Design. This is the substantive core of TR2 in
> `turi-interp-incremental-reclamation-plan.md` -- broken out because it is an
> architectural change to the interpreter's eval loop and the elaborator's
> entry contract, not a localized edit. No code written yet.
>
> **Problem owner:** `docs/reported/turi-repl-quadratic-reparse.md` (CPU) and
> TR0's measurement (memory: ~4.1 GB `eval_arenas` over a 3000-eval session).
>
> **Last updated:** 2026-07-24

---

## The problem in one paragraph

A long-lived turi env re-**parses** and re-**elaborates** the entire accumulated
session source on every top-level eval, then executes only the new tail. Both
passes are O(N) in the accumulated size, so a session is O(N^2) in time and
memory. The re-parse exists only to feed the re-elaboration: the elaborator has
no persistent cross-eval environment, so the only way a new form can resolve a
name/type/typeclass-instance from a prior eval is to re-elaborate every prior
form alongside it in the same pass (`elaborate_program` is handed the whole
`forms[0..nforms)`, `eval.c:10032`; the top-level loops start at 0,
`elab_toplevel.c:1766-1831`). Each eval writes that whole re-elaboration into a
fresh per-call arena that is never freed, and only the *new* forms get bound at
execution -- so every arena accumulates dead re-elaborated copies of all prior
forms. Those dead copies are the ~4.1 GB.

## Why this is not a small fix -- the two load-bearing constraints

**1. No persistent elaboration environment.** Each call builds a fresh
`TypeClassEnv` (`eval.c:10029-10031`) and rebuilds the type / struct / ADT /
instance tables from scratch by walking all forms. `stdlib_prefix` does *not*
skip elaboration of the prefix -- it only resets `has_defmodule` and boundary
scans (`elab_toplevel.c:1443-1460,1621,1709,1813-1824`). So "elaborate only the
new tail" is impossible until a persistent environment exists to resolve against.

**2. Every arena is pinned by live runtime pointers -- AST and IR share it.**
Parse output (`Form*`) and elaborated IR (`Expr*`/`FnDef`) are bump-allocated
from the *same* per-call arena, and the tree-walker executes the `Expr*`
directly. Nine categories of pointer escape into `eval_arenas` and outlive the
call:

1. Closure bodies -- `TuriClosure.fn : FnDef* -> Expr* body` (`eval.c:117`); the dominant one.
2. ADT/GADT constructor natives -- `ud = CtorDef*` (`eval.c:7995-7996`).
3. ADT struct values -- `as_struct->ctor = CtorDef*` (`eval.c:752`).
4. Struct names -- `TuriStruct.name` from the elaborated Expr (`eval.c:575`).
5. Reader-macro templates -- `ReaderMacroEntry.template : Form*` (`env.h:289-294`).
6. `env->last_tc_env : TypeClassEnv*` (`eval.c:10029-10031,10083,10152`).
7. Module-level defers -- `DeferItem.body : Expr*` (`eval.c:770-774,7974`).
8. First-class handler values -- `HandleCase*` arrays into the AST.
9. Effect continuations -- captured work-stack slices tied to the arena.

Because AST + IR share the arena, you cannot free the (dead post-elaboration)
AST without freeing the (live) IR. So arenas genuinely cannot be freed while any
closure/ctor/handler/cont/defer from that eval is reachable.

## Target design: parse + elaborate only the new forms

Turn the whole-program-fresh model into an incremental one:

- **Persist the elaboration environment on `env`.** Carry the accumulated
  `TypeClassEnv`, type/name bindings, and struct/ADT/GADT tables forward between
  evals in a persistent arena (not a per-call one). This is the enabling change;
  everything else follows.
- **Parse only the new source**, appending to a persistent `Form*` store (or
  simply discarding forms after elaboration, since only reader-macro templates
  need the raw AST to survive -- item 5).
- **Elaborate only the new forms** against the persistent environment, resolving
  prior references from it rather than from re-elaborated prefix forms. Merge the
  new definitions (types, instances, signatures) back into the persistent env.
- **Retain a small per-eval arena** holding *only* that eval's new forms' IR
  (which its closures pin). Total retained memory becomes O(total live program) =
  O(N), not O(N^2). Parse and elaborate are each O(new source) per eval.

Net: the re-parse report's CPU cliff and TR0's memory cliff both fall out of the
same change. `src_acc` loses its only real consumer (re-parse) and can be dropped
(TR2.3), except the REPL `:type` command (`repl.c:344-350`), which re-points at
the persistent environment.

Note the arenas still are not *freed* (closures pin them) -- and that is fine.
The goal is O(N) bounded, not incremental reclamation. Freeing an arena when its
defs are redefined/unreachable is a possible later step with poor ROI (redefinition
is rare; the pin is by design).

## Risks (this area is subtle -- the generator-drain bug lives here)

- **Cross-eval typeclass coherence.** Instances now accumulate incrementally;
  the merged `TypeClassEnv` must stay consistent with what a whole-program
  elaboration would have produced (ordering, overlap, orphan rules).
- **Redefinition / shadowing.** The REPL allows redefining `f`; incremental
  elaboration must update the binding and env tables while old closures keep
  pointing at the old arena (correct, just unreclaimed).
- **Imports / `load` expansion.** The run boundary is `prior_prog_items` (program
  items), not `prior_toplevel` (parsed forms), precisely because `(load ...)`
  expands inline (`eval.c:10102-10134`). The incremental path must preserve this
  distinction -- the generator double-advance regression came from getting it wrong.
- **Diagnostics / spans.** Prior forms' spans point into prior `SourceFile`s;
  keep per-eval `SourceFile`s alive (they are small) so a later error referencing
  a prior def still renders.
- **`#lang` / reader-type change** still invalidates the accumulated environment
  (as it invalidates `src_acc` today, `eval.c:9903-9910`) -- a reset point.

## Sub-phases (each independently landable, tested)

- **TR2.0 -- close the test gap first.** There is no in-process C test for a
  reader macro defined in one eval and used in the next (item 5). Add one, plus
  assert the existing cross-eval coverage (`env-longlived.c` 7 sub-tests,
  `eval-basic.c:112-122`) passes unchanged. `env-teardown.c` (leak-cleanliness)
  must stay green throughout.
- **TR2.1 -- persist the elaboration environment.** Thread a persistent
  `TypeClassEnv` + type/name/struct/ADT tables on `env`, built in a persistent
  arena; have `elaborate_program` seed from it. Behavior-preserving (still
  re-elaborates all forms) -- this is pure plumbing that unblocks TR2.2.
- **TR2.2 -- incremental elaboration entry.** New elaborator entry that
  elaborates only `[prior..nforms)` against the persistent env and merges back;
  eval loop parses only new source. This is where the O(N^2) -> O(N) win lands.
- **TR2.3 -- drop `src_acc` re-accumulation.** Re-point `:type` at the persistent
  env; stop accumulating source text.
- **TR2.4 -- enable promotion in the REPL (the literal TR0 action).** Now that a
  long session is bounded, turn on `turi_env_set_scratch_promotion` in `repl.c`
  and ship it with the above.

## Scoping call for the reader

This is a large change to a fragile subsystem, and it is **v1-non-blocking**:
one-shot `tur build` and the compiler never hit it (single eval, no accumulation).
It matters only for long-lived interpreter sessions -- a persistent REPL, a
notebook kernel, an embedded `turi` service. If that is a real v1 use case, TR2.1
+ TR2.2 are the priority interp investment (they subsume the re-parse report). If
it is not, TR2 can wait, and the cheap holding measure is a documented `:reset` /
periodic-compaction escape hatch rather than an architectural change.
