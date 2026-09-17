---
title: "Playground session hygiene (PS)"
category: Planning
description: Stop a doc lookup from mutating the eval session, stop a failed eval from dropping the session onto a path that mislabels the user's own definitions as stdlib, and make Run mean "run this program" rather than "append this program to a session".
---

# Playground session hygiene (PS)

**Status: EXECUTED 2026-09-16.** PS1-PS5 all landed, and so did one fix the
plan did not foresee (a `defmacro*` session aborting on its next failed turn).
PS3 took a different route to the same end than the one decided below, for
reasons given in the record. See [Execution record](#execution-record-2026-09-16)
at the end; the body above it is the plan as written. Filed from
[doc-lookup-poisons-the-playground-eval-session](doc-lookup-poisons-the-playground-eval-session.md),
whose repros are measured against the live site.

**One line:** a documentation hover can permanently break the Run button, and
when it does, the error blames the user's own function for colliding with a
stdlib module that does not contain it.

**Saffron is not involved.** Every symptom reproduces under `#lang turmeric`.
Do not let the dialect in the original report send you into `saffron-lang-plan`.

## Decisions

| # | Question | Answer |
| --- | --- | --- |
| 1 | Does `stdlib_prefix` carry a second role? | **Yes -- four roles.** PS1 is re-scoped around that; investigate the no-discard route first. |
| 2 | Is the docstrings table reachable from C? | **It already *is* a C table**, wrapped in a `.tur` inline-C body. |
| 3 | What does preloading `docstrings.tur` cost? | **Moot -- it cannot work.** Emit a C table and use it both ways instead. |
| 4 | Which `def*` forms can be safely redefined? | Measured; the split is unprincipled. **Make them all redefinable.** |
| 5 | Multi-tab semantics under a Run reset? | **Replay the other tabs after the reset.** |

## The shape of it

Three defects stack into one failure:

1. **A failed eval drops the session onto a path that mislabels every
   accumulated form as stdlib.** This produces the wrong diagnostic.
2. **A doc lookup evaluates into the session.** This is what *causes* the
   failed eval in the reported case, and it is independently wrong: a
   read-only query should not be able to change program semantics.
3. **`defeffect` is not re-runnable across turns.** Separate, and the only one
   that is not really about the playground -- it reaches `tur repl`.

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

The two paths disagree about who is stdlib:

| path | `elab_from` | `stdlib_prefix` | effect |
| --- | --- | --- | --- |
| incremental | `prior` | `0` | nothing is stdlib; redefinition works |
| whole-program fallback | `0` | `prior` | **everything prior is stdlib**, including the user's own last turn |

`env.c:206` records the incremental path's redefinition behaviour as an
intentional fix. It is better read as: the fallback has a bug that the
incremental path routes around.

### `stdlib_prefix` has four roles, not one (Q1)

This is why PS1 is not the four-line change an earlier draft of this plan
claimed. Every consumer, traced:

| Role | Site |
| --- | --- |
| Marks bindings `is_from_stdlib` -- the wrong diagnostic | `elab_fns.c:5643` |
| Seeds the load-dedup visited set, so a later explicit `(load "stdlib/...")` is a no-op | `elab_toplevel.c:1767-1774` |
| Post-load macro-promotion boundary; **reassigned** as `stdlib_prefix = lx.boundary_out` | `elab_toplevel.c:1805` |
| **Partitions both elaboration passes**: `e.in_stdlib_load = (stdlib_prefix > 0)`, flipped at `i == stdlib_prefix` | `elab_toplevel.c:2093/2095`, `2161/2163` |

The fourth is the hazard. Shrinking `stdlib_prefix` re-partitions which forms
count as *user* code in both passes, so prior user turns become newly subject
to the user-region scans at `:1966` (ambiguity), `:2058`, and `:2115-2116`
(file-scope grouping) that they escape today. Defensible -- they *are* user
forms -- but it is a real blast radius, not a diagnostic tweak.

### The fallback is not rare

It is taken whenever `elab_session` is absent, and **any failed eval discards
it** (`eval.c:12827`). Incremental *parsing* is additionally disabled outright
for sweet-exp:

```c
/* src/turi/eval.c:12757 */
env->reader_type != READER_SWEET &&
```

So one failed eval is enough to put a session on the mislabeling path for the
rest of its life. That is the whole mechanism behind the report: a doc lookup
fails, and every subsequent Run is broken.

## PS1 -- stop a failed eval from discarding the elaboration session

**Decided (Q1): investigate the no-discard route first, and do not re-scope
`stdlib_prefix` unless that route fails.**

The `stdlib_prefix` miscount is real, but it is only *reachable* because a
failed eval discards `elab_session` and drops the session onto the
whole-program path. Fixing the trigger keeps every session on the incremental
path, where the miscount cannot manifest -- and touches none of the four roles
above.

**The work:**

1. Establish why `elab_session` is discarded on failure (`eval.c:12827` and the
   discard site). If it is discarded because a failed turn may have left it
   half-mutated, the fix is a rollback to the last committed turn -- the
   accumulator already has that shape (`acc_committed`, `eval.c:12751`, is
   exactly this rollback point for forms).
2. Confirm the sweet-exp arm. Incremental *parsing* is gated separately on
   `reader_type != READER_SWEET` (`eval.c:12757`); establish whether a parse
   fallback forces `use_incr_elab` false. **If it does, sweet-exp sessions stay
   on the mislabeling path and PS1 alone does not fix them** -- that is the
   trigger for doing the `stdlib_prefix` correction after all, as its own
   change with its own test pass.

**Done when:** a deliberately-failed eval leaves the session able to re-run a
`(defn main ...)` program, and no message names an auto-loaded stdlib module
for a name that is not in one.

**Tests:** turi-level, not browser. `tests/` already has
`tur_incremental_elab_diff` as the A/B harness for this boundary
(`env.c:208`). Add: an eval that fails, then a redefinition that must succeed;
and a case asserting the stdlib collision still errors for a name that really
is in the prelude, so the check is not simply deleted.

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
fails -- it discards `elab_session` and drops the session onto PS1's path.

**Decided (Q2/Q3): read a C table directly, and delete the eval entirely.**
See PS3 for where the table comes from. `turi_doc_lookup` already does exactly
this for builtins six lines earlier (`turi_doc_lookup_builtin`,
`wasm_glue.c:616`), so this is a second call against a second table, not a new
mechanism.

**Done when:** an arbitrary number of doc lookups leaves `env->src_acc`
byte-identical, and repro A in the report (lookup, then Run) runs clean.

## PS3 -- emit the docstrings table as C, and use it from both sides

**The defect.** `wasm_preload_stdlib` (`src/web/wasm_glue.c:95-107`) never
loads `stdlib/docstrings.tur`, the only definition site of `doc-lookup`
(`stdlib/docstrings.tur:5`). So PS2's query cannot resolve for *any* name:
`wasmDocLookup('vec-map')` returns `null` for a function that has a docstring
row.

**Masked this whole time.** `web/main.js:4770-4780` falls back to the
`doc-names.json` summary when the wasm lookup returns nothing, so the panel
showed *something*. But that file carries `{name, summary, kind}` only (6055
entries, measured) -- a one-line summary, not the docstring. Stdlib symbols
have been showing the degraded view since the panel shipped.

**Preloading cannot fix it (Q3).** `stdlib/docstrings.tur` is 487 KB / 2250
lines, but size is not the blocker: its only function is an **inline-C body**,
and `doc-lookup` is **not** registered as an interpreter native. The
tree-walking interpreter cannot execute it. Adding the file to
`wasm_preload_stdlib` would load a module whose sole export cannot run.

**Decided: emit a real C table and use it both ways.** The data is *already* a
C array -- `stdlib/docstrings.tur:5` is a `defn` whose body is inline C holding
`static const struct { const char *key; const char *val; } entries[]`. The
generator is wrapping a C table in Turmeric for no reason the playground
benefits from.

1. `tools/gendocs.py` gains an emitter for a `.c`/`.h` docstrings table
   alongside `--emit-tur` (which keeps working -- the compiled path still wants
   the Turmeric module).
2. Link it into the build; `turi_doc_lookup` reads it directly (PS2).
3. Register a `doc-lookup` interpreter native backed by the same table, in
   `turi_env_register_interpreter_natives` beside the other inline-C shims, so
   `(doc 'vec-map)` works at the playground prompt and under `--interpret`.

Step 3 is what makes this strictly better than serving docs from the pack: one
table, three consumers, and the `doc` macro (`stdlib/macros.tur:107`) stops
being a latent hole in every interpreter.

**Done when:** the doc panel shows the same content for a stdlib name as
`/docs/html/api/` does, and `(doc 'vec-map)` prints a docstring at the
playground prompt instead of warning about an unknown name.

## PS4 -- make every `def*` redefinable across turns

**Measured (Q4), `tur repl`:**

| form | redefines across turns | what you get |
| --- | --- | --- |
| `defn` | yes | works (the incremental-path behaviour) |
| `defclass` | yes | works, silently |
| `defopaque` | yes | works |
| `defeffect` | **no** | `defeffect: 'Ask' is already defined` (`elab_effects.c:1409`) |
| `defstruct` | **no** | `defstruct: 'P' is already defined (an auto-loaded stdlib module or earlier form in this file defines a type with this name; pick a distinct name)` (`elab_structs.c:1071`) |
| `defadt` | unestablished | not probed correctly; establish before implementing |

**Decided: treat the split as unintended and make them all redefinable.** Half
the forms already permit exactly what the other half refuses, and nothing in
`env.c:198-215` suggests `defn` was meant to be special -- it is where the
incremental work happened to land. One rule is also the only version of this a
user can predict.

**Two things to carry:**

- **`defstruct`'s message has the same misattribution** as the reported one --
  "an auto-loaded stdlib module or earlier form in this file" blames a module
  that need not be involved. If any refusal survives implementation, its
  message must name the session and point at the reset, which is the action
  that resolves it.
- **Where a redefinition is genuinely unsafe, say so rather than allowing it.**
  Live values may carry an old effect or type tag. The decision above is the
  default, not a mandate to break safety: if a probe shows a form cannot be
  redefined without corrupting live values, refuse *that* form with an honest
  message and record why here.

**Done when:** the shipped effects example (`web/examples.js:99`) runs twice in
a row, and the same holds at `tur repl`.

## PS5 -- what Run means

**The problem.** Run re-evaluates the editor buffer into a session that still
holds the previous run's definitions. Right for a prompt, wrong for a button
labelled Run -- pressing it twice is not a request to redefine anything.

**The constraint.** `runCode` deliberately feeds the session
(`web/main.js:2318, 2329`):

```js
if (!isError) replSessionAccept(code);
```

with the comment that "a Run is how a tab's definitions become callable at the
prompt, so it is also how they become offerable there" (W2).

**Measured (Q5):** `replSessionSource` is a **single global**
(`main.js:3002`). `switchTab` (`:273`) never touches it; only `:reset` and
`resetWasm` clear it (`:1602`, `:5538`). All tabs genuinely share one session.

**Decided: reset-then-evaluate, replaying the other tabs.** On Run:

1. Reset to the pinned prelude (`turi_env_reset_to_prelude`).
2. Re-accept the other tabs' last-run source.
3. Evaluate this tab's buffer.

The session then holds the prelude, the other tabs' programs, and exactly one
copy of this one. Re-running is idempotent, W2 still holds, and cross-tab
behaviour is what it is today -- no user-visible regression, which is why this
beat the simpler session-wide reset.

**What it costs, stated plainly:** a replay on every Run, proportional to the
other tabs' accepted source. Bounded by the tab count and already-elaborated
text, but it is real work on a hot path -- measure it before assuming it is
free, and consider caching the replayed prefix if it bites.

**Also traded away:** definitions typed **at the prompt** between runs are
discarded by the next Run. Defensible -- Run means "run this program", and the
program is the buffer -- but it is a behaviour change worth a line in the
release notes.

**Done when:** pressing Run twice on any program in `web/examples.js` produces
identical output both times, and a definition from another tab is still
callable at the prompt afterwards.

## Sequencing

PS1 first and alone -- it is the trigger for the reported failure and the
cheapest place to break the chain. PS2 next; it is what fires PS1's path in
practice, and PS3 supplies the table it needs, so PS2 and PS3 land together.
PS4 and PS5 are independent of both and of each other.

PS1 + PS2/PS3 close the reported bug. PS4 and PS5 are the reasons it was
reachable at all.

## Open questions

The five that gated this plan are resolved (see Decisions). What remains is
implementation-time verification, not design:

1. **Does keeping `elab_session` alive across a failure actually hold the
   session on the incremental path?** This is PS1's premise. If a failed turn
   can leave the session half-mutated, PS1 becomes a rollback rather than a
   no-discard, and `acc_committed` (`eval.c:12751`) is the model.
2. **Does a sweet-exp parse fallback force `use_incr_elab` false?** If yes,
   PS1 does not cover sweet-exp sessions and the `stdlib_prefix` correction has
   to happen after all -- separately, with its own tests, against the four
   roles above.
3. **Is `defadt` redefinable?** Unestablished; the probe used wrong syntax.
4. **What actually breaks when an effect or struct is redefined with live
   values around?** PS4's policy is decided, but this decides whether any
   specific form has to keep refusing.

## Non-goals

- Anything in `saffron-lang-plan`. The dialect is incidental to every symptom.
- Reworking the accumulating-session model. `env.c:198-215` documents why it
  exists (O(N^2) memory and time over a long session, measured at ~1 GB and
  quadratic over 1500 turns). PS1 and PS5 work *within* that model.
- The offline docs pack's structure. PS3 adds a C table beside it and does not
  change how [offline-docs-plan](offline-docs-plan.md) built the pack.

## Execution record (2026-09-16)

Branch `claude/playground-session-hygiene`. Every repro in the source report
was first reproduced natively -- nothing in `src/web/wasm_glue.c` is
wasm-specific, so a harness linking `libturi_wasm` drives the playground's own
entry points -- and each fix was shown to turn a failing check green.

**Two corrections to the report's mechanism.** A doc lookup does *not* splice
`(doc-lookup ...)` into the session: the eval fails at runtime, and a failed
turn is never committed to `src_acc`. The damage was entirely the failure's
side effect -- the discarded elaboration session and the whole-program rebuild
(PS1). And `web/examples.js`, which the report and PS4/PS5 cite, is not what
the page uses: the dropdown reads the `EXAMPLES` object inside `web/main.js`.
Filed as [web-examples-js-is-unused-and-stale](../reported/web-examples-js-is-unused-and-stale.md).

### PS1 -- replay, not no-discard

Open question 1 answered: keeping the session alive is not an option. A failed
turn can leave its definitions in the session scope (a runtime failure always
does -- elaboration succeeded), and the `Elab` session is shallow-copied state
over malloc'd registries that mutate in place, so there is no cheap snapshot to
roll back to. The rollback is a **replay** instead: `TuriEnv.acc_turns` records
where each committed turn ends in `acc_forms` (and whether it ran as a stdlib
preload), and a missing session is rebuilt by elaborating those turns one call
at a time -- the calls that built it, minus the failed turn -- with diagnostics
muted (a capture frame for errors, a no-op sink for warnings). If the records
do not line up, the old whole-program path still runs. `stdlib_prefix` was not
touched.

Open question 2 answered: **no**. Incremental parsing and incremental
elaboration are gated separately, so a sweet-exp session stays on the
incremental elaboration path and the replay covers it. Tested.

Cost: after a failure, the next turn pays about what elaborating the session
originally did -- ~8 ms for a preload-sized session, ~1.1 s at 1500 `defn`
turns (the old, broken rebuild took 0.5 s there). Half of a long replay is one
pre-existing cost, filed as
[refine-call-sites-re-resolved-every-session-turn](../reported/refine-call-sites-re-resolved-every-session-turn.md).

**Found on the way, fixed:** after a `defmacro*`, *any* failed turn killed the
process with `tur: too many source files` (SIGABRT -- a dead worker in the
browser), on the old rebuild path too. The macro-time env's bracket merged the
caller's diagnostic file registry back instead of replacing it, so the macro
env's own `SourceFile`s stayed registered; a failed turn frees the macro env
with its session, and the next bracket re-registered freed memory.
`diag_files_replace` restores the registry exactly.

Test: `tur_session_failure_recovery` (11 checks fail without the replay).

### PS2 + PS3 -- read the generated file, not a second C copy of it

`turi_doc_lookup` no longer evaluates anything. The decided route was to have
`gendocs.py` emit the table a second time as C and link it in. It was not
taken: `tur doc` already read the table straight out of
`stdlib/docstrings.tur` with a small C-string scanner, and the Emscripten build
already embeds that file at `/stdlib`. That scanner moved to
`src/turi/docstrings.c` (parsed once, sorted, bsearched -- the panel looks up
on hover) and is the one reader for the doc panel, `tur doc`, and two new
interpreter natives. Same direct C lookup, with no second generated artifact to
drift and no ~480 KB added to every binary and to the playground wasm.

The `doc` macro turned out broken on **every** path, not just the playground:
it expanded to a runtime `(= entry 0)` on a `cstr` (a type error) and passed
`'name` as a `:Sym`. It now reads the name at expansion time and expands to
`(doc-print "name")`, so `(doc vec-map)`, `(doc 'vec-map)` and
`(doc "vec-map")` all work under the interpreter. That needed two compile-time
macro builtins: `symbol-name` accepts a quoted symbol in either spelling, and a
new `string?` -- non-gating, like `=`/`not`, because it is a natural runtime
predicate name. A compiled program still has no `doc-print`; the autodoc guide
points shells at `tur doc`.

Tests: `tur_wasm_glue_session_unit`, fixture `macro-ct-name-of-quoted-symbol`.

### PS4 -- every def* form, by turn watermark

Measured first, completing the plan's table (open question 3: there is no
`defadt`; `defdata` was already redefinable, by an accident of its forward-stub
reuse): `defn`, `defdata`, `defgadt`, `defalias`, `deftype`, `defopaque`,
`defclass`, `defdynamic`, `defkind`, `defrec` and `defprotocol` accepted a
redefinition; `def`/`define`, `defstruct`, `defeffect`, `definstance`,
`defmacro` and `defmacro*` refused it.

Each elaborate call that continues a live session records the sizes of the
redefinable registries (globals, macros, ADTs, effects, the instance list head).
A definition below the watermark came from an earlier turn and is replaced:
`def` shadows, macros are removed and re-registered, an effect is redefined in
place (`effect_redefine`, mirrored into the interpreter's session `EffectEnv`),
`defstruct` re-elaborates over the type like `defdata`, and an instance is
unlinked. Compiles never continue a session, so nothing changes for them.

Open question 4 answered by probing. Nothing corrupts: a value built under an
old struct layout, read through the new one, fails cleanly
(`field index 1 out of bounds`), and a redefined effect's handlers are checked
against the new signature at elaboration. What **does** break is redefining a
type the stdlib owns -- it is rewritten in place under the stdlib code built on
it (`(.fst (pair 1 2))` stops resolving after a user `Pair`) -- so stdlib types
are refused in sessions, as are stdlib instances (the 2026-09-11 decision) and
the builtin `Unsafe`. A duplicate within one turn is still an error.
`defstruct`'s refusal now names the stdlib or "an earlier form", whichever it
is, instead of both. The compiled path has the stdlib-type hole for `defdata`
already; filed as
[compiled-defdata-over-stdlib-type-rewrites-the-stdlib-type](../reported/compiled-defdata-over-stdlib-type-rewrites-the-stdlib-type.md).

Done-when met: the page's effects example, and every other example in
`main.js`'s `EXAMPLES`, produces identical results on a second Run in one
session; `tur repl` accepts the effects example twice.

Tests: `tur_session_redefinition`; `tur_incremental_elab_diff` now pins `def`
redefinition as a known divergence beside `defn` (it had been an
identical-results session only because both paths refused it).

### PS5 -- rewind, replay, run

As decided, plus the runtime half the plan did not mention:
`turi_env_reset_to_prelude` rewinds source and the elaboration session but
leaves runtime globals alone, so a deleted definition would still have resolved
at runtime. `turi_env_snapshot_prelude` records the globals once the preload
and its native overrides are in place, and `turi_env_rewind_to_prelude` frees
what turns added and restores reassigned prelude values (the snapshot's values
are promotion roots, for `tur repl`). The page calls it through
`turi_wasm_rewind_to_prelude`, then replays each other tab's last successful
Run in tab order with output muted, then runs the buffer. Only tabs in the
running tab's `#lang` dialect are replayed, since a dialect switch resets the
session anyway.

Cost, measured as the plan asked: 11-24 ms per Run in desktop Chrome for a
`fib 15` program, the stdlib preload re-elaborated each time. No caching.

Tests: `tur_wasm_glue_session_unit` (the rewind) and
`web/tests/run-session.spec.js` (effects example twice around a doc lookup; a
deleted definition stops resolving; a second tab's definitions survive a Run
without re-printing; `(doc 'vec-map)` at the prompt). The full Playwright suite
reports 188 passed and 16 failed; the same 16 fail against a dev server
running `main`, and the one of them that involves Run (`repl-intelligence`'s
"editor-only defn ... until it is Run") fails the same way with `main`'s
`main.js` and worker swapped in. The rest need CI dashboard data, the built
guide HTML, or the live site, or test the prompt and project loading.
