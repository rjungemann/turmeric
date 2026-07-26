# Graduating the `refined` experiment

**Status:** not started. This is a decision plan, not a build plan -- the
feature is implemented; what remains is deciding it is ready and removing the
gate.

**Clock:** `refined` is `XF_LIFECYCLE_PROTOTYPE`, `introduced 0.31.0`,
`expires_at 0.34.0`. `VERSION` is `0.30.8`, so the experiment has not shipped
yet and there are roughly three minor lines of runway. At the cut it graduates
(row deleted, behaviour unconditional) or is shelved. `expires_at` is a hard
contract, not a suggestion.

**Prerequisite:** see [Ordering](#ordering) -- graduation and Z3 retirement are
independent, and the order matters less than doing each deliberately.

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
   `src/compiler/lang_layers.c`. `CLAUDE.md` is explicit that a semantic layer
   must point at an `EXPERIMENTS[]` row and that graduation deletes the layer
   rather than letting layers accumulate. A file that still says
   `#lang turmeric refined` then needs the same accept-as-no-op treatment, or
   the graduation note has to tell people to drop the token -- **decide which,
   because silently erroring on a `#lang` line is a worse break than on a CLI
   flag.**
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
"break": its entire purpose is to pin what happens with the gate OFF. On
graduation there is no gate to turn off, so the fixture is either deleted or
rewritten to pin the `--no-contracts` behaviour instead. Decide deliberately;
deleting a fixture that documents a real distinction is a loss even when the
distinction stops being reachable.

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

Graduation and Z3 retirement are **independent**. Z3 is a dev-build oracle that
no shipped artifact links, so graduating with the scaffold still present ships
nothing extra, and retiring the scaffold while the gate remains changes nothing
a user sees.

Doing Z3 retirement **first** is nonetheless the better order, for one reason:
retirement is the decision that benefits from the oracle still being available
to answer "did we break anything". Graduating first would put the feature in
everyone's hands while its correctness net is still being dismantled. Neither
is a hard dependency -- this is a preference, and the deadline on `refined`
outranks it if the two ever conflict.

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
