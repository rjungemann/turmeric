# Trusted refinement claims -- making the promises the solver believes checkable

> **Status:** Proposed 2026-08-17.  Drafted as the last step of the
> mutable-globals work, exactly where
> [`mutable-globals-plan.md`](mutable-globals-plan.md) section 14 said it
> should be: after G2 landed, from findings that came out of doing that work
> rather than from guessing at them.  **R1 (the warning) and R2 (the gated
> refusal, `--enable=checked-reads`) LANDED 2026-08-17** (see section 4).
> R3 waits on a real two-resource measure; R4 belongs to the ECS/spice side.
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
| Arity | exactly one parameter | a bitmask, `#writes [a b]`, globals under `--enable=global-state` |
| Non-parameter name | hard error | `TUR-E0381` / global allowed behind gate |
| Checked? | **no -- trusted** (but see 1.4) | WF2-checked behind `--enable=write-frames` |
| Consumers | congruence grant at frozen call sites | WF3 invalidation |

The single-parameter limit is a deliberate *minimal slice*, not an argued
permanent shape (mutable-globals plan 13.5): the design commit and the guide
both frame it as "the minimal, trusted, refinement-only slice, shaped so a
stronger version can grow from it without a rename or a semantics break".  A
measure over two frozen resources cannot be expressed today.

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

### R3 -- `#reads [a b]`: the bracket form

Lift the one-parameter limit to a vector, matching `#writes`.  Costs nothing
semantically (13.5); the congruence grant requires *every* named parameter
frozen at the site.  Worth doing whenever a real measure over two frozen
resources shows up; not worth doing speculatively before then.

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

Unchanged.  R3 still waits on a real measure over two frozen resources; R4
still belongs to the measure layer.  If `checked-reads` graduates, the
default flips from "trusted grant + warning" to "refusal on evidence" -- the
sweep at that point is the fixture fold-together above plus retiring the
gate language in stateful-refinements-guide.md and the TUR-W0383 --explain
text.
