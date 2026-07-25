# Incremental Elaboration for the turi REPL -- Design (TR2 core)

> **Status:** LANDED (gated, default-off) as of 2026-07-24 -- TR2.0, TR2.2a
> (incremental parse) and TR2.1 + TR2.2b (persistent elaboration session) are
> all in. Enable per-env with `turi_env_set_incremental_elab(env, true)`.
> TR2.3 (stop retaining the accumulated source per eval) landed 2026-07-25.
> Remaining: TR2.4 (scratch promotion in the REPL) and flipping the gate on for
> real consumers.
>
> **Headline result (N=800-turn session): 296.9 MB -> 1.2 MB (247x less retained
> memory) and 2.65s -> 0.02s (132x faster).** Growth is now exactly linear where
> it was quadratic (400 turns: 0.6 MB; 800 turns: 1.2 MB). Full suite: 2278
> passed, 0 failed -- the shared compiler path is unaffected (`session = NULL`).
>
> **Problem owner:** `docs/reported/turi-repl-quadratic-reparse.md` (CPU) and
> TR0's measurement (memory: ~4.1 GB `eval_arenas` over a 3000-eval session).
>
> **Last updated:** 2026-07-25

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
same change. (On `src_acc`: TR2.3 found the buffer itself is cheap and linear,
and is what makes diagnostics span earlier turns -- what had to go was the
per-eval *retained copy* of it. See TR2.3 below.)

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

## Implementation constraints discovered (2026-07-24)

Probing the eval loop for a safe first slice surfaced two facts that shape how
the change must be built:

1. **Parsed `Form`s are immutable during elaboration -- reuse is safe (good).**
   The only writes to `Form` fields anywhere are the constructors in `forms.c`;
   the elaborator takes `Form *const *` and never mutates the AST. So reusing a
   prior eval's `Form*` across later evals (instead of re-parsing) is sound from
   a mutation standpoint, and prior forms already live in retained `eval_arenas`.

2. **The diagnostic file model blocks *naive* form reuse (the real snag).**
   `turi_eval_impl` calls `diag_reset()` every eval, which clears the entire
   registered-file table (`diag.c:101`), and each eval registers its `SourceFile`
   at `file_id = 0` holding the *whole accumulated blob*. A reused prior form's
   span still says `file_id = 0` but with offsets into the *old* blob, so after a
   reset + re-register it would resolve against the new eval's (shorter) source --
   a wrong or out-of-bounds diagnostic read. Giving each eval a distinct
   `file_id` does not scale either: `MAX_FILES` is **64** (`diag.c:11`), so a
   long session would exhaust it in 64 evals -- exactly the lifetime we are
   trying to support.

**Consequence, and how it was resolved (landed 2026-07-24).** The diag snag has
a clean answer: keep passing the **full accumulated blob** as the `SourceFile`
and only skip *re-parsing* it. Because `src_acc` grows purely by appending, a
prior form's span offsets remain valid in the longer blob, so one `file_id`
serves the whole session -- no `MAX_FILES` pressure, no `diag_reset` conflict,
and error snippets render byte-identically (verified: the A/B differential shows
identical multi-turn diagnostics with correct absolute line numbers). Forms are
also self-contained -- `form_str` `arena_strdup`s its bytes -- so a reused form
never points into a source buffer.

## Known behavioral divergence (one, and it is an improvement)

The A/B differential found exactly one semantic difference, and it is worth a
product decision rather than being papered over:

**Redefining a top-level `defn` across turns.** On the default path every turn
re-elaborates the accumulated program with `stdlib_prefix` = count of prior
forms, which marks all previously-accumulated forms as *stdlib*. Redefining your
own function therefore fails with the actively misleading *"'f' is already
defined by an auto-loaded stdlib module"*. On the incremental path prior turns'
definitions are ordinary user bindings in the session scope, so redefinition
does what a REPL should: the new definition wins.

The incremental behavior is the better one, and the gate is off by default, so
nothing changes for existing consumers until they opt in. `test_known_divergence`
in the differential harness pins BOTH behaviors so further drift on either path
is caught. (`def` redefinition still errors on both paths -- unchanged.)

## Sub-phases (each independently landable, tested)

- **TR2.0 -- close the test gap first. [DONE 2026-07-24]** There was no in-process
  C test for a reader macro defined in one eval and used in the next (item 5).
  Added `tests/turi/reader-macro-cross-eval.c` (`tur_reader_macro_cross_eval`
  ctest): define in one `turi_eval`, use across separate later evals, survive 50
  intervening evals (the incremental case -- the define is not re-run), and
  redefinition. Green on the current tree; must stay green through TR2.1--TR2.4.
  The existing cross-eval coverage (`env-longlived.c` 7 sub-tests,
  `eval-basic.c:112-122`) and `env-teardown.c` (leak-cleanliness) are the rest of
  the safety net.
- **TR2.1 + TR2.2b -- persistent elaboration session. [DONE 2026-07-24]**
  Landed together (they cannot ship separately: reusing the scope while still
  re-elaborating every form would re-declare prior definitions into an
  already-populated scope). The implementation turned out far smaller than the
  original loop-restriction sketch below, because **the caller slices the form
  array** -- no elaborator loop bounds change at all:
  - `ElabSession` (opaque `Elab`) + `elab_session_new/free` (`elab.h`,
    `elab_toplevel.c`). `elaborate_program_session(...)` takes it;
    `elaborate_program(...)` is now a wrapper passing `NULL`, so **every
    compiler path is untouched** (verified: full suite 2278 passed, 0 failed).
  - State restore/save is a struct copy, re-anchoring the one self-referential
    field (`scope = &global`). With a session the per-return teardown is skipped
    and the state is handed back instead; per-call working sets that ARE freed
    (`file_scope_defs`, `bare_fat_*`) are nulled so no dangling pointers persist.
  - The interpreter hands the elaborator only `acc_forms[elab_session_forms..)`,
    with `stdlib_prefix` and the already-run program-item prefix adjusted for the
    slice, and keeps `prior_prog_items` cumulative across both paths.
  - **Discard-on-failure:** any elaboration or runtime error frees the session,
    so the next turn rebuilds by replaying all accumulated forms (exactly the
    old behavior). This is what keeps a half-elaborated program from poisoning
    the session, and it is self-correcting rather than requiring rollback.
  - **Measured (N=800 turns): 299.3 MB -> 3.6 MB (83x less) and 1.54s -> 0.03s
    (51x faster).** Growth is now ~linear where the default path is quadratic
    (400->800 turns: default 76->299 MB, incremental 1.2->3.6 MB).

  *Original sketch (superseded, kept for context):* Thread a persistent
  elaboration state on `env`, built in a persistent arena, and have the
  elaborator seed from it. Behavior-preserving (still re-elaborates all forms) --
  pure plumbing that unblocks TR2.2.

  *Concrete scope (from the `Elab` struct, `elab_internal.h:155-812`).* The struct
  is dominated by cheap `Symbol*` dispatch caches that are re-derived from the
  symbol table each call -- those stay per-call. The state that must persist
  across evals is small and specific:
  - `Scope global` (`:159`) -- the name -> binding environment.
  - `TypeClassEnv typeclass_env` (`:173`) -- accumulated instances.
  - `Expr **file_scope_defs` (`:169-171`).
  - `AdtDef **adt_defs` (`:331-333`) -- ADT registry (+ GADT signature stack).
  - `EffectEnv *effect_env` (`:290`) -- effect registry.
  - `ElabModule *loaded_modules` (`:414-416`) -- module registry.
  - the struct/type registry (registered through the scope/type env).

  *Hard constraint.* `elaborate_program` is shared with the **batch compiler**
  (`main.c` compile path), not only the interpreter. TR2.1 must add an *opt-in*
  seed/merge path (a persistent-state handle the interpreter passes and the
  compiler passes NULL for) so whole-program compilation stays byte-identical;
  the incremental path is interpreter-only. This is why TR2.1 is its own focused
  pass with a full `bash tests/run.sh` gate (the ~1442 fixtures exercise the
  compiler's use of `elaborate_program`), not a quick edit. The new entry shape:
  `elaborate_program_incremental(persistent_state*, new_forms, ...)` that seeds
  `elab_init_state` from `persistent_state`, elaborates only the new forms, and
  merges new defs/instances/registries back into `persistent_state`.
- **TR2.2a -- incremental PARSE. [DONE 2026-07-24]** The eval loop now re-reads
  only the newly appended source and reuses prior evals' Forms, behind the
  default-off `turi_env_set_incremental_elab` gate.
  - `read_all_with_registry_from(..., start_offset, start_line, ...)`
    (`reader.c`) is the offset-aware core; `read_all_with_registry` is it with
    `(0, 1)`, so **every compiler path is byte-identical**.
  - `env->acc_forms` accumulates top-level Forms; committed only on a successful
    eval (mirroring `src_acc`) and rolled back on parse/elaboration/eval error.
    `env->acc_next_line` tracks the resume line incrementally (counting newlines
    in the new chunk only, never rescanning the prefix).
  - Automatic fallback to the whole-blob parse whenever the fast path does not
    apply (sweet-exp reader, first eval, a reader-type reset, or a vector/session
    desync), so correctness never depends on the fast path firing.
  - Guarded by `tur_incremental_elab_diff`: an A/B differential running scripted
    sessions (cross-eval defs, multi-line turns, reader macros, mid-session
    errors, redefinition, 300-turn churn) through both paths and comparing every
    turn's result.
  - **Measured:** N=800 turns, 2.32s -> 0.91s (-61%) and 299 MB -> 240 MB
    (-20%). The time win grows with N (30% at N=400, 61% at N=800), confirming
    the quadratic *parse* term is gone. Memory only drops ~20% because
    elaboration is still whole-program -- that residue is TR2.2b.

- **TR2.2b -- incremental ELABORATION (the remaining memory win).** New
  elaborator entry that elaborates only `[prior..nforms)` against the persistent
  env from TR2.1 and merges back. This is what takes retained memory from
  O(N^2) to O(N); the parse side is already done.
- **TR2.3 -- stop RETAINING the accumulated source per eval. [DONE 2026-07-25]**
  The framing in the original sketch ("drop `src_acc`") turned out to be the
  wrong target. `src_acc` itself is a single linear buffer (39 KB after 3000
  evals) and it is what lets diagnostics render spans into earlier turns -- worth
  keeping. The real cost was that each eval `arena_strdup`'d the *entire*
  accumulated blob into its own per-eval arena: O(N) retained per eval, O(N^2)
  over a session. Once elaboration went incremental this was the whole remaining
  residue.

  Fix: build the combined blob into an env-owned `src_combined` buffer that is
  **reused** every eval, and point the `SourceFile` at it instead of at a fresh
  arena copy. Safe because nothing holds a pointer into it past its own eval --
  Forms copy their bytes (`form_str` arena_strdups) and the `SourceFile` is
  re-registered each turn.

  **Measured: N=800 turns 3.6 MB -> 1.2 MB, and growth is now exactly linear**
  (400 turns 0.6 MB, 800 turns 1.2 MB -- 2x memory for 2x turns). Against the
  default path at N=800: **296.9 MB -> 1.2 MB (247x less), 2.65s -> 0.02s.**

  The REPL `:type` command still reads `src_acc` and needs no change.
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
