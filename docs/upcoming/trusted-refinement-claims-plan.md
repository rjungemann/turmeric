# Trusted refinement claims -- making the promises the solver believes checkable

> **Status:** Proposed 2026-08-17.  Drafted as the last step of the
> mutable-globals work, exactly where
> [`mutable-globals-plan.md`](mutable-globals-plan.md) section 14 said it
> should be: after G2 landed, from findings that came out of doing that work
> rather than from guessing at them.  **R1 (the warning) and R2 (the gated
> refusal, `--enable=checked-reads`) LANDED 2026-08-17** (see section 4).
> **R3 LANDED 2026-08-18** (commit 9376c6c3; see its section).  R4 belongs
> to the ECS/spice side.
> **Type:** Language / refinement checking
> **Depends on:** nothing.  Section 14 of the mutable-globals plan is explicit
> that the read side blocks nothing and must not become a precondition.
> **Related:** [`mutable-globals-plan.md`](mutable-globals-plan.md) sections
> 12-13 (the research this plan's section 1 is distilled from),
> [`docs/guides/stateful-refinements-guide.md`](../guides/stateful-refinements-guide.md)
> (the trusted tier's own documentation and its three-step trajectory),
> [`docs/archive/refine-stateful-measures-plan.md`](../archive/refine-stateful-measures-plan.md)
> (the design record for `frozen` + `#reads`).

## 0. Summary

The refinement system has exactly one **trusted** claim: `#reads <param>` on a
measure.  Everything else the solver believes is either proved (purity via
`rt_classify_expr`, write frames via WF2 under `--enable=write-frames`) or
backed by a runtime check.  `#reads` is neither: it is a promise, its one
consumer (`enc_reads_arg_frozen`) *grants congruence* on the strength of it,
and at the crossing it enables there is **no runtime fallback** -- the outcome
is a proof or a diagnostic, never a kept check.

This plan is about that tier: what it costs when the promise is broken, which
part of making it checkable is cheap, which part is expensive and whose the
expense actually is, and the order to do it in.  The subject is the *tier*,
not the annotation -- `#reads` is one instance, and the measure layer (the
ECS, spices, every inline-C accessor) is the actual blocker for the general
case.

### 0.1 What this is deliberately NOT

- **Not a prerequisite for anything.**  Established in the mutable-globals
  plan section 12.4 and inherited here as a design constraint: no phase of
  this plan may become a gate for other work.
- **Not a deprecation of the trusted tier.**  The trusted form is sound by
  its own contract (the callee's own entry check is never elided) and it is
  the only form that works for inline-C measures, which is essentially all of
  them today.  Checkability is added *around* it, never swapped in under it.
- **Not `#fx{}` work.**  The effect row tracks algebraic effects and infers
  nothing from reads; that is its documented meaning and it stays.

## 1. What exists today (verified 2026-08-17)

Distilled from the mutable-globals plan's research sections; the running
probes live there.

### 1.1 The annotation

`#reads` is narrower than `#writes` in every dimension:

| | `#reads` | `#writes` |
|---|---|---|
| Arity | exactly one parameter | a bitmask, `#writes [a b]`, and mutable globals |
| Non-parameter name | hard error | `TUR-E0381`, except a mutable global, which is allowed |
| Checked? | **no -- trusted** (but see 1.4) | WF2-checked behind `--enable=write-frames` |
| Consumers | congruence grant at frozen call sites | WF3 invalidation |

The single-parameter limit is a deliberate *minimal slice*, not an argued
permanent shape (mutable-globals plan 13.5): the design commit and the guide
both frame it as "the minimal, trusted, refinement-only slice, shaped so a
stronger version can grow from it without a rename or a semantics break".  A
measure over two frozen resources cannot be expressed today.

> **Update 2026-08-18:** the arity row above is stale as of R3 -- `#reads`
> now takes the bracket form too (`#reads [a b]`, a bitmask internally), and
> a measure over two frozen resources IS expressible.  The rest of the table
> still holds: `#reads` remains trusted, remains parameter-only (a
> non-parameter name is still a hard error), and its consumer is still the
> congruence grant.  See the R3 section.

### 1.2 What a broken promise costs

A `#reads w` measure that also reads mutable state outside `w` buys a proof it
has not earned.  Inside a frozen region the solver treats two calls as one
value and **elides the caller-side crossing check**; there is no runtime
fallback at that crossing (the no-region variant is `TUR-W0372`, "the crossing
must be proven").  The fixture pair
`refine-reads-frame-omits-global` / `errors/refine-reads-frame-omits-global-no-region`
pins both halves with a running program that silently crosses on a false
predicate.

Two mitigating facts, both load-bearing for prioritization:

- The design's stated backstop survives: the measure's own internal safety
  check (if the accessor has one) still runs.  A self-checking accessor turns
  a broken promise into a wrong-but-safe answer.
- The hole is pre-existing and not created by `^mut` globals -- an inline-C
  static counter breaks the promise identically.  What mutable globals changed
  is *reachability*: breaking it no longer requires inline C.

### 1.3 The two bills, kept separate

The single most useful finding (mutable-globals plan 12.3): "make `#reads`
checked" conflates two jobs with almost nothing in common.

- **The narrow bill (compiler, small):** notice when a body *demonstrably*
  reads mutable state outside the frame, and act on that positive evidence.
  A classifier over the elaborated body, default-deny, ~100 lines.
- **The general bill (ecosystem, large):** verify what a measure reads when
  its state is behind an inline-C handle.  The walk cannot see into C, so the
  real cost is rewriting the measure layer to hold state in Turmeric-visible
  structs -- a change to the ECS and spices, not to this compiler.  A checked
  `#reads` shipped today would verify approximately nothing that exists.

### 1.4 R1, already landed: the warning tier

`TUR-W0383` fires at the definition of a `#reads` measure whose body directly
reads a mutable global ("`#reads w` omits mutable state the body reads").
Gateless, evidence-based, changes nothing proved -- the override still grants
congruence.  An inline-C body yields no evidence and stays silent, which is
what made it zero-risk for the nine strict-refine fixtures that rely on the
override.  Implementation: `reads_scan_mut_global` in
`src/compiler/elab_fns.c`, direct reads only, unmodeled expression kinds not
descended.

This was the mutable-globals plan's 13.1 recommendation, shipped so that the
"you cannot even tell" problem is closed before any decision about refusing.

## 2. Phases

Ordered by confidence; each is independently landable and none blocks
anything outside this plan.

### R1 -- the warning (LANDED 2026-08-17)

See 1.4.  Remaining follow-through lives in R2's fixture note.

### R2 -- the gated refusal (LANDED 2026-08-17; see section 4)

Escalate positive evidence from "warn" to "refuse the congruence override":
when `reads_scan_mut_global` (or its R2 extension) finds an outside mutable
read, `enc_reads_arg_frozen`'s grant is declined, and the crossing becomes
the ordinary `TUR-W0372` a measure without the frame would get.

- **Gated** (`--enable=` experiment row, every field populated, per
  CLAUDE.md): it can only ever turn a currently-proving program into a
  diagnostic, which is exactly the shape that wants soak time.
- Mechanically: one flag on `RefineFnInfo` (which already carries
  `reads_param_plus1`), set where the binding metadata is stamped; a few
  lines gating the override in `refine_collect.c`.
- The walk should grow "could not see" as a distinct answer before it backs a
  refusal (the `WG_UNKNOWN` discipline the G1 write walk needed) -- refusal
  still keys on **saw a read**, never on "could not tell", or every inline-C
  measure dies.
- **Fixture obligation:** `refine-reads-frame-omits-global` is written to
  flip here -- its header says a landed refusal should make it stop proving.
  Update it deliberately in the same change.

### R3 -- `#reads [a b]`: the bracket form (LANDED 2026-08-18)

Lift the one-parameter limit to a vector, matching `#writes`.  Costs nothing
semantically (13.5); the congruence grant requires *every* named parameter
frozen at the site.

**Landed 2026-08-18** (commit 9376c6c3, "refine: let #reads name multiple
parameters"), implementing `docs/archive/reads-frame-cannot-name-multiple-
params.md` -- the demand signal arrived as a filed expressiveness hole
rather than a shipped measure, and the fix followed the same day.  What
shipped matches the sketch above exactly:

- **Representation:** `Binding.reads_param_plus1` /
  `RefineFnInfo.reads_param_plus1` (a single 1-based index) became
  `reads_params_mask`, a `uint64_t` bitmask over parameter indices.  Params
  past bit 63 are rejected (that arity already trips TUR-W0041).
- **Reader:** `read_reads_annot` mirrors `read_writes_annot` -- `#reads w`
  or `#reads [w g ...]`; the single-symbol form is unchanged.  TUR-E0024
  covers the malformed shapes (two frames, a repeated name, a non-parameter
  name, an empty frame -- `#reads []` stays illegal because, unlike
  `#writes []`, "reads no mutable state" is spelled by omitting the frame).
- **The grant is CONJUNCTIVE:** `enc_reads_arg_frozen` became
  `enc_reads_args_frozen` and requires every named parameter frozen at the
  site.  One unfrozen named parameter is enough for two occurrences of the
  measure to denote different values -- precisely the crossing the grant
  elides -- so "any frozen" would have been silently unsound.  Verified in
  all four quadrants: both-frozen proves (`refine-reads-multi-param-frozen`),
  and w-only / g-only / neither each withhold the grant
  (`errors/refine-reads-multi-param-partial-frozen`).
- R2's evidence walk and refusal apply per-frame unchanged, as this section
  predicted -- the evidence is about mutable globals, which no frame can
  name, so the mask's width never enters it.

(A 2026-08-19 note briefly marked this phase "TO DO" -- it was written
against this file's stale status block, one day after the code had already
landed.  Corrected same day.)

### R4 -- the general checked `#reads`

Blocked on a Turmeric-visible measure layer, and that decision belongs to the
ECS/spice side, not this repo.  Recorded so the dependency direction is
explicit: if the ECS ever holds its state in structs the purity walk can see,
step 2 of the guide's trajectory ("checkable later") becomes real for reads,
and the CSE / safe-parallelization / incremental-recompute consumers the
guide lists as blocked on checking become reachable.  Until then this phase
is a pointer, not work.

The pointer is bidirectional:
[`ecs-refinement-typed-apis-plan.md`](v1/ecs-refinement-typed-apis-plan.md)
carries a trigger note under its C2 prerequisite naming the exact decision
that fires this phase -- its C1 purity caveat's option "(b) making the RE0
unwrappers pure primitives" is the same rewrite viewed from the other side,
so whoever picks that up finds this plan waiting and lands the two as one
design.

#### R4 investigation record (2026-08-19, against turmeric-spices head)

Read the actual measure layer to size the trigger.  Four findings, one of
which narrows the phase considerably.

1. **The premise is confirmed, and the blast radius is smaller than "the
   measure layer".**  Every `#reads` measure in the spice bottoms out in
   inline C over a calloc'd `int64_t` block: `ecs/refined-world`'s
   `rgworld-alive?` reads through `rgw-gen` (inline C over the 129-slot
   control block), and `ecs/sized-world`'s `sized-alive?` is itself inline C
   over the `WorldState` handle (`(defopaque WorldState :int)`;
   `(defopaque SizedDense [n A] :int)` is the same shape).  But `#reads`
   appears in only two src modules and five tests, and the measures involved
   are exactly the two aliveness predicates.  A frame claim is about what
   the ANSWER depends on, so only the measure bodies need Turmeric-visible
   reads -- spawn/despawn/get/set can stay inline C.  "Rewrite the measure
   layer" is really "rewrite two measure bodies plus the entity unwrapper
   family they lean on".

2. **Struct-ification alone would NOT light up the purity walk -- and that
   is fine, because R4 does not need purity.**  `rt_classify_expr` declines
   `EX_GET_FIELD` behind any reference type (TY_REF / TY_RC / TY_PTR_VOID /
   TY_REF_IMMUT / TY_REF_MUT -> UNKNOWN; the aliasing rationale in
   elab_fns.c).  ECS measures take `^borrow w` by design -- the borrow is
   what the frozen region freezes -- so even a fully struct-ified world
   reads UNKNOWN under the purity walk.  The C1 option-(b) framing
   ("make the unwrappers pure") is therefore not reachable by data-layout
   change alone.  What R4 wants is a different question: FOOTPRINT
   ATTRIBUTION ("every read in this body roots in a named frame parameter"),
   which may legitimately accept borrowed-receiver field reads precisely
   because the frozen region, not the classifier, excludes aliased writers
   at the crossing.  `reads_scan_mut_global` is the right skeleton for that
   walk -- it already models VAR / LET / IF / DO / WHILE / SET / MATCH /
   GET_FIELD / BUILTIN / CALL / ASCRIBE / CAST / RETURN with the
   positive-evidence discipline -- extended with the WG_UNKNOWN tri-state
   for calls it cannot follow.

3. **Two concrete compiler gaps stand between here and a walkable measure
   body, both small:**
   - `EX_CAST` / `EX_ASCRIBE` are unmodeled in `rt_classify_expr` (they fall
     to the UNKNOWN default), so even a trivially pure body that touches a
     `defopaque` newtype via `::` is UNKNOWN.  Modeling the coercing cast as
     exactly-as-pure-as-its-operand would let the RE0 unwrappers
     (`slot->int`, `generation->int`, ... -- inline-C identity casts today)
     be DELETED in favour of `::` rather than blessed as primitives.
   - A Turmeric-visible read primitive for the existing calloc'd block
     already exists: `array-get-unchecked` (BS_ARRAY_GET_UNCHECKED, over
     `:ptr<void>`), currently UNKNOWN to the purity walk.  A measure written
     as `(array-get-unchecked (gens-ptr w) slot)` puts the read IN THE TREE
     where a footprint walk can see and attribute it -- no data-layout
     rewrite required, though a typed `:ptr<T>` (or a real struct field)
     attributes more soundly than a void pointer.
   So the cheapest R4-enabling shape is NOT "gens becomes a struct field or
   vec" (the trigger note's sketch) but "the two aliveness bodies read via
   typed pointer builtins instead of inline C, and `::` becomes a modeled
   form".

4. **Ordering.**  R3 (landed 2026-08-18) already supplies the first step
   from this direction too: a visible-body aliveness measure over
   world-plus-entity is exactly the two-frozen-resource shape the bracket
   form serves.  Next the compiler side decides the footprint walk's vocabulary (including the
   cast-modeling above) BEFORE any spice rewrite, and the spice side
   converts only the two measure bodies.  Incidental: the spice's
   `refined-world.tur` header still says `:sealed` is "only ENFORCED under
   `--enable=sealed-opaque`" -- stale since sealed-opaque graduated
   2026-08-17; fix on the spice side when the measure bodies are touched.

#### R4 execution record, part 1 (2026-08-19): the compiler enabler landed, the rewrite validated

The 2026-08-19 investigation's item-3 gaps are now closed or de-risked:

- **`EX_CAST` / `EX_ASCRIBE` are modeled in the purity walk** (landed;
  `rt_classify_expr`, elab_fns.c): each classifies exactly as pure as its
  operand -- an ascription is erased at codegen and a numeric cast reads
  nothing but its operand, so UNKNOWN -> operand's answer only removes
  diagnostics and grows congruence (same argument as the EX_MATCH
  widening).  Consequence, verified by fixture
  `refine-ascribe-pure-measure`: a measure whose only "impurity" was
  newtype traffic through `::` is PURE and discharges a refined crossing
  from an if-guard with no `#reads` frame and no frozen region.  Before
  the widening the same file was a TUR-W0372 under `--strict-refine`.
  This deletes the C1 purity caveat's premise: RE0-style unwrappers no
  longer need to be inline C OR blessed primitives -- they are ordinary
  pure defns spelled with `::`.

- **The R4 target shape works end-to-end today**, pinned by two fixtures:
  `refine-reads-visible-body-measure` (an aliveness measure whose
  generation-table read is `array-get-unchecked` on a struct field -- no
  inline C anywhere in the read path -- still earns the `#reads` grant
  inside a frozen region and proves under `--strict-refine`) and
  `errors/refine-reads-visible-body-unfrozen` (the same measure outside
  the region is refused; this also guards against a future purity
  widening ever classifying a raw-memory read as pure, which would let
  the crossing prove without the region -- unsound).

- **The spice-side rewrite was prototyped in a local turmeric-spices
  checkout and is a no-op behaviorally.**  All eight `ecs/entity` bodies
  (`slot->int`, `generation->int`, `slot-new`, `generation-new`,
  `entity-new`, `entity-index`, `entity-generation`, `entity=?`) rewrote
  to pure Turmeric (`::` plus the bit-op builtins, which are BS_BIN_INFIX
  and hence walk-PURE), and `refined-world`'s `rgw-ctrl` / `rgw-gen`
  rewrote to `(:: w :int)` and an `array-get-unchecked` load.  The
  spice's full test set -- 87 per-file runs including the expected-error
  negatives, honoring each file's `tur-test-flags` -- diffed
  byte-identical against the pre-rewrite baseline (modulo the TUR-W0033
  noise below).  The conversion is mechanical; the sized-world stack
  (`sized-alive?`, whose inline C also bounds-checks) is the remaining,
  slightly larger half.

- **Papercut filed:** every `unsafe` block the visible shape needs draws
  a contradictory TUR-W0033 ("handler clause unreachable") because the
  raw builtins require the block syntactically but never perform the
  `Unsafe` effect.  See
  `docs/reported/unsafe-block-w0033-on-raw-builtins.md`; worth fixing
  before the spice rewrite lands or the ECS build output gets noisy.

#### R4 execution record, part 2 (2026-08-19): slice 1 of the footprint walk -- omitted-PARAMETER evidence

The evidence tier now covers the second way a frame breaks.  R1/R2 saw one
kind of positive evidence (a direct mutable-global read); slice 1 adds the
other: **a demonstrable read of mutable state rooted in a parameter the
frame omits**.  `reads_scan_unframed_param` (elab_fns.c, beside the global
scan it mirrors) walks the body for the two read shapes the trusted tier
exists for -- a raw-memory load (`ptr-deref` / `array-get-unchecked`) and a
field read through a reference-typed receiver -- and chases each read's
pointer root through the value-shaping forms only (VAR, `::`, cast, field
hops, ptr arithmetic).  A root that is a parameter Binding (pointer
identity against the params array, so shadowing cannot mis-attribute)
whose mask bit is clear is the finding.  A root behind a CALL yields no
evidence: interprocedural attribution stays the footprint walk proper's
job, where "could not see" must first become a distinct verdict.

Notes from doing it:

- **Both evidence scans now descend `(unsafe ...)`** (EX_HANDLE, body and
  case bodies).  The raw-load builtins *require* the block, so a walk that
  stopped at it was blind to exactly the reads this tier is about -- this
  also quietly strengthens R1/R2 (a mutable-global read inside an unsafe
  block now warns too).
- **A `^borrow` struct parameter's field read is NOT evidence, and that is
  correct, not a gap.**  Probed before assuming: such a read discharges a
  refined crossing with no frame at all, because the receiver is
  struct-typed (the purity walk's reference decline list matches reference
  VALUES, not borrow-annotated params) and mutation between guard and use
  is the hypothesis-invalidation machinery's job.  Param state the solver
  already tracks is outside the trust tier; warning on it would be a false
  positive.
- The evidence flag was renamed (`Binding.reads_omits_mut_global` ->
  `reads_frame_omits_state`) since it now carries either kind; the R2
  refusal consumes it unchanged, so `--enable=checked-reads` refuses the
  omitted-parameter case with the same fix-the-frame W0372 wording.
  TUR-W0383 gets a second message variant naming the omitted parameter and
  the exact fix (`#reads [w g]`); the --explain text and
  stateful-refinements-guide.md cover both kinds.
- **Fixture triple**, mirroring the R2 set: `refine-reads-frame-omits-param`
  (warn + still proves, gate off), `errors/r4-checked-reads-refuses-param-read`
  (gate on -> refused, TUR-W0372 "grant was refused"), and
  `refine-reads-multi-param-visible-quiet` (frame widened to `#reads [w g]`
  per the warning's advice: silent and proving even under the gate --
  structurally pinned, since residual evidence would refuse the grant and
  fail the run).  The quiet fixture is also the first visible-body
  multi-param `#reads` measure in the tree, closing the loop with R3.

**What remains for R4 proper:** the footprint walk itself (attribute
every read in a walkable measure body to a named frame parameter, with
the WG_UNKNOWN discipline for calls it cannot follow -- slice 1 above is
its evidence-only precursor: same leaves, same roots, no verdict on
silence), the decision of what a VERIFIED frame buys its
consumers, the sized-world half of the spice conversion, and a guides
pass once the walk lands -- `stateful-refinements-guide.md` (the trusted
tier's own documentation and its three-step trajectory),
`refinement-types-guide.md`, and the TUR-W0383 `--explain` text all
describe the reads story as it stands today and need to say what checked
frames actually verify.  The facade half of the spice-side prototype is
up as turmeric-spices PR #54 (2026-08-19), verified byte-identical on
both the new and the old purity walk, so it carries no new `tur` floor.
The walk and the sized-world conversion stay one design, landed
together, per the trigger note in `ecs-refinement-typed-apis-plan.md`.

## 3. Explicitly not doing

- **Making `--strict-refine` refuse trust-based proofs.**  Evaluated and
  rejected (mutable-globals plan 13.1): nine fixtures -- including the
  flagship ECS foreach -- combine `#reads` with `--strict-refine` *because*
  the override decides their crossings.  Strict means "every crossing must be
  decided", and the override is what decides them.
- **A per-heap-location `reads` clause** (Dafny-style footprints).  The
  coarse per-argument shape is the language's chosen slice; refining it below
  argument granularity is research, not a phase.

## 4. R2 execution record (2026-08-17)

**Landed**, behind `--enable=checked-reads`.  `tests/run.sh` -> 2618 passed,
0 failed; `tests/run-turi.sh` green.  No fixture snapshot moved and the nine
strict-refine `#reads` fixtures are byte-identical under the gate (checked by
running two of them with `--enable=checked-reads` added; the fixture below
pins the property structurally).

### 4.1 Shape

Exactly the sketch above, plus one addition it did not anticipate:

- `Binding.reads_omits_mut_global` is stamped where the frame is stamped, on
  **every** elaboration of the defn (clones included) -- the encoder's
  refusal must see the evidence whichever binding a call resolves to.  Only
  the W0383 *warning* is deduped by the bare_fat guard.  The flag mirrors
  onto `RefineFnInfo` in `rt_resolve_fn`.
- The refusal itself is three lines in `refine_collect.c`:
  `!(g_opt_checked_reads && info.reads_omits_mut_global)` conjoined into the
  existing grant condition.  Refusal keys on positive evidence only, so the
  "could not see" discipline the sketch worried about needs no new
  tri-state yet: the walk never reports "saw" for an unwalkable body, and
  nothing consults its silence as a verdict.
- `experiment_warn_if_used("checked-reads")` fires where the refusal becomes
  live -- evidence found AND gate on -- so enabling the gate over a clean
  program stays quiet (it changed nothing).
- **The addition: honest W0372 wording at a refused crossing.**  The
  standard impure-`#reads` message says "guard it inside a `frozen` region",
  which is misleading advice when the region is present and the *frame* is
  what failed.  A `reads_grant_refused` flag on the obligation (computed by
  a narrowed sibling of `rt_pred_reads_measure`) branches the message to
  "its congruence grant was refused (--enable=checked-reads): the frame
  omits mutable state the body reads (TUR-W0383) -- fix the frame, not the
  region".  Both W0372 emission sites branch.

### 4.2 Fixtures

- `errors/r2-checked-reads-refuses-broken-frame` -- the
  `refine-reads-frame-omits-global` program under
  `--enable=checked-reads --strict-refine`: the crossing that proves by
  default is an undischarged hard error, with the refusal wording asserted.
- `r2-checked-reads-inline-c-still-trusted` -- the
  `refine-stateful-guard-discharges` program (inline-C measure, frozen
  region, strict) with the gate ADDED: still proves, still prints 42.  Pins
  no-refusal-without-evidence; its header names the failure mode it guards
  against (refusal acting on "could not see").
- `refine-reads-frame-omits-global` deliberately stays gate-off and pins the
  default (trusted grant + W0383 + prints 7); its header now points at the
  errors sibling and says the two fold together if `checked-reads`
  graduates.

### 4.3 What R3/R4 inherit

Unchanged mechanically; R3 landed 2026-08-18 (see its section).  R4 still
belongs to the measure layer.  If `checked-reads` graduates, the
default flips from "trusted grant + warning" to "refusal on evidence" -- the
sweep at that point is the fixture fold-together above plus retiring the
gate language in stateful-refinements-guide.md and the TUR-W0383 --explain
text.
