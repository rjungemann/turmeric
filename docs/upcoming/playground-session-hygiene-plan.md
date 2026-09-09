---
title: "Playground session hygiene (PS)"
category: Planning
description: Stop a doc lookup from mutating the eval session, stop the whole-program fallback from mislabeling a user's own definitions as stdlib, and make Run mean "run this program" rather than "append this program to a session".
---

# Playground session hygiene (PS)

**Status: planned, not started.** Filed from
[doc-lookup-poisons-the-playground-eval-session](../reported/doc-lookup-poisons-the-playground-eval-session.md),
whose repros are all measured against the live site on 2026-09-09. Read the
report for the symptom; this is the fix sequence.

**One line:** a documentation hover can permanently break the Run button, and
when it does, the error blames the user's own function for colliding with a
stdlib module that does not contain it.

**Saffron is not involved.** Every symptom reproduces under `#lang turmeric`.
Do not let the dialect in the original report send you into `saffron-lang-plan`.

## The shape of it

Three defects stack into one failure, and they are *not* equally deep. Ordered
by how much they explain:

1. **The whole-program fallback marks every accumulated form as stdlib.**
   This is the one that produces the wrong diagnostic, and it is a four-line
   fix. It also silently makes `defn` redefinition fail in more sessions than
   anyone realised (see PS1 -- sweet-exp is always on this path).
2. **A doc lookup evaluates into the session.** This is what *triggers* the
   fallback in the reported case, and it is independently wrong: a read-only
   query should not be able to change program semantics.
3. **`defeffect` is not re-runnable across turns.** Genuinely separate, and the
   only one that is not really about the playground -- it reaches `tur repl`.

Underneath all three is a design question about what Run means, which PS5
settles.

## What exists today (checked against the tree)

`turi_eval` accumulates. Each turn appends its source to `env->src_acc` and its
parsed forms to `env->acc_forms`, and re-elaborates from a boundary:

```c
/* src/turi/eval.c:12831 */
const bool use_incr_elab = env->elab_session && prior > 0 &&
                           env->elab_session_forms == prior;
const uint32_t elab_from = use_incr_elab ? prior : 0;

elaborate_program_session(..., forms + elab_from, nforms - elab_from,
                          /*stdlib_prefix=*/prior - elab_from, ...);
```

`stdlib_prefix` tells the elaborator that `forms[0..stdlib_prefix)` came from an
auto-loaded stdlib module (`elab_toplevel.c:1768`), which sets `is_from_stdlib`
on those bindings. `elab_fns.c:5643` then hard-errors on any user `defn` that
collides with one:

```c
if (existing->is_from_stdlib && !e->in_stdlib_load) {
    diag_emit(DIAG_ERROR, name_f->span,
              "defn: '%s' is already defined by an auto-loaded stdlib "
              "module; rename the local definition", ...);
```

So the two paths disagree about who is stdlib:

| path | `elab_from` | `stdlib_prefix` | effect |
| --- | --- | --- | --- |
| incremental | `prior` | `0` | nothing is stdlib; redefinition works |
| whole-program fallback | `0` | `prior` | **everything prior is stdlib**, including the user's own last turn |

`env.c:206` records the incremental path's redefinition behaviour as an
intentional fix. It is better read as: the fallback has a bug that the
incremental path happens to route around.

The env already knows the right answer. `turi_env_pin_prelude` captures the
preload boundary, and `env->pin_toplevel` is `prior_toplevel` at pin time
(`src/turi/env.h:196`) -- in the same units as `prior`
(`eval.c:12801`, `prior = env->prior_toplevel`).

**The fallback is not rare.** It is taken whenever `elab_session` is absent --
including after any failed eval, which discards it -- and incremental *parsing*
is additionally disabled outright for sweet-exp:

```c
/* src/turi/eval.c:12757 */
env->reader_type != READER_SWEET &&
```

So a `#lang turmeric/sweet` or `#lang saffron/sweet` session is on the
mislabeling path from its first turn.

## PS1 -- `stdlib_prefix` counts the pinned prelude, not the session

**The fix.** At `src/turi/eval.c:12837`, pass the pinned prelude's form count
rather than everything accumulated:

```c
/* The stdlib region is the PINNED PRELUDE, not "everything before this turn".
 * Passing `prior` marked the user's own previous turns as stdlib, so their
 * next `(defn main ...)` came back as "already defined by an auto-loaded
 * stdlib module" -- naming a module that does not contain it, and advising a
 * rename that cannot help. */
const uint32_t pin = env->pin_toplevel;
const uint32_t stdlib_prefix = (elab_from >= pin) ? 0u : (pin - elab_from);
```

**Before touching it, verify `stdlib_prefix` has no second role.**
`elab_toplevel.c:1758` pre-splices the stdlib forms, and `:1788-1790` uses it
as a `track_boundary` in/out pair; `:897` refers to "the corrected
stdlib_prefix", so there is prior art on getting this number wrong. Read those
three sites before changing the caller.

**Why first:** it is the smallest change with the widest effect. It fixes the
wrong diagnostic, and it makes `defn` redefinition work on the fallback path --
which is every sweet-exp session and every session that has seen one error.

**Done when:** re-running a `(defn main ...)` program succeeds after a
deliberately-failed eval, and in a `#lang turmeric/sweet` session; and no
message names an auto-loaded stdlib module for a name that is not in one.

**Tests:** a turi-level regression is the right level here, not a browser test
-- `tests/` already has `tur_incremental_elab_diff` as the A/B harness for this
exact boundary (`env.c:208`). Add: redefine a `defn` across turns *on the
fallback path* (force it with `TUR_NO_INCREMENTAL_ELAB=1`) and assert success,
plus a case asserting the stdlib collision still errors for a name that really
is in the prelude, so PS1 does not simply delete the check.

## PS2 -- a doc lookup must not evaluate into the session

**The defect.** `turi_doc_lookup` runs a read-only query through the
accumulating evaluator:

```c
/* src/web/wasm_glue.c:642 */
TuriValue result = turi_eval(g_env, expr);   /* expr = (doc-lookup "name") */
```

Every lookup splices `(doc-lookup "...")` into the session permanently. It
re-elaborates on every later Run (the repeating `TUR-W0040`), it shifts the
`<eval>:NN` lines the user sees for their own code, and -- because the call
fails -- it discards `elab_session` and drops the session onto the PS1 path.

**Two options.**

- **(a) Read the table directly from C.** The function already does exactly
  this for builtins six lines earlier (`turi_doc_lookup_builtin`,
  `wasm_glue.c:616`). If the docstrings table is reachable as data, the whole
  eval detour disappears. Preferred: no evaluation, no failure mode, no
  session contact.
- **(b) A non-accumulating eval entry point.** There is no `turi_eval_*`
  variant today that skips `src_acc` (`eval.c:11813-11826` are all
  accumulating wrappers over `turi_eval_impl`). Adding one is more surface than
  (a) but is reusable -- the LSP/hover and completion paths want the same
  guarantee.

**Do (a) if the table is reachable; fall back to (b).** Decide by reading
`stdlib/docstrings.tur`'s generated shape and whether `--emit-tur` output has a
C-side counterpart.

**Done when:** an arbitrary number of doc lookups leaves `env->src_acc`
byte-identical, and repro A in the report (lookup, then Run) runs clean.

## PS3 -- decide whether the playground has stdlib docstrings at all

**The defect.** `wasm_preload_stdlib` (`src/web/wasm_glue.c:95-107`) loads
macros, native stubs, collections and typeclasses. It does **not** load
`stdlib/docstrings.tur`, the only definition site of `doc-lookup`
(`stdlib/docstrings.tur:5`). So the query in PS2 cannot resolve for *any*
name: `wasmDocLookup('vec-map')` returns `null` for a function that has a
docstring row.

**This has been masked.** `web/main.js:4770-4780` falls back to the
`doc-names.json` summary when the wasm lookup returns nothing, so the panel
shows *something* and nobody noticed the wasm path was dead. But
`doc-names.json` carries `{name, summary, kind}` only (6055 entries, measured)
-- a one-line summary, not the full docstring. Users have been getting the
degraded view for stdlib symbols the whole time.

So this is a real product decision, not a bug with an obvious fix:

- **Preload `docstrings.tur`** -- full docstrings return, at the cost of its
  size in the wasm startup path. Measure that cost first; it is a generated
  file and not small.
- **Or emit full docstrings into the docs pack** and drop the eval path
  entirely, serving stdlib docs from the JSON/pack the panel already loads.
  This composes with PS2(a) -- if nothing needs `doc-lookup` at runtime, the
  whole mechanism goes away. It also puts stdlib and spice symbols on one code
  path instead of two.

**Recommendation: the second.** [offline-docs-plan](offline-docs-plan.md)
already made the pack the single artifact three consumers read; a parallel
in-wasm docstring table is the thing that plan exists to avoid. But this needs
a size check against OD's budget before committing.

**Done when:** the doc panel shows the same content for a stdlib name as
`/docs/html/api/` does, and `(doc 'vec-map)` at the playground prompt either
works or is honestly unsupported -- not warning about a name the environment
was never given.

## PS4 -- `defeffect` redefinition across turns

**The defect.** PS1 fixes `defn`. `defeffect` has its own "already defined"
check that PS1 does not reach: re-running a program containing
`(defeffect Ask [] :int)` fails on the second run regardless of path. This is
the examples-dropdown case -- `web/examples.js:99` is that program, so running
the shipped effects example twice breaks the session.

**The work:** find `defeffect`'s redefinition check (`elab_effects.c` or
equivalent) and give it the same across-turns behaviour a `defn` has. Then
audit the siblings in the same pass -- `defstruct`, `defadt`, `defclass`,
`definstance`, `defopaque`. Nothing in `env.c:198-215` suggests `defn` was
*meant* to be the only form that survives a redefinition; it is where the
incremental work happened to land.

**A judgement call to make explicitly:** redefining an effect or a type is not
obviously as safe as redefining a function -- existing values may carry the old
tag. If a form cannot be safely redefined, say so in the diagnostic and point
at the session reset, rather than reporting a stdlib collision. A correct "you
cannot redefine an effect mid-session, reset to run this again" is a fine
outcome for PS4; a wrong "rename your function" is not.

**Done when:** the shipped effects example runs twice in a row, or fails with a
message that names the real constraint and the action that resolves it.

**This one reaches `tur repl`,** not just the browser. Test it there too.

## PS5 -- what Run means

The design question under all of it: **Run re-evaluates the editor buffer into
a session that still holds the previous run's definitions.** That is right for
a prompt and wrong for a button labelled Run -- pressing it twice is not a
request to redefine anything.

**The constraint that makes this non-trivial.** `runCode` deliberately feeds
the session (`web/main.js:2318, 2329`):

```js
if (!isError) replSessionAccept(code);
```

with the comment that "a Run is how a tab's definitions become callable at the
prompt, so it is also how they become offerable there" (W2). A naive "reset on
Run" throws that away, and also drops *other tabs'* definitions when you run
this one.

**Proposal: reset-then-evaluate, not reset-instead-of-evaluate.** On Run,
reset to the pinned prelude *and then* evaluate the buffer. Afterwards the
session contains exactly the prelude plus this program -- so W2 still holds
(the buffer's definitions are callable and offerable at the prompt), and
re-running is idempotent.

What that trades away, stated plainly:

- Definitions typed **at the prompt** between runs are discarded by the next
  Run. Defensible -- Run means "run this program", and the program is the
  buffer -- but it is a behaviour change a user can notice.
- **Multi-tab sessions change.** Today, running tab A then tab B leaves both
  callable. After this, only the last-run tab is. Options: reset scoped
  per-tab, or re-run the other tabs' accepted source as part of the reset, or
  accept the change. **Resolve this before implementing** -- see Open
  questions.

**Done when:** pressing Run twice on any program in `web/examples.js` produces
identical output both times.

## Sequencing

PS1 first and alone -- it is small, it is the deepest, and it makes the
remaining symptoms legible. PS2 next (it is what triggers PS1's path in the
reported case). PS3 and PS4 are independent of each other and of PS5. PS5 last,
because PS1+PS2 remove the sharp edges and PS5 is the only one with a real
behaviour trade to negotiate.

PS1 and PS2 together close the reported bug. PS3-PS5 are the reason it was
reachable.

## Open questions

1. **Does `stdlib_prefix` carry a second meaning** at
   `elab_toplevel.c:1758/1788-1790` that PS1 would break? Must be answered
   before PS1 lands, not after.
2. **Is the docstrings table reachable from C** without evaluating Turmeric
   (PS2a)? If not, PS2 costs a new eval entry point.
3. **What does `docstrings.tur` cost in the wasm startup path** (PS3)? Decides
   preload-vs-pack.
4. **Which `def*` forms can be safely redefined mid-session** (PS4)? Needs a
   real answer per form, not a blanket one.
5. **Multi-tab semantics under PS5.** Does a reset scope to the tab, replay
   the other tabs, or change the contract? This is a product call and should
   be made by whoever owns the Try Turmeric UX, not inferred from the code.

## Non-goals

- Anything in `saffron-lang-plan`. The dialect is incidental to every symptom
  here.
- Reworking the accumulating-session model itself. `env.c:198-215` documents
  why it exists (O(N^2) memory and time over a long session, measured at ~1 GB
  and quadratic over 1500 turns). PS1 and PS5 work *within* that model.
- The offline docs pack's structure. PS3 may add to what the pack carries; it
  should not change how [offline-docs-plan](offline-docs-plan.md) built it.
