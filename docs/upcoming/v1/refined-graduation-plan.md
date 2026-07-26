# Graduating the `refined` experiment

**Status:** not started. This is a decision plan, not a build plan -- the
feature is implemented; what remains is deciding it is ready and removing the
gate.

**Clock:** `refined` is `XF_LIFECYCLE_PROTOTYPE`, `introduced 0.31.0`,
`expires_at 0.34.0`. `VERSION` is `0.30.8`, so the experiment has not shipped
yet and there are roughly three minor lines of runway. At the cut it graduates
(row deleted, behaviour unconditional) or is shelved. `expires_at` is a hard
contract, not a suggestion.

**Ordering:** corpus work, then gated dogfooding, then Z3 retirement, then
graduation -- see [Ordering](#ordering) for why, and for the deadline that
overrides it.

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

   **Work item, not just a decision:** add a graduated-layers list mirroring
   `GRADUATED[]` in `experiments.c`, so a retired layer token warns and is
   ignored for one minor line. `refined` would be its first entry; `stringed` is
   the only other layer and is not graduating. Do this BEFORE deleting the row,
   or the two changes have to land together.
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

### 4. The known exclusions are documented as permanent, not pending

Graduating with `docs/reported/` entries open is fine; graduating while the
guide describes gaps as temporary is not. These are design exclusions and the
guide should say so plainly:

- higher-order callees (needs refinements in function types);
- a class parameter refinement not binding callers at a **static** site
  (deliberate -- see the archived report);
- nested datatype shape beyond one level.

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
4. **Graduate** after that.

**The deadline outranks this preference.** `expires_at` is `0.34.0`. If steps
1--3 run long, graduation gets forced first; take that rather than let the
experiment expire, and retire the oracle afterwards. The order is a preference;
the deadline is a contract.

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

## Acceptance criteria

- `--enable=refined` and `#lang turmeric refined` both accepted as no-ops with
  `TUR-W0063`, verified by a fixture each.
- No `EXPERIMENTS[]` row, no `LANG_LAYERS[]` row, no `g_opt_refined`.
- Suite green, corpus green, source-level fuzzer clean across at least two
  seeds.
- A CHANGELOG entry that states the one user-visible consequence in a sentence:
  a refinement violated on every execution reaching it is now a compile error.
