# Checked write frames -- `#writes`, frame-aware hypothesis invalidation

> **Status 2026-08-01 -- WF3's narrow slice is LANDED. WF1/WF2/WF4 remain
> proposed.**
>
> **WF3 needed neither WF1 nor WF2.** The plan sequences the phases as
> vocabulary -> checked tier -> payoff, which reads as though the payoff
> depends on the first two. It does not: the narrow slice is a *local* analysis
> of the caller body, and every case that would need a callee's declared frame
> (a call with `^mut` args, a nonempty `#writes`) is in the "keeps the full
> decline" bucket. So the payoff shipped first, and `#writes` is still
> unbuilt.
>
> What landed: `rt_collect_set_targets` / `rt_form_mentions_name` /
> `rt_form_borrows_name` in `src/compiler/elab_fns.c`, replacing the single
> `rt_form_mentions_set` veto in `rt_push_cs_path_conds`. A hypothesis now
> survives an assignment iff every assignment in the body targets a plain
> symbol the hypothesis does not mention and the body never borrows. Anything
> else -- place-expression target, an assignment symbol this scan cannot
> attribute, depth or slot exhaustion, a borrowed target -- restores the old
> whole-body decline.
>
> One argument the sketch below leaves implicit, written down because the
> slice's soundness rests on it: **the assignment's VALUE needs no check.** A
> hypothesis is only usable if its terms are congruent, and congruence is
> granted only to a pure measure or to a `#reads` measure inside a region that
> freezes its argument (`enc_reads_arg_frozen`). A pure measure cannot be
> disturbed by any call; inside a frozen region a mutator of the frozen world
> is statically unreachable (`TUR-E0200`). So a call in value position cannot
> stale a hypothesis that was going to be believed. That is also why
> `(set! acc (+ acc (get! w e)))` -- whose value position calls the refined
> accessor -- is rescuable at all.
>
> Evidence: suite 2510 -> **2512 passed, 0 failed**; source fuzzer
> `--n 200 --mode both` at seeds 1 and 2, **0 soundness bugs, 0 other BUG
> classes**. Sabotage run (strip all three guards, never decline): all three
> negatives -- `errors/refine-stateful-mutation-invalidates`,
> `errors/refine-macrogen-set-in-expansion`,
> `errors/refine-wf3-borrowed-target` -- **wrongly prove**, and correctly
> decline in the shipped build, so each guard is load-bearing. The sabotage
> hook was removed before commit.

**Status:** proposed. This is the plan for step 2 of the trajectory
`stateful-refinements-guide.md` sketches ("trusted now -> checkable later ->
effect-row eventually"), written down because two concrete demand signals now
exist (below). `refine-stateful-measures-plan.md` RM-S2 item 1 anticipated it:
"a new effect row remains an option if B2/B3 need mutation facts the cap
cannot carry, but is not the starting point." The cap was the right start; this
is the option, scoped.

## The two conservatisms this addresses

**1. The whole-body `set!` decline is boolean, not frame-aware.** The crossing
path-condition collector (`rt_push_cs_path_conds`, `elab_fns.c`) abandons
EVERY path condition for EVERY crossing in a caller body the moment ANY
`set!`/`swap!`/`reset!` appears anywhere in it (`rt_form_mentions_set`) -- even
when the assignment provably cannot touch what the hypothesis talks about.
Concretely, both of these decline today:

- `(for-each-alive! w n e (set! acc (+ acc (get! w e))))` -- the accumulator
  `set!` is about `acc`; the hypotheses that would discharge the read are about
  `w` and `e`. The aliveness guard is dropped anyway, so the ergonomic macro
  only takes PURE bodies (hit while shipping RE1 (c); the shipped tests use
  `println` bodies for exactly this reason).
- any `while`-lowered loop -- the counter's `set!` kills everything, including
  facts about state the counter never touches.

The decline is the right default (a condition mentioning reassigned state may
no longer hold), but it is the COARSEST correct rule, and it is now the
binding constraint on two shipped surfaces.

**2. Trusted `#reads` can never back optimization.** The guide is explicit:
elision, reordering, CSE all *act* on the claim, so they need the claim
checked, not trusted. The concrete case: the resizable-buffer bounds pattern
(`frozen` + `#reads` on `(in-bounds? buf i)`) PROVES today -- fixture
`refine-stateful-resizable-bounds` -- but `buf-get`'s own internal check is
kept by design, because the whole chain rests on a trusted annotation. A
checked frame is the gate to dropping it. (The SIZED-world bounds case needs
none of this -- capacity is a type-level constant, the proof is pure, and the
recursion shape discharges it today; see the RE2 probe update in
`v1/ecs-refinement-typed-apis-plan.md`.)

## What this is NOT (honesty first)

- **Frame-aware invalidation does not give `while` loops their bounds
  facts.** A `while` bound (`i < n`) is *about the counter*, and the counter
  IS reassigned -- declining it is correct, and only a loop invariant (C3,
  `hold/loop-invariants-plan.md`) or the recursion shape supplies it. What
  frames rescue is facts about OTHER state surviving nearby mutation. The two
  features compose; neither subsumes the other.
- **Not the effect-row unification** (step 3): subsuming the ECS caps,
  feeding parallelization/CSE, merging with `#fx{...}`. Recorded as the
  horizon, deliberately unplanned -- every one of those elides or reorders
  real work and needs the checked tier to be proven trustworthy in the field
  first.

## Design sketch

### WF1 -- `#writes w` declarations (trusted tier)

Parallel to `#reads w`: a per-argument write frame on a defn, parsed and
carried on the binding. Same coarseness (whole argument, not per-field), same
trust boundary for inline-C bodies, same lifecycle home (the `refined`
experiment). No semantics yet beyond the declaration existing -- WF1 is the
vocabulary.

### WF2 -- the checked tier for pure-Turmeric bodies

A body with no inline C is walkable: verify that every `set!` target, every
`^mut`/`^unique ^mut` argument pass, and every callee's own declared `#writes`
frame stays inside the declaring function's frame. Diagnostic on violation
(the `#reads` analog: a declared frame the body exceeds is an error, not a
silent widening). Inline-C bodies remain trusted-with-declaration -- the same
step `#reads` deliberately took -- so the tier split is
checked-when-checkable, never checked-by-pretending.

### WF3 -- frame-aware hypothesis invalidation (the payoff)

Replace the boolean `rt_form_mentions_set` decline with a per-hypothesis
test: a hypothesis dies iff the body's writes CAN touch what it mentions.
Ship the narrowest sound slice first:

- a `set!` whose target is a plain local scalar (never borrowed, not a
  parameter, no reference/struct type) invalidates only hypotheses that
  mention that name;
- everything else -- field writes, writes through borrows, calls with `^mut`
  args or nonempty `#writes` frames touching a mentioned binding, anything
  aliasable -- keeps the full decline.

That slice alone un-blocks the accumulator body in `for-each-alive!` (the
`acc` case above) and preserves frozen-region facts across counter mutation
in `while` bodies. Widening beyond it (field-granular frames, borrow-aware
disjointness) only with a demand signal, and each widening carries its own
adversarial fixtures -- the failure mode here is exactly the miscompile shape
the refinement work has found three times: a stale hypothesis proving a fresh
lie.

### WF4 (deferred) -- elision on checked frames

Once WF2's checked tier exists and has soaked: allow a CHECKED `#reads`
measure's kept entry check to be elided inside a region that freezes its
frame -- the resizable-bounds case's last step. Requires the graduation-grade
evidence bar (the parent plan's "whole-program entry-check elision: measured,
declined" section is the standing warning that this must be profiled, not
assumed).

## Validation

- WF2: fixtures for a frame the body honors, a frame the body exceeds (each
  write channel: direct `set!`, `^mut` pass, transitive callee frame), and an
  inline-C body staying trusted.
- WF3: the `for-each-alive!` accumulator body proves; the sneaky-set cases
  (`errors/refine-macrogen-set-in-expansion`, `refine-stateful-mutation-
  invalidates`) STILL decline -- they mutate mentioned state; a
  borrowed-then-written local still declines; the differential fuzzer gains a
  shape that mixes disjoint and overlapping writes and stays at 0 soundness
  bugs.

  > **Correction 2026-08-01 -- that list was self-contradictory, and the
  > contradiction is instructive.** "They mutate mentioned state" was true of
  > `refine-stateful-mutation-invalidates` (`(set! (.n w) 9)` -- a field write
  > on the very world the hypothesis is about) and **false** of
  > `refine-macrogen-set-in-expansion`, which assigned `q`, a plain local
  > `int` no hypothesis mentions. A rule that declined the latter would be
  > declining the accumulator case in the same breath -- they are the same
  > shape -- so "the accumulator proves" and "the macrogen fixture still
  > declines" could not both hold.
  >
  > What that fixture was really guarding is the **macro-expansion hop**: an
  > assignment hidden in a template must be seen at all. That property is
  > still essential (the frame walk keeps the hop, and dropping it would be
  > unsound), so the fixture was rewritten to assign `e` -- the entity the
  > guard is about -- which still exercises the hop and is genuinely
  > unsound to rescue. The disjoint half it used to cover moved to the new
  > positive fixture `refine-wf3-disjoint-set`, and the borrow guard got its
  > own negative, `errors/refine-wf3-borrowed-target`.
- Every rescue is provable-disjoint by construction; when in doubt the old
  boolean decline is the fallback.

## Non-goals

- CSE / reordering / parallelization on frames (step 3; unplanned).
- Subsuming the ECS `ReadCap`/`WriteCap` machinery (works today, checked,
  linear; replacing it is only worth it after WF2 exists).
- Per-field or heap-location frames (Dafny-precision); per-argument
  granularity matches `#reads` and covers both demand signals.
- Any change to the trusted `#reads` semantics -- it stays step 1, unchanged.

## References

- `docs/guides/stateful-refinements-guide.md` -- the trajectory this plans
  ("The intended trajectory"), and the trust-boundary argument WF2 inherits.
- `docs/upcoming/v1/refine-stateful-measures-plan.md` -- RM-S2 item 1 (the
  "effect row remains an option" note this makes concrete), and the B3 spike's
  invalidation-site inventory WF3 joins.
- `docs/upcoming/v1/ecs-refinement-typed-apis-plan.md` -- the RE2 probe
  update (recursion shape needs none of this; `while` + side-effecting bodies
  are the demand signals).
- `docs/upcoming/hold/loop-invariants-plan.md` -- C3; composes with WF3,
  neither subsumes the other.
- `tests/fixtures/refine-stateful-resizable-bounds` -- the trusted slice of
  the WF4 case, working today.
