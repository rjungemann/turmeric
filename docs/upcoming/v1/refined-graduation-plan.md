# Graduating the `refined` experiment

**Status: EXECUTED 2026-08-01, targeting v0.33.0.** All four preconditions
closed and the flip has landed. What follows is kept as the record of the
decision and the evidence behind it; the mechanical checklist below is done.

| precondition | state |
|---|---|
| 1. suite survives it | **measured** -- one fixture affected, and it is repurposed rather than deleted |
| 2. cost is acceptable | **MEASURED on a real program (2026-07-26)** -- tur-ecs, ~5400 lines / 22 modules / 66 tests: **1.004x** on unannotated code, worst per-file delta +1ms, zero `TUR-E0371`, all obligations behave as designed; see [refined-dogfood-ecs-report.md](refined-dogfood-ecs-report.md) |
| 3. nothing still moving | **MET 2026-08-01** -- the sit clock restarted 2026-07-26 and ran through the whole 0.32.x line (0.32.5 through 0.32.8) with the `ecs/sized-refined` surface exercising the three changes that restarted it. Nothing moved again; the flip lands in 0.33.0 as the recommended timeline said |
| 4. exclusions documented as permanent | **DONE** -- every Limits entry tagged |

**What landed (2026-08-01).** `"refined"` added to `GRADUATED[]`
(`src/runtime/experiments.c`) and `GRADUATED_LAYERS[]`
(`src/compiler/lang_layers.c`); both its `EXPERIMENTS[]` and `LANG_LAYERS[]`
rows deleted; all 15 `g_opt_refined` conditionals and the global itself
removed, along with the four `experiment_warn_if_used("refined")` calls.
`LANG_LAYERS[]` now holds `stringed` alone and no semantic layer.

Fixtures: `refine-off-is-contracts-only` was repurposed in both directions --
`refine-runtime-check-still-fires` (`--keep-contracts`, pins that the runtime
entry check is still emitted and still fires) and
`refine-no-contracts-strips-runtime-check` (`--no-contracts`, pins that the two
halves stayed separable). Both use a solver-opaque violating value so the
program still compiles under unconditional discharge. `refine-lang-layer`
became `refine-graduated-lang-layer-noop` (`TUR-W0064`) and
`refine-graduated-enable-noop` is new (`TUR-W0063`) -- the two compatibility
acceptance fixtures. The now-redundant `--enable=refined` was stripped from 73
fixture `flags` files (48 rewritten, 25 deleted as empty), deliberately: left
in place they would all have broken at once when the `GRADUATED[]` entry ages
out, and the shim is better exercised by two fixtures that test it on purpose
than by 73 that depend on it incidentally.

**Not in the checklist, and nearly missed: the source-level fuzzer needed a new
reference leg.** `tests/refine-fuzz-src.py` compiles each generated program
twice and compares; its reference leg was spelled "omit `--enable=refined`".
Graduation makes that flag a no-op, so both legs became identical builds -- the
harness would have gone on reporting PASS while proving nothing, which is worse
than failing. This is the only place graduation cost real coverage, and it is
load-bearing: the corpus README calls this fuzzer half the standing
solver-soundness coverage, and three archived bugs were found by it.

No shipping flag reconstructs the leg. `--no-contracts` emits NO checks;
`--keep-contracts` emits checks MINUS whatever discharge elided; and the elided
set is exactly what the miscompile property is about (both known refinement
soundness bugs proved something false and dropped the check that would have
caught it). So `TUR_REFINE_NO_DISCHARGE` was added as an **env-only test
seam** -- not an experiment, not a CLI flag, no `EXPERIMENTS[]` row, alongside
the existing `TUR_REFINE_STATS`/`TUR_REFINE_DUMP`. It is one early return in
`refine_collect_obligation`, the single chokepoint every obligation flows
through; all six callers already treat a NULL obligation as "not proven", so
nothing is decided, nothing is elided, and no refinement diagnostic fires.

Yes, this re-adds a conditional the flip otherwise deleted. The rule it bends
("delete the conditionals rather than hard-coding the global") is about not
leaving a dead *feature gate* for readers to wonder about; this is a documented
testing seam with one caller and a comment saying so.

Verified after the change: self-test PASS with the reference leg genuinely
aborting (`off=abort` on the two impure fixtures -- degenerate legs would have
shown `off=reject`), and n=100 on seeds 1 and 2 both clean, 0 soundness bugs
and 0 other BUG classes, with the legs visibly diverging (40 `agree_abort` and
44/37 `agree_rejected_early` per seed). That closes the plan's "source-level
fuzzer clean across at least two seeds" acceptance criterion.

**Z3 retirement: DONE, executed 2026-07-30 in 0.32.5.** The input was banked
2026-07-26 -- the oracle build (system Z3 4.15.4) re-checked every VC the
tur-ecs corpus generates, verdicts identical to the in-house chain, zero
`TUR-I0379`. The scaffold is now deleted: `refine_libz3.c`, the
`TUR_REFINE_Z3_ORACLE` option, the `find_package(Z3)` block, the VC-level
differential fuzzer, and every `#ifdef`. What survives as the standing
regression is `tur_refine_corpus` (125 labelled benchmarks, no solver linked,
0 soundness failures) plus the source-level fuzzer `tests/refine-fuzz-src.py`,
which never needed an oracle. `TUR-I0379` is retired-but-reserved in `diag.h`.

| prerequisite | state |
|---|---|
| graduated-layer shim (`GRADUATED_LAYERS[]`) | **DONE and verified**, landed empty ahead of use |

**Clock:** `refined` is `XF_LIFECYCLE_PROTOTYPE`, `introduced 0.31.0`,
`expires_at 0.34.0`. `VERSION` is `0.32.5`, so the experiment is shipping with
roughly two minor lines of runway. At the 0.34.0 cut it graduates (row deleted,
behaviour unconditional) or is shelved.

`expires_at` is **advisory and never blocks a release** -- per CLAUDE.md, no
registry check has ever existed in the release-cut skills. An earlier revision
of this plan called it "a hard contract, not a suggestion" in two places; that
was wrong, it is the prose that has stranded releases before, and it is
corrected here and in [Ordering](#ordering). Treat 0.34.0 as the review point
at which someone decides, not a wall that decides for them.

**Ordering:** corpus work, then gated dogfooding, then Z3 retirement, then
graduation. The first three are **done**; only the flip remains. See
[Ordering](#ordering).

---

## Decision review 2026-07-26 -- where the flip stands

The ordering's first three steps are done (step 3 executed 2026-07-30); what
remains is the sit clock and the flip itself.

**Ordering progress.**

| step | state |
|---|---|
| 1. corpus | **clean** -- in-tree corpus 119/119 parsed, 0 soundness failures, 0 over-budget; the external 200-sample reader tail (193 parsed) is `corpus-reader-tail-plan.md`, which declares itself optional |
| 2. gated dogfooding + oracle build | **DONE** -- [refined-dogfood-ecs-report.md](refined-dogfood-ecs-report.md); oracle re-run at head over all 12 refined ecs tests (including the `ecs/sized-refined` promotion surface, which generates novel crossing shapes): verdicts identical, zero `TUR-I0379` |
| 3. Z3 retirement | **DONE 2026-07-30 (0.32.5)** -- executed as its own change set during the sit window, as planned. Suite unchanged by it: 2442 passed / 7 failed, and all 7 are pre-existing HKT carrier-cast codegen failures verified to fail identically at HEAD without the change |
| 4. graduation | waiting on the sit clock (below) -- now the ONLY remaining step |

**The stop-list, scored against the dogfood run.** None of the three
would-stop conditions fired: zero `TUR-E0371` anywhere (so none on correct
code); cost on a real program is 1.004x against the 1.7x fixture worst case
(the fear inverted); `TUR-W0377` fired zero times (no noise signal). Nothing
in the evidence argues for shelving.

**The sit clock (precondition 3) restarted 2026-07-26.** That day landed:
the `#reads` entry-contract suppression, the macro-expansion crossing path
walk (`refine_note_macro_expansion` + the set!-scan depth change), and the
template-emitter fixes (`#reads`/`#refine` through macro copiers). All are
completeness-direction changes (more discharges, never fewer checks), but
they are semantics, and graduation freezes them. Let them sit through at
least one minor line with the ecs surface exercising them.

**Adjudicated: the `::` trust boundary does NOT gate graduation.** Raised
deliberately here because graduating makes the stateful slice's story
permanent. What graduates includes `#reads`/`frozen` (they ride the same
gate), and their guarantee can be stepped around by a deliberate `::`-cast /
inline C
(`docs/reported/frozen-region-aliasing-via-coercing-cast.md`). Decision:
ship it as a documented-permanent limit, do not block on sealing. Reasons:
(a) the failure mode under a deliberately false declaration is the
forgiving DEFAULT semantics (a stale in-bounds read) wearing a proven badge
-- no elision, no new unsafety, because an impure measure's entry contract
was never emittable and compile-time rejection remains intact for ordinary
code; (b) the boundary is now a tagged **[by design]** entry in the guide's
Limits section (added with this review -- precondition 4 stays whole); (c)
`::`-sealed newtypes / module-private construction is an independent
language feature, and holding the experiment hostage to it serves neither.
The report stays open on its own merits.

**Scope statement.** Graduating `refined` graduates the pure core AND the
stateful slice as implemented (trusted tier, step 1 of the
`checked-write-frames-plan.md` trajectory). The alternative -- splitting
`#reads` into a second experiment so the core graduates alone -- is
rejected: it would mint a second enable path (what the `#lang` layer rules
forbid), and the slice is load-bearing for the flagship consumer (the ecs
strict-aliveness family is built on it, and the dogfood report's
annotation-burden finding was that stateful measures are what real code
reaches for first).

**Recommended timeline.** Sit through the 0.32.x line (with the denser
`ecs/sized-refined` corpus as the soak vehicle -- a second dogfood
measurement round there is cheap and would refresh the cost figure);
execute Z3 retirement during the sit; flip in 0.33.x, comfortably inside
the `expires_at 0.34.0` contract rather than at the wire.

---

## What graduation means

Every use of `#refine{...}` gets **static discharge unconditionally**. It does
not change the runtime contract layer, which is already always-on: a refinement
predicate is already enforced at run time whether or not the gate is set. What
becomes unconditional is the *static* half -- proving obligations, eliding
nothing, and reporting `TUR-E0371` on a definitely-wrong argument.

So the user-visible change is **new compile-time errors on programs that
currently compile**, in exactly one class: a refinement violated on every
execution reaching it. That is the feature working, but it is still a
behavioural change and belongs in a release note.

## The mechanical checklist

Verified against the current tree; each item is small.

1. **Delete the `EXPERIMENTS[]` row** for `refined` in
   `src/runtime/experiments.c`.
2. **Add `"refined"` to `GRADUATED[]`** in the same file, so a lingering
   `--enable=refined` in a downstream `build.tur` or
   `~/.config/turmeric/experiments.tur` is accepted as a no-op (`TUR-W0063`)
   rather than the hard `TUR-E0310` an unknown name gets. Entries age out one
   minor line after graduation, matching `cps-effects` and friends.
3. **Delete the `#lang turmeric refined` layer row** in
   `src/compiler/lang_layers.c` -- but **NOT before building a soft landing for
   it, which does not currently exist.** `CLAUDE.md` is explicit that a semantic
   layer must point at an `EXPERIMENTS[]` row and that graduation deletes the
   layer rather than letting layers accumulate. Deleting the row today has a
   sharper consequence than the CLI flag does:

   | reference after graduation | result |
   |---|---|
   | `--enable=refined` | `TUR-W0063`, compiles fine |
   | `:experiments [:refined]` in `build.tur` | `TUR-W0063`, compiles fine |
   | `#lang turmeric refined` | **`TUR-E0330` hard error, the file stops compiling** |

   Verified: an unrecognised layer token is `TUR-E0330: unknown #lang layer`,
   and `lang_layers.c` has **no `GRADUATED[]` equivalent** -- the soft-landing
   path exists for experiments and not for layers. So graduation as written
   breaks every file that opted in per-file, which is the population most likely
   to have adopted the feature deliberately.

   **DONE (landed ahead of graduation).** `GRADUATED_LAYERS[]` and
   `lang_layer_is_graduated()` now exist in `lang_layers.c`, mirroring
   `GRADUATED[]` in `experiments.c`; the reader accepts and ignores a listed
   token with a one-time `TUR-W0064`. The list is deliberately **empty** -- no
   layer has graduated yet.

   So step 3 is now two lines: add `"refined"` to `GRADUATED_LAYERS[]` and
   delete the `LANG_LAYERS[]` row, in the same commit. The shim landing first
   is the point: adding it afterwards would ship one release in which every
   `#lang turmeric refined` file breaks.

   Verified with a temporary entry before landing empty -- a graduated token
   warned once and compiled on both the compiled and interpreter paths, an
   unknown token still reported `TUR-E0330`, and the live `stringed` layer was
   unaffected.
4. **Retire `g_opt_refined`.** 15 call sites read it: `elab_fns.c` (9),
   `elab_typeclasses.c` (4), `elab_call.c` (1), `elab_toplevel.c` (1). Delete
   the conditionals rather than hard-coding the global to `true`, so a reader is
   not left wondering what the dead flag was for.
5. **Remove the `experiment_warn_if_used("refined")` calls**, which otherwise
   warn about an experiment that no longer exists.
6. **Update the docs**: the guide's gating section, this plan's `Gate:` line,
   `CHANGELOG`, and the `#lang` layer table in the syntax guide.

## What has to be true first

### 1. The suite survives it -- MEASURED, essentially clean

58 fixtures use `#refine`; 51 already run gated. The 7 ungated ones were run
with the gate forced on:

| fixture | forced-on result |
|---|---|
| `contract-type` | clean |
| `refine-basic` | clean |
| `refine-class-result` | clean |
| `refine-in-defn` | clean |
| `refine-lang-layer` | clean |
| `errors/refine-impure-predicate` | already an errors fixture |
| `refine-off-is-contracts-only` | **1 diagnostic -- its premise is gate-off** |

So the blast radius inside the tree is one fixture, and that one does not
"break": its entire purpose is to pin what happens with the gate OFF.

**DECIDED: repurpose it, do not delete it.** On graduation there is no gate to
turn off, but the distinction the fixture documents does not disappear -- it
moves to `--no-contracts`, which still strips the runtime half. Rewrite it to
pin that, and rename it accordingly. A fixture that documents a real
distinction is worth keeping even when the way you reach that distinction
changes.

### 2. The cost is acceptable -- MEASURED, small but not zero

Median of 5 runs, `tur check`:

| file | gate off | gate on |
|---|---|---|
| `refine-crossing-path-conditions` | 200ms | 338ms |
| `refine-ctor-axioms` | 157ms | 172ms |
| `stdlib/refine.tur` | 164ms | 153ms |

The crossing-heavy fixture is the worst case at ~1.7x, and it is a file written
to stress crossings. A file with no `#refine` at all pays nothing -- no
obligations are collected, so the chain is never entered. **The cost is
proportional to refinement use, which is the right shape for an always-on
feature**, but the 1.7x figure should be re-measured on a real program before
the cut, not just on fixtures designed to be hard.

### 3. Nothing is still moving

Graduation is a promise that the semantics are stable. Two things landed very
recently and should sit for a while first:

- the class-parameter reading (instance governs at a static site, `TUR-W0377`
  lints the leniency) -- a *design decision*, not a bug fix, and the kind of
  thing that is embarrassing to reverse after graduation;
- closedness measured on the goal, which widened `TUR-E0371` to any caller with
  parameters in scope. That is the change most likely to surprise someone, and
  it is exactly what graduation makes unconditional.

### 4. The known exclusions are documented as permanent, not pending -- DONE

Graduating with `docs/reported/` entries open is fine; graduating while the
guide describes gaps as temporary is not, because graduation is when those
descriptions become promises to every user rather than to opt-in ones.

Every entry in the guide's **Limits** section now carries a tag, and the
section opens with a legend saying what each means:

| tag | count | meaning |
|---|---|---|
| **[by design]** | 8 | a deliberate decision, usually forced by soundness or the adoption philosophy; will not change |
| **[incomplete]** | 3 | could be improved, nobody is working on it |
| **[prototype]** | 2 | needs a design change this prototype excludes; not planned |
| **[deferred]** | 1 | has a written plan and a trigger condition |

The distinction that matters for graduation is **[prototype]** and **[by
design]** against **[incomplete]**: the first two are promises that the
behaviour is intended, the third is an admission that it could be better. Both
are fine to ship; describing one as the other is not.

The section also states plainly that none of the limits is a bug being worked
around -- every one lands on the safe side of the one-directional invariant,
where the worst outcome is an obligation that falls back to the runtime check
it would have had anyway.

## Ordering

**DECIDED: corpus work, then gated dogfooding, then Z3 retirement, then
graduation.** Reasoning, including the argument that nearly reversed it.

Graduation and Z3 retirement are technically **independent**. Z3 is a dev-build
oracle that no shipped artifact links, so graduating with the scaffold present
ships nothing extra, and retiring the scaffold while the gate remains changes
nothing a user sees. The order is therefore a judgement about where risk lands,
not a dependency.

The case for **graduating first** is stronger than it first appears, and worth
stating properly: the oracle's marginal value is highest exactly when NOVEL VCs
are arriving, and novel VCs arrive when real programs use the feature. Retiring
the oracle just before real usage begins retires it at the moment before it
would have been most useful.

What defeats that argument is that **graduation is not the only way to get
usage**. The feature can be dogfooded gated -- `--enable=refined` in a real
project -- and that produces the same novel VCs while the oracle is still
present to cross-check them, without putting every user on the feature at the
same time as the net comes down. Two risks at once is the thing to avoid, and
gated dogfooding avoids it while keeping the oracle's benefit.

So the sequence is:

1. **Corpus work** -- raise what the reader handles (see
   [corpus-reader-tail-plan.md](corpus-reader-tail-plan.md)).
2. **Gated dogfooding, with an oracle build available.** Build the dogfooded
   project once under `-DTUR_REFINE_Z3_ORACLE=ON`; that is the cross-check on
   VCs that real programs generate, which is the evidence retirement actually
   needs and which no corpus can supply.
3. **Retire Z3** once the corpus and the dogfooding are both clean.
   **DONE 2026-07-30 in 0.32.5.**
4. **Graduate** after that. -- the only step left.

**If the review point arrives first, prefer graduating to stalling.**
`expires_at` is `0.34.0`. Steps 1--3 are done, so this no longer bites, but the
principle stands for the next experiment: if the ordering runs long, graduate
first and finish the scaffold cleanup afterwards.

An earlier revision ended this section "the order is a preference; the deadline
is a contract." The second half is **wrong** and is corrected: per CLAUDE.md,
`expires_at` is advisory and never blocks a release -- the release-cut skills
surface an expiring row and proceed. Graduating early is routine; being at or
past expiry is not a reason to refuse a version bump.

### A caution on "200/200"

The corpus target is easy to state in a way that cannot be met. The two numbers
are different:

| metric | now | ceiling | reachable by |
|---|---|---|---|
| **parsed** | 193 / 200 | 200 | reader work -- the 7 remaining skips |
| **decided** | 142 / 200 | **189** | solver throughput, not reader work |

11 benchmarks carry no `:status` at all, so they can never be decided -- 189 is
the real ceiling, not 200. And the gap from 142 to 189 is **40 over-budget plus
7 skipped**: overwhelmingly the solver, not the reader. Closing all 7 skips
moves "decided" by a handful at best, because the benchmarks that defeat the
reader are the large ones that then exceed the budget anyway.

So "close to 200/200" is achievable for PARSED and is not achievable for
DECIDED without a different project -- faster arithmetic, a real simplex,
incremental EUF. **Retirement should be gated on parsed coverage plus a clean
soundness record, not on a decided count**, because incompleteness was never
what the corpus is defending against: `RT_UNKNOWN` is always safe, and a
benchmark the chain declines to decide cannot break the invariant.

## Shelving, if it comes to that

The alternative at the cut is shelving: `lifecycle` moves off
`XF_LIFECYCLE_PROTOTYPE`, `expires_at` moves out, and the plan says why. That is
a legitimate outcome and should not be treated as failure -- but it needs a
reason recorded, because "we ran out of time" and "the semantics are not settled"
call for different follow-ups.

## The flip, as a sequence

Everything below is one change set. Splitting it across commits ships an
intermediate state in which the gate is half-removed.

1. Add `"refined"` to `GRADUATED[]` (experiments) **and** to
   `GRADUATED_LAYERS[]` (layers), then delete both its `EXPERIMENTS[]` row and
   its `LANG_LAYERS[]` row.
2. Delete the 15 `g_opt_refined` conditionals and the global itself; delete the
   `experiment_warn_if_used("refined")` calls.
3. Repurpose `refine-off-is-contracts-only` to pin `--no-contracts` and rename
   it; add a fixture each for `--enable=refined` (`TUR-W0063`) and
   `#lang turmeric refined` (`TUR-W0064`) still being accepted.
4. Docs: the guide's status banner and "Turning it on" table, the `#lang` layer
   table in the syntax guide, this plan's status, `CHANGELOG`.

## Acceptance criteria

- `--enable=refined` accepted as a no-op with `TUR-W0063`, and
  `#lang turmeric refined` with `TUR-W0064` -- a fixture each. These are the
  compatibility promise; without them graduation is a breaking change for
  everyone who opted in.
- No `EXPERIMENTS[]` row, no `LANG_LAYERS[]` row, no `g_opt_refined`, no
  `experiment_warn_if_used("refined")`.
- Suite green, corpus green, source-level fuzzer clean across at least two
  seeds.
- **A dogfooding report exists** and its compile-time figure is on a real
  program, not a fixture.
- A CHANGELOG entry stating the one user-visible consequence in a sentence: a
  refinement violated on every execution reaching it is now a compile error.

## What would make me stop

Worth writing down in advance, because the deadline creates pressure to
proceed:

- the dogfooding run produces `TUR-E0371` on code that is **correct** -- that is
  a false positive going unconditional, and it is the one outcome that should
  halt the flip outright;
- compile time on a real program is materially worse than the fixture
  measurement suggests (the fixtures say ~1.7x worst case; something like 3x
  sustained would want investigating before it becomes everyone's cost);
- `TUR-W0377` fires often and its advice reads as noise, which would mean the
  class-parameter reading needs revisiting before it is frozen.
