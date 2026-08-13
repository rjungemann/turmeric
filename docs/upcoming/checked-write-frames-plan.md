# Checked write frames -- `#writes`, frame-aware hypothesis invalidation

> **Status 2026-08-02 -- WF1, WF2, and the WF3 borrow widening are LANDED.
> WF4 is RETIRED: its premise turned out to be false.**
>
> Behind `--enable=write-frames` (a new experiment; `#reads`'s old home, the
> `refined` experiment, graduated 2026-08-01). The annotation always parses;
> the gate withholds the checking and the acting.
>
> - **WF1** -- `#writes w` / `#writes [a b]`, parsed to `(writes ...)` stamped
>   `PROV_WRITES`, resolved to a per-argument bitmask on the `Binding`.
>   `writes_declared` is deliberately separate from the mask: an empty frame
>   ("writes nothing") and no frame ("unknown, assume anything") are different
>   claims, and collapsing them would silently upgrade every un-annotated
>   function in the tree to the strongest frame there is.
> - **WF2** -- `wf_resolve_write_frames`, a deferred pass returning one of three
>   verdicts per function: VERIFIED (`writes_checked = true`), EXCEEDED
>   (TUR-E0382), or UNVERIFIED (no diagnostic; the declaration still documents
>   intent and nothing optimizes on it). The third case being a downgrade rather
>   than an error is what keeps `#writes` adoptable -- an error would mean no
>   function could carry a frame until its whole transitive callee set did.
> - **WF3 widening** -- the borrow guard now asks instead of assuming. A
>   borrowed assignment target is rescued when every borrow of it provably
>   reaches nothing that writes; `errors/refine-wf3-borrowed-target` keeps
>   pinning the un-widened behavior because it does not enable the experiment.
>
> Evidence: suite 2512 -> **2521 passed, 0 failed**; source fuzzer `--n 200
> --mode both` at seeds 1 and 2, **0 soundness bugs, 0 other BUG classes**
> (the generator emits no `#writes`, so that is a no-regression signal on the
> un-widened path -- the fixtures carry the new one). Sabotage run, each guard
> stripped and rebuilt in turn: **all five** negatives wrongly pass without
> their guard and correctly fail with it, so each is load-bearing -- WF2
> channels 1/2/3, the WF2 macro-expansion hop, and the WF3 reaches-a-writer
> test.
>
> ### WF4 is retired -- the check it would elide does not exist
>
> WF4 proposed eliding "a CHECKED `#reads` measure's kept entry check" inside a
> region that freezes its frame. **There is no such check, and there never
> was.** `rt_inject_param_checks` (`elab_fns.c:129`) skips entry-check injection
> for any parameter whose refinement mentions a `#reads` measure, because such a
> predicate is impure and a runtime contract for it would be TUR-E0375. The
> emitted C for the motivating fixture confirms it: `buf_hyget` in
> `refine-stateful-resizable-bounds` carries no contract check of any kind.
> The elision WF4 wanted already happened, and it happened when `#reads`
> shipped.
>
> The plan's premise -- "`buf-get`'s own internal check is kept by design,
> because the whole chain rests on a trusted annotation" -- conflated two
> different checks. What the guide calls "the accessor's own internal check is
> the backstop" is the *author's* defensive check inside their own inline-C
> body. A compiler cannot remove that, and the frame that would license
> removing it is precisely the one WF2 **cannot** produce: an inline-C body is
> the trusted tier by construction. WF4's own motivating case is inline-C on
> both ends (`in-bounds?` and `buf-get`), so it can never reach the checked
> tier at all. That circularity is the finding.
>
> The one entry check that IS kept-and-elidable belongs to a **pure** measure
> (verified: a `#refine{ x : int | (> x 0) }` parameter emits
> `tur_contract_check` even when the caller proved the crossing). Eliding it is
> whole-program entry-check elision, which has already been **measured and
> declined** -- 0 ms and 0 bytes of benefit against the largest soundness
> surface in the feature, and 14 soundness bugs under naive sabotage. See
> `refinement-types-plan.md`, "Whole-program entry-check elision: measured,
> declined (2026-07-25)". It is also not a write-frames feature: a pure measure
> cannot be disturbed by mutation, so a frame tells you nothing you did not
> already know.
>
> Nothing replaces WF4. If a future demand signal wants an elision here, it
> needs a *stateful measure with a Turmeric-visible body* -- struct fields
> rather than an inline-C handle, per the guide's step 2 -- which is a
> different starting point than this plan assumed.

> **Status 2026-08-01 -- WF3's narrow slice is LANDED (superseded by the block
> above; kept for the reasoning, which still holds).**
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
checked, not trusted. The principle is right and is what WF2 delivers.

> **Correction 2026-08-02 -- the CONCRETE CASE this cited was wrong, and it
> was the sole demand signal for WF4.** The claim was: the resizable-buffer
> bounds pattern (`frozen` + `#reads` on `(in-bounds? buf i)`) proves today --
> fixture `refine-stateful-resizable-bounds` -- but `buf-get`'s own internal
> check is kept by design, so a checked frame is the gate to dropping it.
>
> `buf-get` has no check to drop. The emitted C for that fixture contains no
> contract check of any kind, because `rt_inject_param_checks` skips entry
> checks for `#reads`-measure refinements (impure predicate; a runtime
> contract would be TUR-E0375). And the check the *guide* means by "the
> accessor's own internal check is the backstop" is the author's own defensive
> code inside their inline-C body, which no compiler pass can remove -- and
> which WF2 structurally cannot license removing, since an inline-C body is
> the trusted tier by construction.
>
> The principle survives intact; only this illustration of it was false. WF2
> and the WF3 widening stand on demand signal 1, which is real.

(The SIZED-world bounds case needs none of this -- capacity is a type-level
constant, the proof is pure, and the recursion shape discharges it today; see
the RE2 probe update in `v1/ecs-refinement-typed-apis-plan.md`.)

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

### WF1 -- `#writes w` declarations (trusted tier) -- LANDED

Parallel to `#reads w`: a per-argument write frame on a defn, parsed and
carried on the binding. Same coarseness (whole argument, not per-field), same
trust boundary for inline-C bodies. No semantics yet beyond the declaration
existing -- WF1 is the vocabulary.

Three things the sketch got wrong, corrected as built:

- **The lifecycle home.** The `refined` experiment graduated 2026-08-01, so
  `#writes` got its own `write-frames` row rather than a retired one.
- **The spelling.** `#writes [a b]` accepts a vector, not just one symbol.
  A greedy symbol run cannot work -- the annotation is followed by the
  return-type `:`, which reads as a symbol -- and the bracket form is what
  gives `#writes []` a spelling at all. That empty frame turned out to be the
  most useful one: it is the strongest claim available and the one WF3 asks
  for most often.
- **Declared-vs-empty.** `writes_declared` is a separate bit from the mask.
  "Writes nothing" and "no frame" are different claims and a zero mask cannot
  carry both; conflating them upgrades every un-annotated function in the tree
  to the strongest frame there is, which is the stale-hypothesis failure mode
  wearing a different hat.

`#reads` and `#writes` are accepted in either order. A fixed order would turn
a stylistic choice into "unknown function or operator 'writes'" -- the
annotation would fall through and elaborate as a call.

### WF2 -- the checked tier for pure-Turmeric bodies -- LANDED

A body with no inline C is walkable: verify that every `set!` target, every
`^mut`/`^unique ^mut` argument pass, and every callee's own declared `#writes`
frame stays inside the declaring function's frame. Diagnostic on violation
(the `#reads` analog: a declared frame the body exceeds is an error, not a
silent widening). Inline-C bodies remain trusted-with-declaration -- the same
step `#reads` deliberately took -- so the tier split is
checked-when-checkable, never checked-by-pretending.

Two things the sketch left out, both discovered in the building:

- **There are three verdicts, not two.** "Honors it" and "exceeds it" are not
  exhaustive: an unresolvable callee, an unannotated non-`^borrow` slot, or a
  DECLARED-but-unchecked callee frame all mean "I could not check this". That
  is UNVERIFIED -- no diagnostic, `writes_checked` stays false. Making it an
  error instead would mean no function could carry a frame until its entire
  transitive callee set carried one, which is the difference between an
  adoptable feature and a dead one. It is also simply the honest answer: "I
  could not check this" is not "you did something wrong".
- **A bare-symbol assignment to a parameter is NOT a write.** `(set! p 5)`
  rebinds this function's own slot; turmeric passes by value, so no caller can
  observe it. This is the same argument the landed WF3 slice already rests on
  ("a plain symbol target cannot alias"), and the two must agree -- if a
  bare-symbol assignment could escape, WF3 would be unsound too. Checked
  against the reference case: `(set! p ...)` on a `&T`-typed parameter is a
  *type error*, not a write-through, so the rule holds for reference
  parameters as well.

  > **Correction 2026-08-02.** This bullet originally continued: "A PLACE
  > expression rooted at a parameter (`(set! (.n w) 9)`) does reach the
  > caller's object and does count." The second half is true; the
  > justification was overstated, and it is worth fixing rather than quietly
  > dropping, because it asserts something about the language that is false.
  >
  > A place write reaches the caller's object through an `rc<Struct>` or a
  > `:heap` receiver -- the case the rule exists for. Through a plain
  > **by-value** struct parameter it reaches a copy: the compiled backend
  > emits `(a).n = 3` into a discarded stack temporary, which `cc -O2` then
  > deletes entirely (verified from the emitted assembly). So counting it is
  > an OVER-approximation there, not an equivalence.
  >
  > Pessimism is the safe direction for a frame -- it over-declares, never
  > under-declares -- so the rule itself stands unchanged. But the by-value
  > case is a silent no-op compiled and a write-through interpreted, which is
  > a live divergence:
  > `docs/archive/struct-param-mutation-backend-divergence.md`. Resolving
  > that by rejecting the no-op write would narrow this branch to rc/heap
  > receivers and make it exact.

The check is DEFERRED (`wf_resolve_write_frames`, before the crossing
resolution that consumes it), because "every callee's declared frame stays
inside this one" is a question about functions that may be defined later in
the unit. It iterates to a fixed point: a caller's verdict depends on its
callees' `writes_checked`, so one linear sweep would under-verify a caller
purely because its callee had not been visited yet.

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

#### Widening 1: borrow-aware disjointness -- LANDED 2026-08-02

The borrow guard was the right widening to take first, and not for the reason
the list above implies. Of the three candidates, it is the only one where a
callee frame is the *load-bearing new ingredient*:

- **Transitive callee frames, on their own, buy nothing here.** A call in a
  caller body does not currently trigger a decline at all -- it is outside the
  analysis, because a hypothesis is only believed when it is congruent, and
  congruence needs either a pure measure (immune to calls) or a frozen region
  (where a mutator is TUR-E0200-unreachable). There was no decline to lift.
- **Field-granular frames do not need a callee frame** -- attributing
  `(set! (.n w) 9)` to its root `w` is a local rewrite. It carries a real
  aliasing hole (a place rooted at a borrow-bound local reaches the borrowed
  object), which is *itself* a borrow question, so it wants this widening
  underneath it rather than beside it. Deferred, with that ordering noted.
- **The borrow guard is exactly a callee question.** "A borrowed local is the
  one way a callee could write this frame's slot" is a statement about
  callees, and until WF2 there was no way to ask it, so assuming the worst was
  the only sound answer available.

What landed: `wf_call_slot_writes` / `wf_alias_write_free` /
`wf_borrow_write_free` in `elab_fns.c`, consulted by the borrow guard in
`rt_push_cs_path_conds`. Two borrow shapes are recognized -- passed straight
into a call slot, and `(let [b (& acc)] body)` where `b` aliases the borrow --
and everything else keeps the decline. Zero uses of the alias is the common
case and trivially safe: that is the `frozen` region idiom, whose `(& w)`
exists only to lock `w` down and is never passed anywhere.

"Cannot be written" has exactly three sources, and the third is the point of
the whole tier split:

1. a `^borrow` parameter -- a fact, the borrow checker enforces it;
2. a CHECKED `#writes` frame excluding the slot -- a fact, WF2 walked the body;
3. nothing else. An unresolvable callee, an unannotated non-borrow slot, and a
   DECLARED-but-UNCHECKED frame all answer "assume it writes". Acting on a
   *trusted* frame here would let a promise elide a check, which is precisely
   what the checked tier exists to prevent.

`errors/refine-wf3-borrowed-target` keeps pinning the un-widened behavior
unchanged -- it does not enable the experiment, so it still declines. The new
pair is `refine`-adjacent: `wf3-borrow-write-free` (the rescue) and
`errors/wf3-borrow-reaches-writer` (the borrow handed to a writer, which must
still decline). Both matter; the positive alone would pass just as well if the
guard were deleted outright, which is what the sabotage run is for.

### WF4 (RETIRED 2026-08-02) -- elision on checked frames

~~Once WF2's checked tier exists and has soaked: allow a CHECKED `#reads`
measure's kept entry check to be elided inside a region that freezes its
frame -- the resizable-bounds case's last step.~~

**The check does not exist.** See the status block at the top of this file for
the full finding. In short: `rt_inject_param_checks` has always skipped entry
checks for `#reads`-measure refinements (they are impure and would be
TUR-E0375), the emitted C for the motivating fixture confirms it, and the only
kept-and-elidable entry check belongs to a *pure* measure -- which no write
frame can help, and whose elision was measured and declined a week before this
plan was written.

The standing evidence-bar warning was right, and it was pointing at the wrong
thing. The bar this proposal actually failed was not "is the win big enough" but
"is there a check here at all".

## Validation

- WF2: fixtures for a frame the body honors, a frame the body exceeds (each
  write channel: direct `set!`, `^mut` pass, transitive callee frame), and an
  inline-C body staying trusted.

  > **Shipped 2026-08-02, with one addition the list missed.** All four exist
  > (`wf1-writes-frame-honored`, `errors/wf2-writes-exceeds-{set,mut-pass,
  > callee-frame}`, `wf1-writes-inline-c-trusted`), plus
  > `errors/wf1-writes-unknown-param` for TUR-E0381 and --
  > the addition -- `errors/wf2-writes-exceeds-in-macro`, a write hidden in a
  > macro template. The list above did not call for it, and it should have:
  > the frame walk has the same macro-expansion hop the WF3 scanners do, for
  > the same reason, and a hop that is not pinned by a fixture is a hop that
  > can be deleted without anything going red. The sabotage run confirms it is
  > load-bearing.
  >
  > `wf1-writes-inline-c-trusted` deliberately includes `lie!`, an inline-C
  > body whose declared frame is flatly false. It compiles, and that IS the
  > fixture's point: it pins the trust boundary rather than asserting the
  > frame is true. If inline-C bodies ever become walkable, `lie!` becomes
  > TUR-E0382 and the fixture moves to `errors/`.
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

  > **The fuzzer line above overpromised, and the gap is worth naming.** The
  > differential fuzzer does not generate `#writes` at all, so running it
  > (`--n 200 --mode both`, seeds 1 and 2, 0 soundness bugs) is a
  > NO-REGRESSION signal on the un-widened path -- not coverage of the new
  > one. The hand-written negatives carry the new path, and the sabotage run
  > is what shows they actually carry it: strip any of the five guards and its
  > negative wrongly passes. Teaching the generator to emit frames is the
  > honest next step for anyone widening further.

## Non-goals

- CSE / reordering / parallelization on frames (step 3; unplanned).
- Subsuming the ECS `ReadCap`/`WriteCap` machinery (works today, checked,
  linear; replacing it is only worth it after WF2 exists).
- Per-field or heap-location frames (Dafny-precision); per-argument
  granularity matches `#reads` and covers both demand signals.
- Any change to the trusted `#reads` semantics -- it stays step 1, unchanged.
- Entry-check elision, in any form -- WF4 is retired (the check does not
  exist) and whole-program elision was measured and declined.

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
