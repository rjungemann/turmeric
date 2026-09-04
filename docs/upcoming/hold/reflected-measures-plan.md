# Plan: Reflected Measures (`^reflect`)

> **Status:** Elaborated 2026-08-17 -- phased and sized against the real
> elaborator, still **on hold** pending the trigger below. Not started.
> **Last Updated:** 2026-08-17
> **Type:** Compiler / Refinement types
> **Depends on:** [refinement-types-plan.md](../../archive/refinement-types-plan.md)
> (RT0--RT7 + S0--S4, all landed), the `refined` graduation (**happened
> 2026-08-01**, shipped v0.33.0), and
> [refine-predicate-measures-plan.md](../../archive/refine-predicate-measures-plan.md)
> (RM-B, landed).

## Goal

Let a recursive measure *mean* something inside a refinement predicate, instead
of being an opaque symbol the solver can only compare to itself.

Today every named measure is an uninterpreted function. That is a deliberate
language rule, stated at `src/compiler/refine_collect.c` (the `enc_measure`
block):

> An unrecognised head becomes a named measure -- an uninterpreted function
> symbol reasoned about by congruence closure, never unfolded. This is the
> language rule that keeps S1 tractable.

Congruence alone carries a surprising amount: `(= (len xs) 3)` as a hypothesis
does discharge `(> (len xs) 0)`, because EUF hands the term to LIA. What it
cannot do is connect a measure to the *structure* of its argument:

```turmeric
(defn len [xs : List] : int
  (match xs
    [(Nil)      0]
    [(Cons _ t) (+ 1 (len t))]))

(defn head-of [xs : #refine{ v : List | (> (len v) 0) }] : int ...)

(head-of (Cons 1 (Nil)))
```

The obligation is `(> (len (Cons 1 (Nil))) 0)`. `len` is opaque, the argument is
a constructor term, and nothing relates them -- so this falls to `TUR-W0372`
and keeps its runtime check. Two unfolding steps would settle it:
`len(Cons 1 Nil) = 1 + len(Nil)`, then `len(Nil) = 0`, then `1 > 0`.

`(sorted? xs)`, `(in-bounds? v i)`, `(elems s)` all fail the same way, and they
are the predicates users actually want to write.

---

## What this is NOT -- the carve-out, now stated as a decision to be taken

Two documents in this tree list termination checking as an explicit non-goal,
and **this plan does not reopen either of them**:

- [refinement-types-plan.md](../../archive/refinement-types-plan.md), under
  "Non-goals for this prototype": *"Termination checking or total-correctness
  verification."*
- [loop-invariants-plan.md](loop-invariants-plan.md), "Explicitly not in
  scope": *"A refinement says nothing about whether the loop finishes... A
  non-terminating loop with a true invariant is perfectly well-typed. Ranking
  functions / decreasing measures. Same reason."*

Both are about **user program** termination -- proving that a `while` loop or an
arbitrary `defn` halts, i.e. total correctness. That remains out of scope,
permanently, and it is the right call for a systems language: 223 of the
1448 `defn`s in `stdlib/` are self-recursive, and the concentration is
`re.tur` (27), `logic.tur` (11), `httpd.tur` (8), `parsec.tur` (6),
`reactor.tur` (6) -- backtracking, a logic engine, a server, an event loop.
A `reactor` loop that provably terminates is a bug report. Nothing here should
ever make a non-terminating program fail to compile.

This plan is about a much smaller obligation: **a function the user has opted
in to reflecting, and only such a function, must be shown total before its
defining equation is admitted as an axiom.** No annotation, no obligation.
Every other function in the language is untouched.

The two are judged on different merits. "Should Turmeric prove programs halt?"
has been answered no, twice. "May a measure's definition enter the logic, and
what does that require?" is the question this plan poses -- and **RF0 below
makes taking that decision, in writing, the first deliverable of the work**,
before any checking code is cut. Liquid Haskell's `{-@ reflect @-}` is the
direct precedent and the same trade-off; naming the feature after what it
delivers (a *definable* measure) rather than after the checker keeps it from
being evaluated as the feature that was already declined.

---

## Why the totality gate is load-bearing (unchanged, and worth repeating)

A ground unfolding of `f(x) = 1 + f(x)` at any term `t` asserts
`f(t) = 1 + f(t)`, which LIA reduces to `0 = 1`. An inconsistent hypothesis set
discharges *every* obligation in the file, including false ones. This is not a
completeness hole that degrades to a runtime check -- it is silent, total
unsoundness, and it is the reason the criterion is a gate rather than a
nicety. A `^reflect` that fails its check is a hard diagnostic on the
definition, never a silent downgrade -- an annotation that quietly does
nothing is how a reader ends up believing a predicate is enforced when it is
not (the reasoning that produced `TUR-W0380`).

**No program that compiles today changes behavior.** Reflection is opt-in
syntax behind an experiment gate; unproven obligations keep their runtime
checks exactly as now. That property is the whole shippability argument,
inherited from the parent plan's runtime fallback.

---

## What elaboration settled

### 1. Coverage is mostly already enforced -- the sizing fear was wrong

The previous version flagged "does the elaborator check `match` exhaustiveness
today?" as the question to settle first, suspecting the coverage half might be
the larger half of the work. Settled: **it does, and coverage is the smaller
half.** All `match` elaboration lives in `elab_match`
(`src/compiler/elab_structs.c:2925`), with three paths:

- **Union scrutinees** (`:3023`): exhaustiveness is a hard error,
  `TUR-E0301`, at `:3143-3155`.
- **ADT/GADT scrutinees** (`:3460`): a `covered[]` bitmap per constructor,
  checked at `:4085-4149`; a plain-ADT miss is a hard (uncoded) error. Two
  details matter here: a **guarded arm does not count toward coverage**
  (`covered[ctor->tag] = !arm_has_guard[ai]`, `:3660`), so compiling code
  with guards necessarily carries a wildcard; and the
  **`#{NonExhaustive}` marker** (`:2934-2971`) suppresses the check.
- **Literal/primitive scrutinees** (`:3252-3379`, "S4-lit"): **no coverage
  check exists at all.** `(match n 1 "a" 2 "b")` over `int` elaborates
  cleanly with no default arm.

So for an ADT-scrutinee measure, *compiling* already implies covered, and the
coverage obligation reduces to three rejections RF2 must add: a body
containing `#{NonExhaustive}`, a literal-scrutinee `match` with no
wildcard/variable arm, and (belt-and-braces) any body form the reflection
walk does not recognize. Codegen already relies on the elaborator's guarantee
(`emit_expr.c:10902`, `emit_cps_ir.c:6237` both emit unreachable defaults),
which is independent confirmation the guarantee is real.

### 2. The body must be carried in a side table -- `defn_form` is not retained

Unfolding needs the measure's body *as a `Form`* (the refinement encoder
`enc` consumes `Form`s, not `Expr`s), and **`Binding::defn_form` is only
retained on two narrow paths** (`^fat` lazy bodies and `TY_CONT` resume
specialization -- `elab_fns.c:7176`, `:7565`). The precedent is exactly WF2's
`WriteFrameSite` (`src/compiler/elab_internal.h:1000-1012`): a side table
registered during elaboration (`wf_note_frame_site`, no-op unless the gate is
on) carrying `fn`, `params`, `defn_form`, `body_start`, and the annotation
form for the diagnostic span, resolved by a **deferred pass after
elaboration** (`wf_resolve_write_frames`, `elab_fns.c:1665`) so the verdict
does not depend on definition order. RF0 registers a `ReflectSite` of the
same shape; RF1/RF2 are its deferred resolver.

### 3. Totality is a new walk, not a fourth lattice value -- confirmed, with a sharper reason

The previous version said purity and totality are independent axes and
collapsing them would be a mistake. Elaboration found the crisper argument:
they have **opposite fixpoint polarities**. The purity walk
(`rt_classify_binding`, `elab_fns.c:348-380`) takes the *greatest* fixpoint --
its recursion case assumes pure, sound because impurity only enters through
concrete leaves, with the `min_open` guard refusing to memoize provisional
results. Totality must take the *least* fixpoint: a recursive call is
non-total until a decreasing argument is exhibited. So the checker is a
separate walk (a new `reflect_total` field on `Binding` beside the
`refine_purity` memo byte at `expr.h:474`, following the documented
zero-means-uncomputed discipline), not a reuse of `rt_p_join`. It *does*
reuse `rt_binding_is_pure` as a prerequisite check -- reflecting an impure
function is meaningless (its occurrences are fresh symbols anyway) and is
rejected outright.

There is still **no shared call-graph structure** in `src/` (re-verified:
`FnIndex` is file-local to `effect_check.c`; the purity walk and WF2 each
build their own traversal). RF1 follows the WF2 pattern -- its own bounded
deferred pass -- rather than building a shared graph; the `effect_check.c`
re-sweep fixpoint (`:1293-1330`) is the template on the shelf if mutual
recursion is ever admitted.

### 4. The encoding: reduction at constructor-headed arguments, not quantified axioms

The supported fragment is quantifier-free, so the defining equation cannot be
asserted as `forall x. len(x) = ...` -- that is the solver cliff the design
stays on the cheap side of. The previous sketch said "ground instances to
depth `k`". Elaboration sharpened *how* a ground instance is built, because
the VC language has no `match` construct to encode a body into:

**Unfold by syntactic reduction.** For an application term `f(t)` in the VC
where `f` is reflected-and-total and `t`'s form is **constructor-headed or a
literal**, substitute `t` for the parameter in the body form, *select the
match arm syntactically* (the scrutinee's head constructor is known), and
encode the surviving arm's expression; assert `f(t) = <reduced>`. New
application terms introduced by the reduction (`len(Nil)` inside
`1 + len(Nil)`) are candidates for the next round, bounded by fuel `k`. This

- keeps the logic quantifier-free with EUF and LIA untouched;
- needs no encoding of `match` at all -- the match is *gone* by the time
  anything is encoded;
- makes each asserted instance a definitional equality (Dafny-fuel
  discipline: the axiom is real, the instantiation is bounded, running out
  of budget costs completeness, never soundness);
- naturally declines the non-ground case (`f(xs)` for a variable `xs` has no
  arm to select), which is deferred to RF4 rather than half-built.

The machinery it extends already exists: `enc_measure`'s return-refinement
propagation block (`refine_collect.c:499-541`) already substitutes callee
names for actual argument terms, guards self-application cycles with the
`propagating[]` name stack, and asserts facts *about the application term* so
they are sound under negation. Unfolding is a sibling block with its own fuel
counter (distinct from `ENC_MAX_PROPAGATE`, which stays as-is for
non-reflected propagation). Sorts ride RM-B1's `ret_sort` resolution
unchanged.

### 5. Diagnostic codes: the ones this plan reserved are gone

Write-frames took `TUR-E0381`/`TUR-E0382`. The sibling
[loop-invariants-plan.md](loop-invariants-plan.md) needs no codes (its
obligations report through `TUR-E0371`/`TUR-W0372` like `:pre`/`:post` do),
so this plan takes the next free slots: **`TUR-E0383`** (a `^reflect` whose
function fails the totality gate -- purity, termination, or coverage; the
message names which) and
**`TUR-W0384`** (unfolding fuel exhausted; the obligation stays Unknown and
"fails toward a runtime check", the `TUR-W0372` principle). Registration is
the four-place ritual: the `diag.h` enum, the two string mappings at
`diag.c:275`/`:433`, and a `tur explain` entry. Codes are allocated for real
at land time -- re-check the band then.

---

## Phases

### RF0 -- The decision, the gate, the syntax

**Goal:** the carve-out is decided in writing, and `^reflect` parses to an
inert, registered site.

- **Take the decision.** Landing RF0 requires an explicit sign-off on the
  carve-out framed above ("a reflected function must be shown total; program
  termination stays out of scope") -- recorded in this file's header, not
  discovered in review.
- `EXPERIMENTS[]` row `reflected-measures` (all seven fields are mandatory
  per `experiments.h:24`; model on the live `write-frames` row at
  `experiments.c:215-222`), `g_opt_reflected_measures` in `globals.{h,c}`,
  `plan_path` pointing at this file, `introduced`/`expires_at` set when work
  starts (current convention: expiry four minors out).
- Syntax: `^reflect` needs **zero reader work** (`^` is a symbol character,
  `reader.c:164`); one `intern_cstr` in the caret table
  (`elab_core.c:1803-1822`) and a pre-name attribute block in `elab_defn`
  following the `^deprecated` template (`elab_fns.c:4194-4224`, the running
  `name_idx` cursor composes with `^private`). Placement is settled:
  **on the `defn`**, matching `#reads`/`#writes` -- the annotation hangs the
  obligation on the function, which is where the diagnostic must land.
- A `bool is_reflected` on `Binding` (near the `refine_*` cluster,
  `expr.h:344-476`), and a `ReflectSite` side table on `Elab` modeled
  field-for-field on `WriteFrameSite`, registered during `elab_defn` when
  the gate is on (`experiment_warn_if_used("reflected-measures")` fires
  here). No checking, no encoding.

**Acceptance:** `tur experiments` lists the row; a `^reflect` defn parses and
compiles identically to today with the gate on and off; macro expansion
preserves the attribute (fixture, since `#reads` needed explicit provenance
plumbing in `elab_macros.c` -- verify rather than assume).

### RF1 -- Totality: purity + termination

**Goal:** the deferred pass classifies each `ReflectSite` sound-or-rejected;
rejection is `TUR-E0383`.

- Deferred resolver `rf_resolve_reflect_sites` after elaboration (the
  `wf_resolve_write_frames` shape, so definition order never changes the
  verdict).
- Gate 1: purity, by `rt_binding_is_pure`. Impure -> `TUR-E0383` naming the
  reason.
- Gate 2: structural termination over the body **Form**: every self-call
  passes, in some fixed argument position, a strict syntactic subterm of the
  corresponding parameter -- where "subterm" means a pattern variable bound
  by a `match` on that parameter (the `t` in `[(Cons _ t) ... (len t)]`), or
  a subterm of such. Anything else -- arithmetic on the argument
  (`(f (- n 1))` included, deliberately: structural only in the first cut),
  calls through variables, argument shuffling across positions -- rejects.
  Self-recursion only; **any mutual recursion rejects** (see settled
  question 3; the fixpoint template exists if that proves too narrow).
  Non-recursive bodies pass trivially.
- Verdict stamped as `reflect_total` on the `Binding`; the site keeps its
  `defn_form` for RF3.

**Acceptance:** `len` passes; `f(x) = 1 + f(x)` rejects; `(f (- n 1))`
rejects; a mutual pair rejects; each with `expected.diag` naming the failing
gate. A sabotage build (termination gate stubbed to true) must make the
inconsistency fixture of RF3 wrongly prove -- written now, armed then.

### RF2 -- Totality: coverage

**Goal:** the equation is only admitted where a value actually exists.

Small, per settled question 1. The resolver additionally rejects a
`^reflect` body that contains:

- a `#{NonExhaustive}` match (the opt-out is an unchecked promise; a
  reflected axiom cannot rest on it);
- a literal/primitive-scrutinee `match` with no wildcard or variable arm
  (the S4-lit path has no coverage check to lean on);
- any form the reflection walk does not positively recognize (default-deny,
  the same posture as the purity whitelist -- a missed case costs one
  declined reflection, never a wrong axiom).

ADT/union matches need nothing: compiling implies covered (hard errors at
`elab_structs.c:3143` and `:4085-4149`), and guarded arms already force a
wildcard. `panic` in a recognized position is treated as not-covered
(reject): honest, and cheap to relax later if a use case appears.

**Acceptance:** a `#{NonExhaustive}` measure rejects with `TUR-E0383`; an
int-literal match without default rejects; the same with a trailing `_` arm
passes.

### RF3 -- Bounded ground unfolding in the encoder

**Goal:** the motivating example proves. The core phase.

- Publish reflection through the resolver seam: `RefineFnInfo` gains
  `is_reflected` plus access to the site's body form and parameter names
  (via `rt_resolve_fn`, exactly as `reads_params_mask` travels today,
  `elab_fns.c:653-655`).
- In `enc_measure`, alongside the propagation block: for a
  reflected-and-total callee whose encoded argument forms are
  constructor-headed or literal, reduce (substitute, select arm, recurse on
  fuel) and assert the definitional equality. Fuel: a per-obligation counter,
  default proposed 8, `TUR_REFLECT_FUEL` env override for experiments --
  **measure before fixing the default** (too small looks broken, too large
  is a compile-time surprise; the RT7 memo bounds re-proving either way).
  Reduction is deterministic, so the depth choice can shift completeness but
  never an answer's soundness, and memoized results stay stable.
- Fuel exhaustion: `TUR-W0384` (warning; check kept), surfaced through the
  RM-B3 `out_reason` channel under `TUR_REFINE_STATS=1` plus its own
  `RefineStats` counter.
- Non-ground arguments do not unfold in this phase -- they stay congruence-
  only, exactly today's behavior.

**Acceptance:** `(head-of (Cons 1 (Nil)))` from the header: `1 proven`, no
`TUR-W0372`, runtime check elided in the emitted C. `len` at depth: a
3-cons literal against `(> (len v) 2)` proves; against `(> (len v) 3)` is
**`TUR-E0371` with a model** where possible, else Unknown -- pin whichever
is true once `refine_model_search`'s ufunc gate (below) is or is not lifted.
A float-returning reflected measure fixture using `3.25` (never `3.0`; the
RM-B0 missort is the standing warning) plus a not-tightened negative control.
The RF1 sabotage fixture: with gates stubbed, `f(x) = 1 + f(x)` reflected at
one call site must discharge a false obligation -- proving the gate is
load-bearing, per the WF sabotage convention. Source fuzzer
(`tests/refine-fuzz-src.py`) gains reflected shapes -- both prior soundness
bugs lived in the encoder, so `--n 400` at two seeds is non-optional.

### RF4 -- Non-ground unfolding (only if the motivating cases demand it)

**Goal:** `(len xs)` unfolds when hypotheses pin `xs`'s constructor.

When congruence already knows `xs = (Cons h t)` (a match-arm hypothesis, per
the ctor-axiom machinery in `rt_prove_paths`), the reduction of RF3 applies
through the equality. Mechanically: before declining a non-ground argument,
ask the obligation's hypotheses (syntactically, pre-solver) whether the
argument is equated to a constructor form, and unfold at that form.
Anything requiring per-arm guarded equations over tag/selector symbols --
the general encoding -- stays out until a real predicate (`sorted?` over a
matched list is the likely first) shows the need. **Do not build the general
encoding speculatively**; it multiplies terms into every cube and the caps
(`REFINE_MAX_CUBES`, `REFINE_MAX_EUF_TERMS`) exist because term count is the
cost model.

**Acceptance:** a `match`-guarded `(sorted? xs)` shape proves; cube/term
counts under `TUR_REFINE_STATS=1` recorded before/after on the fixture
suite, with a stated budget for acceptable growth.

### RF5 -- Diagnostics, strict mode, tooling

**Goal:** failures are first-class and inspectable.

- Register `TUR-E0383`/`TUR-W0384` (four-place ritual + `tur explain`
  long-forms).
- `--strict-refine` promotes `TUR-W0384` per the standing pattern (strict
  only raises severity of existing reports, `refine_discharge.c`'s three
  sites). Confirm with a fixture that the fuel default is not load-bearing
  on strict-mode build success for the in-tree fixtures (deterministic
  reduction + memo makes this checkable, not hopeful).
- `--dump-reflect`: one line per site -- verdict, decreasing argument
  position, rejection reason -- following `--dump-write-frames`.

### RF6 -- The unlocked follow-ons (each optional, each measured)

Three existing rough edges are downstream of the same missing fact; none
justifies the plan alone, each is cheap once RF3 exists:

1. **Counterexamples with measures.** `refine_model_search` declines any VC
   with ufuncs (`refine_solver.c:300`) because an opaque symbol has no
   interpretation to evaluate. A reflected measure *does*: evaluate its
   application at candidate models by the same reduction. Lift the gate only
   for VCs where every ufunc is reflected-and-total and every application
   grounds within fuel. This is an error-message win (`TUR-E0371` regains
   its model on measure obligations).
2. **`ENC_MAX_PROPAGATE` as a well-founded budget.** The depth-4 cutoff and
   `propagating[]` stack are a compiler termination hack standing in for a
   logic termination argument; a proven-decreasing callee can carry a
   per-measure budget instead. Touch only if a real program hits the cutoff.
3. **RT4's justification becomes true as stated.** The return-refinement
   propagation comment's partial-correctness argument is vacuous for a call
   that never returns; a total callee makes it hold. Documentation truth,
   zero code.

---

## Why not now (updated)

1. **Still no measured demand.** The 2026-08-02 finding stands: 85+
   `#refine{}` fixtures, zero whose predicate calls a recursive user
   function; `stdlib/refine.tur` is nine scalar aliases with no container
   measure. Unlike the sibling loop-invariants plan (whose trigger the ECS
   v1 work has partially fired), nothing on the v1 track asks for
   reflection. The gap was found by reasoning about the design, not by
   hitting it.
2. **The decision in RF0 has not been taken.** Elaboration frames it; it
   still must be made deliberately, in writing, before code is cut.
3. ~~The cost is concentrated in a checker the language has never had, and
   the coverage half may be larger than the termination half.~~ **Corrected
   by elaboration:** coverage is the smaller half (already enforced for
   ADT/union scrutinees; RF2 is three rejections), and the checker reuses
   the WF2 deferred-pass shape wholesale. The concentrated cost is RF3's
   encoder work -- the one place both historical soundness bugs lived, hence
   the fuzz/sabotage weight there.

## The trigger

Start this when a real program wants it. Concretely, any one of:

- A structure-indexed type lands in `stdlib/refine.tur` -- a bounded index, a
  non-empty container, a sortedness predicate -- and someone tries to prove
  something about it and cannot.
- The ECS refinement work wants a measure over a component set or an entity
  generation and hits the opacity wall.
- A report is filed with a concrete measure the author wanted unfolded,
  showing the `TUR-W0372` it produced.

Until then this file is the record, and "no measured demand" is the answer.

---

## Explicitly not in scope

- **Program termination / total correctness.** See "What this is NOT". A
  non-terminating function that is never `^reflect`ed is legal, unremarkable,
  and frequently correct.
- **Ranking functions or well-founded orders written by the user.**
  Structural recursion or decline; `(f (- n 1))` declines by design in RF1.
  A user-supplied termination measure is a much larger surface and is not
  needed for the motivating cases.
- **Mutual recursion.** Declined in RF1; the `effect_check.c` fixpoint is
  the template if demand appears.
- **Coinduction / productivity.** Infinite structures are not supported by
  the refinement layer and nothing here changes that.
- **Automatic reflection.** Opt-in only. Auto-reflecting every pure
  recursive measure would silently change compile cost on existing code,
  which the graduation measured and held at 1.004x.
- **Refinement inference.** Inherited non-goal from the parent plan;
  permanent, not deferred.
- **Fixing the `#reads` trusted grant.** `#reads` is "a promise, not a
  checked fact" and is the current design's one "fails toward the wrong
  answer" hole. Totality does not fix it -- a checked read-effect system
  would. Keep the two separate.
- **General guarded-equation encoding of `match` bodies.** RF4's reduction-
  through-congruence only; see the term-count argument there.

---

## References

- [refinement-types-plan.md](../../archive/refinement-types-plan.md) -- the
  parent plan; "Why checking, not inference" is the constraint this one
  inherits, and its termination non-goal is what RF0 carves out from.
- [refine-predicate-measures-plan.md](../../archive/refine-predicate-measures-plan.md)
  -- RM-B; bool-returning measures as predicate atoms (what makes reflecting
  them worth anything) and the `ret_sort` plumbing RF3 rides.
- [refine-float-measure-missort.md](../../archive/history/refine-float-measure-missort.md)
  -- the soundness bug that mandates RF3's `3.25` fixture discipline.
- [checked-write-frames-plan.md](../../archive/checked-write-frames-plan.md) -- WF2's
  `WriteFrameSite` + deferred resolver is the structural template for RF0/RF1,
  and its sabotage-run convention is the acceptance style RF1/RF3 adopt.
- [loop-invariants-plan.md](loop-invariants-plan.md) -- the sibling plan;
  shares the termination non-goal and the structure-indexed-type trigger, and
  reuses `TUR-E0371`/`TUR-W0372` rather than claiming codes of its own.
- [../../guides/refinement-types-guide.md](../../guides/refinement-types-guide.md)
  -- the supported fragment, measure rules, and purity walk this plan works
  within.
- [../../guides/refinement-solver-internals-guide.md](../../guides/refinement-solver-internals-guide.md)
  -- the staged decision procedure and its caps (the RF4 cost model).
- Liquid Haskell, `{-@ reflect @-}` -- direct prior art for opt-in reflection
  gated on totality. Dafny's `fuel` -- prior art for bounded unfolding as the
  quantifier-free encoding.
