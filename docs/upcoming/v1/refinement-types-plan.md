# Refinement Types -- Prototype Plan (RT0--RT7 + in-house solver S0--S4)

> ## Landed so far (2026-07-24)
>
> **RT0--RT3 and S0--S3 are implemented and on `main`'s feature branch**, gated
> behind `--enable=refined` / `#lang turmeric refined`. RT5b (`stdlib/refine.tur`)
> landed with them. The user-facing write-up is
> [docs/guides/refinement-types-guide.md](../../guides/refinement-types-guide.md).
>
> | Phase | State | Where |
> |---|---|---|
> | RT0 gate + discharge bit + pipeline hook | done | `experiments.c`, `lang_layers.c`, `elab_fns.c`, `elab_toplevel.c` |
> | RT1 constraint collector + env | done (incl. call-site crossings) | `refine_collect.{c,h}` |
> | RT2 normalized VC + seam + SMT-LIB | done | `refine_vc.{c,h}`, `refine_smtlib.{c,h}` |
> | RT3 discharge chain + diagnostics | done | `refine_discharge.{c,h}` |
> | RT3 Z3 scaffold (dev-only) | done, EXERCISED against Z3 4.13 | `refine_libz3.c`, `TUR_REFINE_Z3_ORACLE` |
> | RT3 differential fuzzing vs Z3 | done | `tests/unit/refine_fuzz.c` |
> | S0 trivial | done | `refine_solver_s0.c` |
> | S1 congruence closure (EUF) | done | `refine_solver_euf.c` |
> | S2 linear arithmetic | done (Fourier-Motzkin, not simplex -- see below) | `refine_solver_arith.c` |
> | S3 Nelson-Oppen | done (convex exchange; no integer case-split) | `refine_solver_no.c` |
> | S4 boolean structure | done (small-DNF cube expansion) | `refine_solver.c` |
> | RT5b stdlib refinement aliases | done | `stdlib/refine.tur` |
> | RT4 predicate propagation | done (+ declared-result propagation) | `elab_fns.c`, `refine_collect.c` |
> | RT6 error message quality | done | `refine_discharge.c` |
> | RT5a WASM confirm, RT7 caching | not started | -- |
>
> ### Deliberate deviations from the plan as written
>
> - **S2 is Fourier-Motzkin over exact rationals, not S2a-Bellman-Ford-then-
>   S2b-simplex.** FM is a strict superset of difference logic (so the S2a
>   coverage target -- `0 <= i`, `i < len`, `i + 1 <= n` -- is met), decides
>   conjunctions of linear constraints outright over the rationals, and is a few
>   hundred lines rather than the multi-week incremental-simplex investment. Its
>   weakness is combinatorial blow-up, which the `Unknown` escape hatch absorbs:
>   `REFINE_MAX_LA_CONSTR` bounds growth and a cap hit degrades to a runtime
>   check. Integer strict constraints are tightened (`e < 0` => `e <= -1`), which
>   is what discharges `x > 0 |= 2x > 0`. If profiling ever shows the cap biting
>   on real obligations, the Dutertre--de Moura simplex slots in behind the same
>   `la_*` interface with no change above it.
> - **S4 is not a separate stage.** The small-DNF cube expansion the plan
>   assigns to S4 is shared machinery in `refine_solver.c` that S1/S2/S3 all run
>   over, because every theory stage needs conjunctions of literals to work on.
>   The cap-to-`Unknown` behavior is as specified; no DPLL(T) engine exists or
>   is planned.
> - **`RT_INVALID` comes from a bounded counterexample search**, not from a
>   theory solver. The proving stages only ever answer Valid or Unknown -- they
>   refute the negated goal, and failing to refute proves nothing. So after the
>   chain comes back Unknown, `refine_model_search` enumerates a small candidate
>   assignment space and *evaluates* `hyps AND (not goal)` exactly; a satisfying
>   assignment is a genuine witness, which is what makes `TUR-E0371` an
>   actionable error with a model rather than a guess. It declines VCs containing
>   uninterpreted symbols (a measure has no fixed interpretation to evaluate).
> - **The `refined` experiment's `introduced`/`expires_at` are `0.31.0` /
>   `0.34.0`** (VERSION was 0.30.8 at landing).
>
> ### Bugs found and fixed on the way (all pre-existing, all in the CT layer)
>
> 1. **A `#refine{...}` parameter got no runtime check at all.** `[x : #refine{...}]`
>    reaches the elaborator wrapped in an `F_TYPE_ANN` (the `:` separator), and
>    the CT1 predicate collector only unwrapped a bare `F_CONTRACT_TYPE` -- the
>    spelling the migration away from bare braces left behind. The predicate was
>    silently dropped. Now collected from the RESOLVED TYPE instead of the Form,
>    which fixes all three spellings at once (bare brace, `: #refine{...}`, and a
>    named alias, which has no contract Form at the use site).
> 2. **A contract RETURN type did not compile.** `: #refine{ r : int | p }` left
>    `return_kind == TY_CONTRACT`, so every call site failed to type the result
>    (`got { _ : ? | ... }`). Now peeled to the base type, with the predicate
>    riding along as a postcondition.
> 3. **A refinement predicate's free names were mistaken for implicit type
>    parameters.** `form_mentions_type_param` walked the whole `F_CONTRACT_TYPE`
>    including the predicate, so `[v : int, n : int]` with a `(len v)` refinement
>    lost both parameters to type-variable inference and then failed to parse
>    their annotations. It now descends only into the base-type slot.
> 4. **`deftype` of a contract body produced a `TY_REC`**, making every use site
>    fail with `expected <rec>, got int`. A contract body now binds the name
>    directly to the contract type -- which is what makes `stdlib/refine.tur`
>    possible at all.
>
> ### Call-site crossings (landed 2026-07-24)
>
> RT1's first table row -- passing an argument where the parameter declares a
> refinement -- is now collected and discharged. `(safe-div 10 0)` is a compile
> error rather than a runtime panic.
>
> Three implementation decisions worth recording:
>
> - **Resolution is DEFERRED to after the whole unit elaborates.** At the moment
>   a call is elaborated the callee may still be a pass-1 forward declaration
>   with no predicates stamped. Because `elab_defn` *reuses* that binding rather
>   than replacing it, recording `(callee-binding, call-form)` during
>   elaboration and resolving afterwards makes the check independent of
>   definition order -- a call to a later-defined function behaves exactly like
>   a call to an earlier-defined one. Crossings are deduplicated through a hash
>   set, not a linear rescan, so an opted-in build does not go quadratic in its
>   call count.
> - **The caller's hypothesis environment is BACK-FILLED, not threaded.**
>   `elab_defn` marks the crossing range its body produced and stamps the
>   environment once the body is done. A mutable "current env" on `Elab` would
>   have to be unwound correctly through that function's many early-error
>   returns, and a stale one left behind by a failed path would hand a later
>   crossing hypotheses that do not hold there -- the exact direction the
>   soundness invariant forbids.
> - **An unproven argument is NOT an error by default; a definitely-wrong one
>   is.** The split is on whether the counterexample is *closed*: a goal with no
>   free variables evaluates to false on the values written at the site, which
>   is a defect. An open counterexample only says "not for every input", which
>   for an argument is the ordinary condition of partially-annotated code -- and
>   the callee still checks on entry. Erroring on it would have made the
>   experiment impossible to adopt incrementally. `--strict-refine` opts into
>   the stricter reading. (A function's own return refinement keeps the strict
>   treatment either way: an open counterexample there means the function's
>   claim about *itself* is false.)
>
> The callee's entry check is still always emitted. Eliding it needs
> whole-program call-graph knowledge including exported and indirect callers,
> and that is a separate piece of work with real soundness preconditions.
>
> ### RT4 predicate propagation (landed 2026-07-24)
>
> Landed with a piece the plan does not name, and which turned out to matter
> more than the templates: **propagating DECLARED return refinements into call
> results.** A call encodes as an uninterpreted term either way, but when the
> callee has a return refinement that predicate is a fact about the value the
> call produced. Asserting it is what makes refined code compose --
> `(twice (double-pos p))` goes from unknown to proved -- and without it RT4's
> inferred refinements would have had nowhere to be used. Both flow through the
> same `RefineFnResolver` hook, so `refine_collect.c` stays free of scope and
> binding knowledge.
>
> The soundness argument for assuming a declared refinement is that something
> enforces it: it was proved statically, or the runtime check that guarantees
> it would have panicked first. That is conditional, and the condition is
> checked -- when contracts are stripped (`--no-contracts`, or a release build
> without `--keep-contracts`) an unproved refinement is **not** published, and
> call sites go back to treating the result as opaque. A first cut put a
> blanket `g_no_contracts` refusal in the resolver instead; that was both
> redundant (the publish gate already knows) and wrong (it discarded INFERRED
> refinements, which are proved facts and hold regardless of any check).
>
> The template layer itself is what the plan describes: five shapes tried
> against the body, first proved one wins, nothing emitted. It is emphatically
> not inference -- no search over a recursive constraint system, just guesses
> that are individually checked, so a wrong guess costs a discarded proof
> attempt rather than a wrong answer. `tests/fixtures/errors/refine-
> unpropagated-result` is the fixture that would go green if that ever stopped
> being true.
>
> Known asymmetry: call-site crossings are resolved after the whole unit, so
> they see every callee's refinement; a function's own return obligation is
> decided inline (that is what lets its check be elided), so it only sees
> functions already elaborated. Under mutual recursion one direction may miss
> one. This can only lose a hypothesis, never add a false one.
>
> ### RT6 error message quality (landed 2026-07-24)
>
> A failing obligation now reports claim -> witness -> remedy: the predicate as
> the user wrote it (rendered through `fmt_print`, so it matches the source),
> the counterexample, and a `help:` line naming a fact that would discharge it.
>
> The hint is the plan's "second seam query" taken literally: a candidate is
> asserted as a hypothesis and the chain is asked again, so only a candidate
> that genuinely discharges the goal is offered. Two candidate families --
> comparisons against literals the VC already mentions, and relations between
> two variables. The second family is what produces `(< i n)` for an index
> obligation; no literal bound can express it, and that is the shape the plan's
> `SizedVec` motivation is about.
>
> Two guards, both learned by writing the tests:
>
> - **A contradictory candidate is rejected.** Adding `(> x 0)` when `x < 0` is
>   already known makes the hypotheses unsatisfiable, which discharges the goal
>   by ex falso -- and would have the compiler suggest constraining a variable
>   to be both negative and positive. Each candidate is checked for
>   satisfiability before it is offered.
> - **An already-valid obligation gets no hint.** The discharge pass only calls
>   the search on a failure, so this was unreachable in practice; the check
>   belongs in the function anyway, because the function is exported and should
>   be correct for any caller.
>
> `refine_hint_search` is exported rather than static specifically so the
> "never suggests a contradiction" property can be unit-tested. Substring
> matching over a fixture's stderr can confirm a hint *appears*; it cannot
> confirm a wrong one does not, and that is the property that matters.
>
> Deviation from the acceptance criteria: the plan asks for snapshots in
> `tests/fixtures/refine/error-messages/` diffed by the session-types snapshot
> mechanism. The repo's error-fixture harness matches `expected.diag`
> substrings, so the five representative shapes are asserted that way instead
> of inventing a parallel mechanism -- three new `errors/refine-messages-*`
> fixtures plus tightened assertions on the existing closed-value, unknown, and
> nonlinear fixtures.
>
> ### Indirect calls (investigated + partly landed 2026-07-24)
>
> Three shapes, three different answers:
>
> - **Alias of a global** (`(let [g safe-div] (g 10 0))`) -- already worked.
>   `elab_call` folds a let-bound alias of a global to the global before the
>   refinement hook runs, so the crossing lands on the real callee. Pinned by a
>   fixture now.
> - **Lambda with contract parameters** -- was a HARD COMPILE ERROR, not a
>   missing feature. `elab_fn` never peeled `TY_CONTRACT` to its base type (the
>   same hole the contract *return* type had), so the parameter's type stayed
>   the contract type and every call failed with `expected { _ : ? | ... }, got
>   int`. `#refine` on a lambda parameter did not compile at all. Now peeled,
>   given a CT1 entry check, and checkable at the call site.
> - **Function-typed parameter** -- genuinely higher-order. Nothing is checked
>   and nothing can be without refinements in function types, which the
>   prototype excludes. Sound (the callee's entry checks still run); documented
>   rather than papered over.
>
> Two things worth recording from the implementation:
>
> - The CT1 parameter-check injection is now ONE function shared by `defn` and
>   `fn`, rather than a second copy in the lambda path. A lambda's contract
>   parameter being silently decorative is exactly what a divergent second copy
>   produces.
> - The link from a `let` binding to its lambda's predicates is read off the
>   INIT EXPRESSION's FnDef, not the closure-binding graph. A non-capturing
>   lambda has no closure box, so `closure_fn_binding` is unset for it -- the
>   graph route would have missed the simplest case, which is the one people
>   write.
>
> ### Typeclass method dispatch (landed 2026-07-24)
>
> The same contract-peel defect, reached a THIRD time: an instance method's
> parameter annotation kept its `TY_CONTRACT` type, so the method body could
> not use the value (`'*' arg 2: expected int, got { v : int | ... }`) and a
> refined method signature did not compile. After fixing it in `defn` returns
> and then `fn` parameters, the third occurrence was the signal to stop
> patching sites: the peel now lives in one documented helper
> (`rt_peel_contract`, elab_internal.h) that names the failure mode and says to
> call it from any new annotation site. The CT1 entry-check injection is
> likewise one shared function across `defn`, `fn`, and instance methods.
>
> Instance methods parse their parameters in pass 1 and elaborate their bodies
> in pass 2, so the predicates ride on the method's BINDING -- the record that
> spans both passes, and also where a dispatch site looks for them.
>
> A statically-resolved dispatch is now checked like any other call; the
> receiver occupies slot 0 of the impl, so parameter and argument slots line up
> without special-casing. Dotted and bare forms share the resolution point and
> so are checked identically. A dispatch that stays dynamic is deliberately not
> checked -- which method runs is unknown there.
>
> One diagnostic detail worth keeping: the message names the method as the
> SOURCE writes it. A method binding carries its mangled instance symbol
> (`__inst_Scaler_scale_hyby_int`), which is not something to put in front of a
> user, so the crossing records the call form's head instead.
>
> ### Z3 oracle: actually built and run (2026-07-25)
>
> The scaffold had never been compiled. Building it against a real Z3 4.13
> found three bugs, two of them in the scaffold itself and both of the kind
> that only running it can reveal.
>
> **1. The oracle could not link at all.** `refine_libz3.c` is compiled into
> `tur_core`, which is an OBJECT library, so a per-target `PRIVATE` link of
> `z3::libz3` never reached the executables that embed `$<TARGET_OBJECTS:
> tur_core>` -- every test harness failed with undefined `Z3_*` references.
> Fixed with a directory-scoped `link_libraries`, which is exactly the
> accommodation the Windows `ws2_32` block a few lines above already makes for
> the same reason.
>
> **2. One Z3 context for the whole process silently poisoned every query.**
> The code assumed `Z3_eval_smtlib2_string` is self-contained because each call
> carries its own declarations. It is not -- assertions accumulate on the
> context. A single self-contradictory query poisons it, and since we assert
> the hypotheses AND the negated goal, a trivially-VALID obligation is exactly
> such a query. After the first one, every later query returned `unsat` and the
> oracle answered VALID to everything. As the chain tail that proves false
> obligations; as the cross-check it rubber-stamps the in-house stages, which
> is worse, because an oracle that never disagrees is not an oracle. Fixed with
> a fresh context per query.
>
> **3. `sat` was read as INVALID even for an abstracted VC.** The VC replaces
> nonlinear and measure terms with uninterpreted functions. Abstraction is
> sound in ONE direction: `unsat` of the abstraction implies `unsat` of the
> concrete obligation, so VALID transfers -- but a model that assigns an opaque
> symbol some convenient value says nothing about the real function. Z3
> correctly reports `sat` for the abstraction of `x>0, y>0 |- x*y>0`, which is
> a TRUE obligation, and the scaffold turned that into `TUR-E0371` on correct
> code. `sat` may only be read as INVALID when nothing was abstracted -- the
> same rule `refine_model_search` already followed.
>
> ### Differential coverage: what the corpus could and could not establish
>
> Compiling the whole fixture corpus with `refined` forced on under the oracle
> found zero disagreements -- but that is a much weaker statement than it
> sounds, because almost no fixture contains a `#refine`: the first 400 fixture
> files yield **3 obligations** between them. Fixture coverage is not
> differential coverage.
>
> `tests/unit/refine_fuzz.c` is the harness that actually exercises the solver:
> it generates random VCs in the supported fragment (linear integer arithmetic,
> boolean structure, and uninterpreted applications in a quarter of them), runs
> the in-house chain and Z3 on each, and fails on either direction of
> disagreement -- in-house VALID where Z3 says INVALID (a proof that is not a
> proof), or in-house INVALID where Z3 says VALID (a counterexample search
> claiming a witness it does not have). It builds only in an oracle build and
> is deterministic, so a failure reproduces from the printed seed.
>
> Results are also a fair completeness measurement, which nothing before this
> provided: on 4000 VCs the chain agreed with Z3 on 3070 and was incomplete on
> 159 (~5% of the VCs Z3 could decide). Those 159 cost a runtime check, which
> is the designed outcome.
>
> ### Retirement criteria: still not met
>
> Two of the three criteria remain open, and the honest reading is that the
> oracle has just started doing its job rather than finished:
>
> - *Bootstrap discharged* -- the in-house chain already decides every fixture
>   the oracle does, so this one holds.
> - *Oracle trust established* -- a soak window and the labelled SMT-LIB
>   `QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` corpora are still not in the repo. The
>   fuzz harness is a real start; a checked-in labelled corpus that runs
>   WITHOUT Z3 present is what the criterion actually asks for.
> - *No scaffold references in shippable code paths* -- re-verified: the
>   default build's `tur` has zero `z3` symbols, zero `z3` in its link line,
>   and no `TUR_REFINE_Z3_ORACLE` string; Release + oracle still fails
>   configuration outright.
>
> ### Next slice
>
> Candidates, roughly by value:
>
> - **Class/instance refinement variance** -- nothing checks that an instance's
>   parameter refinement is no stronger than its class signature's, so an
>   over-strict instance panics at its entry check on an argument a generic
>   caller was entitled to pass. (Open Question 6 in this plan proposed
>   rejecting refined method signatures outright; supporting them and leaving
>   variance unchecked is the softer landing, since the entry check catches the
>   case loudly.)
> - **Dynamic typeclass dispatch** -- a dispatch with no statically-resolved
>   instance is not checked; which method runs is unknown at the site.
> - **Higher-order callees** -- a function-typed parameter carries no
>   refinements in its type, so neither its body nor its callers can check the
>   eventual call. This needs refinements in function types, which the
>   prototype excludes; the callee's own entry checks still run, so only the
>   static crossing is lost.
> - **A purity gate on the encoder** -- two syntactically identical calls
>   currently encode to the same term. Predicates must be pure, but argument
>   expressions need not be, so an effectful call appearing twice in one
>   obligation would be modelled as one value.
> - **Branching-body propagation** -- RT4 is limited to single-expression
>   bodies; a path-sensitive join at merge points would cover `if`/`match`.
> - **Whole-program entry-check elision** -- the piece that turns the call-site
>   layer from a diagnostic into a code-size/perf win. Needs an
>   exported/address-taken analysis before it can be sound.
>
> ---

> **Status:** RT0--RT3 + S0--S3 + RT5b landed (see "Landed so far" above);
> RT4/RT5a/RT6/RT7 not started. RT0 syntax/storage is largely covered by the
> existing Contract Types (CT0--CT4) infrastructure; the `#refine{var : T | p}`
> reader is already shipped. The remaining work is the constraint-generation
> and discharge pipeline (RT1--RT4), a stdlib layer of predicate-annotated
> types (RT5--RT6), and -- the substantive new direction -- a **staged,
> in-house decision procedure (S0--S4)** that becomes the *only* shipped solver.
> Z3 is **scaffolding**: a development-time correctness oracle and a transitional
> bootstrap that is never bundled, never auto-fetched, and never linked into a
> default or release build, and that is retired outright once S0--S3 cover the
> fragment. The shipped product carries **zero heavyweight solver dependency**.
>
> **Prerequisites:** Contract Types (CT0--CT4). The `TY_CONTRACT` node,
> `F_CONTRACT_TYPE` reader tag, `#refine{...}` reader, and predicate-as-`Form*`
> storage are already in place and reused directly.
>
> **Gate:** `refined` -- an `EXPERIMENTS[]` row (`--enable=refined`), surfaced
> per-file as the `#lang turmeric refined` semantic layer. There is **no**
> `-Xrefinements` flag; the retired `-X` surface does not come back for this.
> See "Gating" below.
>
> **Last updated:** 2026-07-24

---

## Motivation

Contract types (`#refine{ x : T | p }`, CT0--CT4) verify predicates at
**runtime**. Refinement types go further: the compiler attempts to **prove**
predicates statically, emitting a runtime check only when static proof fails.
This eliminates whole classes of defensive guards that programmers currently
scatter through the codebase -- e.g. `require! (!= divisor 0)` becomes a
type-level obligation the compiler resolves at every call site.

The single fact that shapes this entire plan: **every refinement already has a
runtime-contract meaning.** Because of that, the static discharger is allowed
to answer `Unknown` on any obligation and stay sound -- the obligation simply
falls back to the runtime check it would have had anyway (or, under
`--strict-refine`, a compile error telling the user to add an annotation).
That fallback is what makes an incrementally hand-rolled solver *safe*: the
soundness obligation is one-directional (never say **valid** when it is
**invalid**; saying **unknown** is always fine), so a 40%-complete solver is
still a real, shippable feature rather than a broken one.

Goals:

- Catch division-by-zero, out-of-bounds access, and overflow at compile time
  rather than at runtime.
- Allow library authors to publish APIs with machine-checked pre/post conditions
  (e.g. `Vec` indexing, `sqrt` on non-negative floats).
- Compose with the existing substructural and session type systems without
  introducing mutual dependencies.
- Grow a small, auditable, dependency-free decision procedure that lives inside
  `tur.wasm`, so the browser playground gets real static checking at zero
  download -- and no heavyweight backend is ever fetched, in the browser or
  anywhere else.

Non-goals for this prototype:

- Full dependent types (Pi types, proof terms, eliminators).
- Universal or existential quantification inside predicates.
- Termination checking or total-correctness verification.
- Refinements that depend on algebraic effects or mutable state.
- **Refinement *inference*** (LiquidHaskell-style predicate abstraction +
  Horn-clause fixpoint). Turmeric refinements are **written, not inferred**;
  we only *check* closed implications. This deletes the single hardest layer
  of a refinement backend (see "Why checking, not inference" below). RT4 adds a
  deliberately small, template-based propagation convenience on top -- it is not
  general inference.

---

## Background: What Is Already Shipped

| Component | Location | Notes |
|---|---|---|
| `TY_CONTRACT` type node | `src/compiler/types.h:121` | Stores `{var : base | pred}` |
| `F_CONTRACT_TYPE` reader tag | `src/compiler/reader.c` | Parses `#refine{ x : T \| p }` |
| `type_contract()` constructor | `src/compiler/types.h:936` | Builds a `TY_CONTRACT` from a `Form*` predicate |
| Contract elaboration | `src/compiler/elab_types.c:1072` | Checks predicate is well-typed + pure |
| Runtime check insertion | `src/compiler/elab_core.c` | Emits C `assert` / panic for CT3 |
| `:pre` / `:post` in `defn` | `src/compiler/elab_fns.c` | Parsed and elaborated; currently runtime only |
| `EXPERIMENTS[]` registry | `src/runtime/experiments.c` | Where the `refined` row lands (RT0) |
| `LANG_LAYERS[]` registry | (lang-layers-plan L0) | Where the `refined` semantic layer lands (RT0) |

Refinement types reuse all of the above. The delta is:

1. A **constraint collector** that gathers `TY_CONTRACT` predicates as proof
   obligations during elaboration (RT1).
2. A **normalized VC representation** -- the neutral, backend-independent form
   every obligation is lowered to before a solver sees it (RT2).
3. A **solver seam**: the `decide(vc) -> Valid | Invalid(model) | Unknown`
   contract that both the in-house procedures and Z3 implement, and fall
   through to each other along (RT2/RT3).
4. A **discharge pass** that runs the seam and marks obligations proven (elide
   runtime check) or not (keep runtime check, optionally warn/error) (RT3).
5. A **Z3 backend** behind the seam, used as *scaffolding* -- a dev-build
   correctness oracle and a transitional bootstrap so refinement types can be
   exercised end-to-end while the in-house solver is still a stub. It is never
   part of a default or release build and is retired once S0--S3 land (RT3).
6. A **staged in-house decision procedure** (S0--S4) that decides the fragment
   with no dependency and *becomes the only shipped solver* (S0--S4).
7. A **predicate propagation** layer for simple arithmetic (RT4).
8. A **WASM integration layer**: the in-house solver compiles into `tur.wasm`
   directly and is the whole story there -- no Z3-in-WASM, no `z3-solver`
   fetch, no bundle cost (RT5a).

---

## Design Decisions

### Gating: the `refined` experiment + `#lang` layer

Refinement checking is an in-flight compiler feature, so it ships behind the
experiment mechanism, **not** a resurrected `-X` flag. Two coordinated
registrations:

1. **`EXPERIMENTS[]` row** in `src/runtime/experiments.c` with all seven
   descriptor fields populated:

   ```c
   { "refined",
     "static discharge of #refine{...} predicates (refinement types)",
     "docs/upcoming/v1/refinement-types-plan.md",
     "0.<minor>.0",              /* introduced -- set at RT0 landing */
     "0.<minor+3>.0",            /* expires_at -- soft deadline; review at cut */
     XF_LIFECYCLE_PROTOTYPE,
     &g_opt_refined },
   ```

   The feature's discharge entry point reads `g_opt_refined` and calls
   `experiment_warn_if_used("refined")` so the lifecycle warning
   (TUR-W0060/W0061) fires.

2. **`LANG_LAYERS[]` row** (semantic layer, per the lang-layers plan L4) whose
   `experiment` field points at that same row. `#lang turmeric refined` is then
   *exactly* `--enable=refined` scoped to one file -- no parallel enable path. A
   project manifest that disables the `refined` experiment turns a
   `#lang ... refined` file into a **hard error** (`this file requires layer
   'refined', which is disabled by the project manifest`), never a silent
   ignore.

Enable precedence is the standard experiment chain: user file
(`~/.config/turmeric/experiments.tur`) < project manifest (`build.tur`
`:experiments`) < CLI (`--enable=refined`), with `#lang turmeric refined` acting
as the file-scoped form of the CLI enable. When `refined` is off, the elaborator
behaves exactly as with contracts alone: predicates parse and stay runtime-only,
the discharge pass does not run.

`--strict-refine` (a diagnostic-strictness knob, *not* an experiment) upgrades
`Unknown`/`Invalid` obligations from "keep the runtime check" to a hard compile
error -- for users who want a fully-discharged build with no silent runtime
fallbacks.

### Predicate Language Scope -- co-designed with the solver

The prototype targets **quantifier-free linear integer/real arithmetic with
equality and uninterpreted functions** -- SMT-LIB `QF_UF` + `QF_IDL`/`QF_LIA`/
`QF_LRA`. This is not an arbitrary cut: it is the exact fragment the in-house
solver (S0--S4) can be made *complete* on, and it is also the corner of Z3 that
the fallback ever touches.

```
pred ::= (= e e) | (!= e e) | (< e e) | (<= e e) | (> e e) | (>= e e)
       | (and pred pred ...) | (or pred pred ...) | (not pred)
       | (=> pred pred)
       | (measure e ...)             ;; named measure -> uninterpreted fn (EUF)
       | true | false

expr ::= integer-literal | float-literal
       | bound-var                   ;; the variable from #refine{ x : T | ... }
       | fn-param                    ;; a visible parameter in scope
       | (+ e e) | (- e e) | (* e const) | (/ e const)  ;; linear only
       | (mod e const)               ;; modular arithmetic
```

Two language rules -- stated as rules, not accidents -- keep the fragment on the
cheap side of the solver cliff:

- **Named measures are uninterpreted functions.** `len`, `elems`, and similar
  measures become opaque function symbols reasoned about by congruence closure
  (EUF), never unfolded. This is what makes S1 tractable.
- **Variable*variable multiplication is uninterpreted.** `(* x const)` is linear
  and stays interpreted; `(* x y)` with both variable is treated as an opaque
  term. Congruence closure still handles it soundly (it loses the arithmetic
  facts, not soundness), and any genuinely nonlinear obligation falls through to
  Z3 or to a runtime check. **We do not climb the nonlinear wall** -- no nlsat,
  ever. The encoder warns (TUR-W0373) when a predicate contains a nonlinear
  subterm.

Restricting the predicate language this tightly is a lever we hold that
LiquidHaskell-on-Haskell does not: it is *our* language, so "small **and
complete on the permitted fragment**" is an achievable target rather than a
fantasy. Language design and solver scope move together.

Higher-order predicates, recursion inside predicates, quantifier alternation,
and effect-dependent predicates are rejected at elaboration time with a
dedicated diagnostic.

### Why checking, not inference

LiquidHaskell's hard part is not *deciding* verification conditions -- it is
*inferring* the refinements in the first place (predicate abstraction plus
Horn-clause fixpoint solving over `liquid-fixpoint`). That is the machinery to
dread reimplementing. Because Turmeric makes people **write** their refinements,
that entire layer is deleted. We only answer "is this one closed implication
valid?" Checking is a decision problem; inference is a search for predicates
satisfying a recursive constraint system. Dropping inference removes the hardest
~60% of what a refinement backend does, and it is the reason a hand-rolled
solver is realistic here at all.

### The Solver Seam

Every backend -- the in-house S0--S4 procedures and Z3 alike -- sits behind one
interface and returns one of three verdicts:

```c
typedef enum { RT_VALID, RT_INVALID, RT_UNKNOWN } RefineVerdict;

typedef struct {
    RefineVerdict verdict;
    /* RT_INVALID only: a counterexample model, in normalized-VC variable terms
     * (translated to source syntax by RT6). NULL otherwise. */
    RefineModel  *model;
} RefineDecision;

/* A backend decides a single normalized VC. It MUST NOT return RT_VALID unless
 * the goal is genuinely entailed by the hypotheses; returning RT_UNKNOWN is
 * always permitted and always sound. */
typedef RefineDecision (*RefineBackend)(const RefineVC *vc, Arena *a);
```

**The soundness invariant is one-directional and absolute:** a backend may
never answer `RT_VALID` for an invalid obligation. `RT_UNKNOWN` is always a
safe answer. `RT_INVALID` should carry a model but is allowed to be a
best-effort "definitely not valid, no model."

**Fall-through chain.** The discharge pass runs backends in ascending cost and
stops at the first non-`Unknown` verdict:

```
S0 (normalize/trivial) -> S1 (EUF) -> S2 (arithmetic) -> S3 (N-O combine)
   -> [dev-only Z3 scaffold] -> RT_UNKNOWN => keep runtime check (or error under --strict-refine)
```

Early stages return `Unknown` for anything outside their competence; the next
stage picks it up. **In every default and release build the chain is the
in-house stages only** -- Z3 is not linked, so an obligation no stage can decide
falls straight to its runtime check. Z3 appears as the chain's last link *only*
in dev builds that opt into the scaffold (see below); it is a bridge, not a
destination, and it is removed entirely once S0--S3 land.

This seam is the whole reason the in-house solver can be built a stage at a time
and stopped wherever we like: adding a stage only ever *moves obligations left*
in the chain (off the Z3 scaffold or off the runtime-check fallback onto a
faster, dependency-free path). It never changes an answer, because every stage
preserves the soundness invariant.

### Backend strategy: Z3 as scaffolding, in-house as the product

The shipped solver is the in-house chain, full stop. Z3 exists only to make
building that chain fast and safe, and it is designed to disappear. Its two
scaffolding roles, and the single build option (`-DTUR_REFINE_Z3_ORACLE=ON`,
**off by default**, refused entirely for release and WASM builds) that gates
both:

1. **Correctness oracle.** In an oracle build, run *both* the in-house stage and
   Z3 on every obligation and assert agreement. If the in-house solver ever
   answers `Valid` where Z3 answers `Invalid`, that is a soundness bug, caught
   immediately (`TUR-I0379`, downgraded to `Unknown` so the build stays sound).
   Fuzz the in-house solver against generated VCs plus the labelled
   `QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` SMT-LIB benchmark corpora. This is the role
   that justifies Z3's existence, and it is dev-only from day one.
2. **Transitional bootstrap.** In an oracle build, Z3 is also the chain's last
   link, so the end-to-end pipeline (RT1 collector -> RT2 VC -> RT3 discharge ->
   codegen elides the check) can be tested *before* any in-house stage exists.
   This lets RT3 land and be exercised while S0--S4 are still stubs -- without
   ever putting Z3 in a shippable artifact.

Consequences that follow directly from "scaffolding, not backend":

- **No CPM auto-fetch, no bundling, no user setup.** The oracle build uses a
  **system-installed** Z3 (`find_package(Z3 4.12 CONFIG)`); if absent, the
  oracle option is simply unavailable and the build proceeds in-house-only. Z3
  is never downloaded, never statically linked into a distributed binary, and
  never added to the WASM bundle. The 15--20 MB binary-size question the
  original plan wrestled with disappears.
- **A `refined` release build discharges only what the in-house chain can.**
  Early on (RT3, before S-stages) that is little, and unhandled obligations fall
  to runtime checks -- acceptable precisely because `refined` is an
  experiment-gated prototype with an `expires_at` contract. Coverage climbs as
  S0--S3 land; it never depended on shipping Z3.
- **The browser story is unconditional, not "tail-fetches-Z3".** S0--S2 compile
  into `tur.wasm` and are the entire discharge path there. There is no
  `z3-solver`, no lazy fetch, no `SharedArrayBuffer`/`Atomics.wait` bridge.

**Retirement.** Z3's bootstrap role ends the moment the in-house chain can
discharge the fixtures RT3 relies on (target: end of S1, fully by S3). Its
oracle role ends once the fuzzing corpus and SMT-LIB differential runs are
trusted. At that point the Z3 backend file, the `find_package` block, and the
`TUR_REFINE_Z3_ORACLE` option are **deleted** -- see "Z3 retirement criteria"
below. Nothing in the shipped compiler ever referenced them.

The one dividing line we do **not** cross: a competitive **DPLL(T)** SAT engine
driving the theories is a months-to-years project. Decision procedures for a
fixed theory or a Nelson-Oppen combination are a solo, incremental project;
DPLL(T) is not. Explicit annotations keep the propositional structure of
individual VCs small (S4 handles that with naive case-splitting / small-DNF), so
we very likely never need to cross that line. If a VC's boolean structure is
genuinely too rich, the honest answer is `Unknown` -> runtime check.

The **blueprint** for the whole in-house architecture -- congruence closure +
simplex + Nelson-Oppen + matching -- is the pre-Z3 ESC/Java theorem prover
**Simplify** (Detlefs, Nelson, Saxe). It is a complete, documented description
of exactly the thing S0--S4 build, written before SMT solvers got big enough to
make people forget you could hand-roll one.

### Relationship to Contract Types

| | Contract Types (CT) | Refinement Types (RT) |
|---|---|---|
| Predicate syntax | `#refine{ x : T \| p }` | same |
| Verification time | Runtime always | Compile time when possible; runtime fallback |
| Solver involvement | None | Yes -- via the seam (in-house, then Z3) |
| Gate | contracts (shipped) | `refined` experiment / `#lang turmeric refined` |
| `TY_CONTRACT` node | Yes | Same node; adds `rt_discharged` bit |

Refinement elaboration is **additive**: if `refined` is off, the elaborator
behaves exactly as with contracts alone. The discharge pass simply does not run.

### Interaction with Existing Type System Features

| Feature | Interaction |
|---|---|
| Borrow checker | Refinements on borrowed values are checked at borrow site; no interaction otherwise |
| Linear / affine types | A linear value with a refinement still tracks linearity; refinement discharge is independent |
| Session types | No interaction in prototype |
| HKT / GADTs | Refinements on type parameters are unsupported in prototype; raise a diagnostic |
| Sized types | Sized bounds (SZ0--SZ3) are encoding targets -- `(< i n)` for a `SizedVec n a` index naturally maps to difference logic (S2) |

---

## Architecture Overview

```
src/compiler/
  elab_types.c         -- RT0: attach rt_discharged flag; call constraint collector
  elab_fns.c           -- RT1: collect :pre/:post as obligations; propagation stubs
  refine_collect.c     -- RT1 (new): constraint collector -- walks elaborated forms
  refine_collect.h     -- RT1 (new)
  refine_vc.c          -- RT2 (new): normalized VC representation + builder
  refine_vc.h          -- RT2 (new): RefineVC, RefineVerdict, RefineBackend seam
  refine_smtlib.c      -- RT2 (new): normalized-VC -> SMT-LIB2 serializer (Z3 backend only)
  refine_smtlib.h      -- RT2 (new)
  refine_discharge.c   -- RT3 (new): discharge pass -- runs the backend fall-through chain
  refine_discharge.h   -- RT3 (new)
  refine_libz3.c       -- RT3 (new): Z3 SCAFFOLD backend; #ifdef TUR_REFINE_Z3_ORACLE; deleted post-S3
  refine_libz3.h       -- RT3 (new)
  refine_solver_s0.c   -- S0 (new): normalize + trivial discharge
  refine_solver_euf.c  -- S1 (new): congruence closure (union-find + EUF)
  refine_solver_arith.c-- S2 (new): difference logic (Bellman-Ford), then simplex LRA
  refine_solver_no.c   -- S3 (new): Nelson-Oppen combination of EUF + LRA
  refine_solver_sat.c  -- S4 (new): boolean structure (small-DNF / case-split)
  refine_solver.h      -- S0--S4 shared declarations; assembles the chain
  refine_propagate.c   -- RT4 (new): template-based predicate propagation
  refine_propagate.h   -- RT4 (new)
  diag.h               -- RT3: add TUR-E0370..TUR-E0379 refinement diagnostics
  types.h              -- RT0: add rt_discharged bit to TY_CONTRACT union arm

src/runtime/experiments.c -- RT0 (modified): add the `refined` row
(lang-layers L4)          -- RT0 (modified): add the `refined` LANG_LAYERS row

src/wasm_glue.c        -- RT5a: no change for solver (in-house S0--S2 compile in directly)

CMakeLists.txt         -- RT3 (modified): TUR_REFINE_Z3_ORACLE option (dev-only, system Z3, refused for Release/WASM)

stdlib/
  refine.tur           -- RT5b (new): Nat, Pos, NonZero, Bounded, NonEmpty, Unit
  refine-vec.tur       -- RT5b (new): SizedVec + refinement on index bounds

tests/fixtures/refine/ -- RT5b: end-to-end fixture tests
tests/unit/            -- RT2/S*: normalized-VC + per-stage decision-procedure tests
```

---

## Phase RT0: Infrastructure Hooks

**Goal:** Register the gate (experiment row + lang layer), add the discharge
bit, and wire the discharge pass into the pipeline. No solver yet -- all
predicates still fall through to runtime checks.

### Changes

**`types.h`** -- add `rt_discharged` to the `TY_CONTRACT` arm:

```c
struct {
    struct Type        *base_type;
    const char         *var_name;
    const struct Form  *predicate;
    bool                rt_discharged; /* RT0: true = statically proved; elide runtime check */
} contract_;
```

**`src/runtime/experiments.c`** -- add the `refined` row (all seven fields);
declare `g_opt_refined` in `globals.h`.

**`LANG_LAYERS[]`** (lang-layers plan L4) -- add the `refined` semantic-layer
row with `experiment` pointing at the `refined` experiment.

**`elab_types.c`** -- after the existing contract elaboration block:

```c
if (g_opt_refined) {
    experiment_warn_if_used("refined");
    refine_collect_obligation(e, ct, pred_form, source_loc);
}
```

**Compilation pipeline** -- after all elaboration is complete:

```c
if (g_opt_refined)
    refine_discharge_all(compilation_unit);
```

### Acceptance Criteria

- `tur experiments` lists `refined`; `--enable=refined` and
  `#lang turmeric refined` both enable it; a manifest `:experiments []`
  suppresses the user file and makes a `#lang ... refined` file a hard error.
- With `refined` off, behavior is identical to contracts-only.
- Existing contract tests continue to pass with the gate on and off.

---

## Phase RT1: Constraint Collector

**Goal:** Implement `refine_collect.c`. This pass walks the post-elaboration
form tree and records each `TY_CONTRACT` crossing point as a **proof
obligation**.

### Obligation Record

```c
typedef struct RefineObligation {
    const Form    *predicate;   /* the p in #refine{ x : T | p } */
    const char    *var_name;    /* the x */
    Type          *base_type;   /* the T */
    SourceLoc      loc;         /* where the crossing occurs */
    RefineEnv     *env;         /* in-scope refinements (hypotheses) at this point */
    RefineVC      *vc;          /* RT2: normalized VC, filled during discharge */
    bool           discharged;  /* set by RT3 discharge pass */
    bool           proven;      /* set by RT3: true = a backend returned RT_VALID */
    RefineModel   *counterex;   /* RT3: model if a backend returned RT_INVALID */
} RefineObligation;
```

### Crossing Points to Collect

| Situation | Obligation |
|---|---|
| Passing an `expr : T` where `T` is `#refine{ x : U \| p }` | Prove `p[expr/x]` given current env |
| Returning from a `defn` with `:post r` | Prove `:post` holds for the return value |
| Struct field write where field type is `TY_CONTRACT` | Prove field predicate holds |
| Pattern-match arm narrowing a refined type | Add the predicate to the env for that arm |

### Environment

The `RefineEnv` is a lightweight chain of `(name, predicate)` pairs pushed at
each lexical boundary. Inside `(if (> x 0) <then> <else>)`, the then-branch env
gains `(x > 0)`; the else-branch gains `(not (> x 0))`.

```c
typedef struct RefineEnvEntry {
    const char           *name;
    const Form           *predicate;   /* known-true hypothesis mentioning name */
    struct RefineEnvEntry *next;
} RefineEnvEntry;

typedef struct RefineEnv { RefineEnvEntry *head; Arena *arena; } RefineEnv;
```

### Acceptance Criteria

- `tests/unit/refine_collect_test.c` drives the collector on hand-constructed
  forms and asserts the correct obligation set.
- No obligations are collected when `refined` is off.

---

## Phase RT2: Normalized VC + Solver Seam

**Goal:** Implement `refine_vc.c` (the neutral obligation representation and the
`decide` seam) and `refine_smtlib.c` (one serializer, VC -> SMT-LIB2, used only
by the Z3 backend). The in-house stages (S0--S4) consume the normalized VC
directly and never touch SMT-LIB text.

### Normalized VC

An obligation is lowered to a backend-independent structure: a set of typed
**variables**, a set of **uninterpreted function symbols** (measures + nonlinear
terms), a list of **hypotheses** (from the env), and a single **goal**. All
`!=` are desugared to `(not (= ...))`, literals folded, and nonlinear/measure
subterms replaced by fresh uninterpreted-function applications with a side table
mapping them back to source terms (for RT6 counterexample translation).

```c
typedef enum { VS_INT, VS_REAL } VCSort;

typedef struct {
    VCVar     *vars;    size_t n_vars;    /* declared constants + sorts */
    VCUFunc   *ufuncs;  size_t n_ufuncs;  /* measures / nonlinear-as-uninterpreted */
    VCTerm   **hyps;    size_t n_hyps;    /* assumed-true facts */
    VCTerm    *goal;                      /* the thing to prove */
    UFOrigin  *origins;                   /* uninterpreted-symbol -> source-term map */
} RefineVC;
```

### The Seam

`refine_vc.h` declares `RefineVerdict`, `RefineDecision`, and the
`RefineBackend` function-pointer type (see "The Solver Seam" above). The
discharge pass (RT3) owns the ordered array of backends and the fall-through
loop; individual backends are pure `RefineVC -> RefineDecision`.

### SMT-LIB2 Serializer (Z3 backend only)

`refine_smtlib.c` walks a `RefineVC` and emits:

```
(set-logic QF_UFLIA)         ; QF_UFLRA if any VS_REAL var present
(declare-const x Int) ...    ; one per VCVar
(declare-fun len (Int) Int)  ; one per VCUFunc
(assert <hyp-1>) ...         ; hypotheses
(assert (not <goal>))        ; negate goal -- unsat => valid
(check-sat)
(get-model)                  ; reached only if sat (counterexample)
```

Term encoding is the obvious structural map (`(= a b)`, `(<= a b)`, `(+ a b)`,
`(* a const)`, `(mod a const)`, etc.). Because nonlinear/measure terms are
already uninterpreted in the VC, the serializer has no special cases for them --
they are just `declare-fun` applications.

### Acceptance Criteria

- Unit tests build normalized VCs from obligations and assert their structure.
- A serializer test compares VC -> SMT-LIB output against expected fixtures.
- Nonlinear subterms surface as uninterpreted functions in the VC and emit
  `TUR-W0373`.

---

## Phase RT3: Discharge Pass + Z3 Scaffold Backend

**Goal:** Implement `refine_discharge.c` (runs the backend fall-through chain
over all obligations) and `refine_libz3.c` (the Z3 *scaffold* backend). RT3
brings the end-to-end pipeline up with Z3 as a dev-only bootstrap so the
collector -> VC -> discharge -> codegen path is testable before any in-house
stage exists -- **without** making Z3 part of any shippable artifact.

### CMake Integration -- system Z3 only, dev opt-in, never release/WASM

```cmake
# Dev-only correctness oracle + transitional bootstrap. OFF by default.
option(TUR_REFINE_Z3_ORACLE "link a system Z3 as a dev-build refinement oracle" OFF)

if(TUR_REFINE_Z3_ORACLE)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR EMSCRIPTEN)
    message(FATAL_ERROR "TUR_REFINE_Z3_ORACLE is a dev scaffold; not allowed in Release or WASM builds")
  endif()
  find_package(Z3 4.12 CONFIG REQUIRED)   # SYSTEM install only -- never fetched/bundled
  target_link_libraries(turi PRIVATE z3::libz3)
  target_compile_definitions(turi PRIVATE TUR_REFINE_Z3_ORACLE=1)
endif()
```

Note the deliberate departures from the original plan: **no `CPMAddPackage`, no
`FetchContent`, no auto-download, no static-linking into distributables.** The
oracle uses a Z3 the developer already has (`find_package ... REQUIRED`); if it
is absent, they simply do not get the oracle build. Release and WASM builds
*refuse* the option outright, so there is no path by which Z3 reaches a shipped
binary or the web bundle.

### Z3 Scaffold Backend

`refine_libz3.c` is compiled only under `#ifdef TUR_REFINE_Z3_ORACLE`. It
implements a `RefineBackend`: serialize the VC (RT2), submit via
`Z3_eval_smtlib2_string`, map `unsat -> RT_VALID`, `sat -> RT_INVALID` (parse
the model), `unknown/error -> RT_UNKNOWN`. One `Z3_context` per compilation unit;
`Z3_push`/`Z3_pop` isolate each query. In an oracle build it serves both roles
from "Backend strategy" above: chain tail (bootstrap) and cross-check against
each in-house stage (oracle).

### Discharge Pass

```c
void refine_discharge_all(RefineObligationVec *obs, Arena *a, DiagCtx *diag);
```

For each obligation: build the normalized VC (RT2), then run the ordered backend
chain, stopping at the first non-`Unknown` verdict:

1. `RT_VALID`: set `proven = discharged = true`; set the `TY_CONTRACT`
   `rt_discharged` bit; codegen emits no runtime check.
2. `RT_INVALID`: store the model in `counterex`; emit `TUR-E0371`; keep the
   runtime check (hard error under `--strict-refine`).
3. Chain exhausted with `RT_UNKNOWN`: emit `TUR-W0372`; keep the runtime check
   (hard error under `--strict-refine`).

The chain is assembled at build time: `[S0, S1, ...]` for a normal build, with
the Z3 scaffold appended (and cross-check enabled) only under
`TUR_REFINE_Z3_ORACLE`. At the RT3 milestone specifically, no S-stage exists
yet, so an oracle build's chain is `[z3_scaffold]` and a normal build's chain is
empty (every obligation -> runtime check). That empty-chain normal build is the
honest state of the prototype until S0 lands a day later.

### New Diagnostics

| Code | Kind | Message Template |
|---|---|---|
| `TUR-E0370` | Error | `refinement predicate is ill-typed: <reason>` |
| `TUR-E0371` | Error | `refinement predicate cannot be proved statically; counterexample: <model>` |
| `TUR-W0372` | Warning | `solver returned unknown for refinement predicate at <loc>; runtime check kept` |
| `TUR-W0373` | Warning | `non-linear predicate subterm '<subterm>' treated as uninterpreted; arithmetic reasoning incomplete` |
| `TUR-E0375` | Error | `refinement predicate mentions effects; pure predicates only` |
| `TUR-E0376` | Error | `refinement on type parameter is not supported in this prototype` |

### Acceptance Criteria

End-to-end fixtures in `tests/fixtures/refine/`:

```turmeric
;; refine/proved.tur -- compiles cleanly, no runtime check emitted
(defn double-pos [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }
  (* x 2))

;; refine/unproved.tur -- emits TUR-E0371 with counterexample
(defn wrong [x : int] : #refine{ r : int | (> r 0) }
  x)

;; refine/float.tur -- float predicate discharged via QF_UFLRA
(defn sqrt-pos [x : #refine{ v : double | (>= v 0.0) }] : double
  (sqrt x))
```

- In an oracle build (`-DTUR_REFINE_Z3_ORACLE=ON`, system Z3 present) all three
  behave correctly, establishing the end-to-end pipeline.
- In a normal build at the RT3 milestone (no S-stage yet), the same fixtures
  compile with the obligations falling to runtime checks -- and are re-run,
  expecting static discharge, as each S-stage lands. These fixtures carry a
  `requires` marker so the suite knows which build decides them statically.

---

## In-house Solver: Staged Gradient (S0--S4)

Each stage is independently shippable, prepends itself to the discharge chain,
and returns `RT_UNKNOWN` for anything outside its competence. None of them can
regress correctness (the soundness invariant + `Unknown` exit guarantee it);
each only *moves obligations left*, off the runtime-check fallback (and, while it
still exists, off the Z3 scaffold). Build them in order, stop wherever the
coverage/effort tradeoff says stop. These stages *are* the shipped solver -- Z3
is never in the chain a user runs.

Every stage lands validated against the **Z3 scaffold oracle** (see "Backend
strategy"): in an oracle build, run the stage *and* Z3 on the same VC and assert
agreement (a stage saying `Valid` where Z3 says `Invalid` is a soundness bug ->
`TUR-I0379`, treated as `Unknown`). Fuzz against generated VCs + the labelled
`QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` SMT-LIB benchmark corpora. The oracle is a
development harness only; it is not present in the builds these stages ship in.

### S0 -- normalize + trivial discharge

**Effort: days.** Reflexivity (`p => p`), constant folding, syntactic
entailment, interval bookkeeping over literal bounds. No real solver. Discharges
a startling fraction of demo-grade obligations (`b != 0` after a literal guard,
`0 <= 3`, a goal syntactically present among the hypotheses). Every later stage
runs on S0's normalized output.

### S1 -- congruence closure (EUF)

**Effort: 1--2 weeks.** Union-find + congruence closure decides quantifier-free
equality with uninterpreted functions -- exactly where every measure-as-opaque-
function (`len`, `elems`) and every nonlinear-as-uninterpreted term lives.
Cleanest textbook treatment: Harrison, *Handbook of Practical Logic and
Automated Reasoning* (working OCaml, mechanically portable to C); Bradley &
Manna, *The Calculus of Computation* for exposition.

### S2 -- arithmetic, easy half first

**Effort: days (difference logic) + weeks (full LRA).** Two sub-steps:

- **S2a difference logic** (`x - y <= c`): negative-cycle detection
  (Bellman-Ford) on a constraint graph. Days of work; covers most index/bounds
  reasoning (`0 <= i`, `i < len`, `i + 1 <= n`) -- the sweet spot for array-style
  refinements and the direct target for `SizedVec` indices.
- **S2b full LRA**: the Dutertre--de Moura simplex formulation specifically,
  because it is designed to be incremental and backtrackable (needed once
  theories combine in S3). A few weeks to get pivoting + bound bookkeeping right.
- **S2c integers, the long tail**: branch-and-bound / Gomory cuts on top of LRA.
  This is the first place to feel *zero shame* returning `Unknown` with a depth
  limit -- integer completeness is genuinely hard and the runtime fallback
  absorbs it. If real completeness on the integer-linear fragment is ever wanted,
  the Omega test (Pugh) and Cooper's algorithm are the references (Omega was
  built for exactly the array-index constraints refinements produce).

### S3 -- combine EUF + LRA (Nelson-Oppen)

**Effort: 1--2 weeks once S1 + S2 exist.** The theories exchange entailed
equalities. EUF and LRA are convex, so they combine deterministically; the
wrinkle is integers (non-convex), which forces case-splitting on equalities.

### S4 -- boolean structure

**Effort: days for the naive version; do not build more.** Only needed if VCs
have nontrivial propositional shape (disjunctions, nested `ite`). Naive
case-splitting / small-DNF over the theory solver is usually enough for
explicit-annotation checking. A real CDCL SAT engine driving the theories
(true DPLL(T)) is the **months-to-years line we do not cross** -- if a VC's
boolean structure is too rich for small-DNF, return `Unknown` -> runtime check.

### Where to stop

S0--S2a alone (a couple weeks) already covers the demo-grade and array-index
majority and is small enough to live in `tur.wasm`. S0--S3 is the honest
"complete on the permitted fragment minus the integer tail" target and is the
point at which the Z3 scaffold is retired (see below). S4 and S2c are the
diminishing-returns tail -- do them only if real obligations demand it, and lean
on the **runtime fallback** for the rest without apology. (Note the fallback is
now the runtime check alone: past retirement there is no Z3 to lean on, and that
is the point -- the shipped compiler answers `Unknown` and inserts a check
rather than pulling in a solver.)

### Z3 retirement criteria

The scaffold is deleted -- `refine_libz3.c`, the `find_package(Z3)` block, the
`TUR_REFINE_Z3_ORACLE` option, and every `#ifdef TUR_REFINE_Z3_ORACLE` -- once
all of the following hold:

- **Bootstrap discharged:** every RT3/RT5b fixture that Z3 statically decided is
  now statically decided by the in-house chain (reached incrementally; fully by
  end of S3).
- **Oracle trust established:** the differential run over generated VCs + the
  `QF_UF`/`QF_IDL`/`QF_LIA`/`QF_LRA` SMT-LIB corpora has been clean across a
  soak window, and those corpora are checked into the repo as a standing
  regression the in-house solver runs against *without* Z3 present (labels come
  from the corpus, not a live Z3).
- **No scaffold references remain in shippable code paths** (guaranteed by
  construction, since the CMake option refuses Release/WASM, but re-verified at
  deletion).

After deletion the differential harness keeps running against the checked-in
labelled corpora -- that is the durable soundness net; Z3-the-dependency was
only ever the bootstrap that seeded it.

---

## Phase RT4: Bidirectional Predicate Propagation

**Goal:** Implement `refine_propagate.c` to infer *result* refinements from
*argument* refinements for simple arithmetic, reducing annotation burden. This
is a small, template-based convenience -- **not** the general refinement
inference we explicitly ruled out.

Without it:

```turmeric
(defn inc-pos [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }
  (+ x 1))
```

With it, the result refinement `(> r 0)` is inferred from the argument
refinement and the body, because the solver proves `(> x 0) => (> (+ x 1) 0)`.

### Approach: template-based (Liquid Types, checking-only)

A fixed set of predicate *shapes* is tried against the return expression, and
the seam checks which are implied by the argument refinements. Templates:

1. Predicates appearing on the function's parameters.
2. A small built-in vocabulary: `(> r 0)`, `(>= r 0)`, `(< r 0)`, `(<= r 0)`,
   `(!= r 0)`, `(= r <literal>)`.

First provable template wins; if none is provable and no explicit return
refinement was written, the return carries no refinement (same as today).
Conservative (may miss some) but sound (never infers a false refinement).

### Scope

- Single-expression bodies (no branching).
- Arithmetic over direct parameters.
- <= 4 refined parameters (`TUR-W0377` beyond that).

Branching bodies (path-sensitive join at merge points) are deferred.

### Acceptance Criteria

```turmeric
(defn inc-pos [x : #refine{ v : int | (> v 0) }] : int
  (+ x 1))                       ;; inferred result (> r 0)

(defn double-nonzero [x : #refine{ v : int | (!= v 0) }] : int
  (* x 2))                       ;; inferred result (!= r 0)
```

Both compile without `TUR-E0371`, with inferred refinements shown by
`tur --print-types`.

---

## Phase RT5a: WASM Integration

**Goal:** Static refinement discharge in the web REPL. Because Z3 is scaffolding,
this phase is almost entirely a *non*-integration: the **in-house solver
(S0--S2) compiles directly into `tur.wasm`** and is the entire discharge path.
There is no `z3-solver`, no JS bridge, no lazy fetch, no `SharedArrayBuffer` /
`Atomics.wait`, no COOP/COEP requirement, and no bundle-size cost. Whatever the
in-house chain returns `Unknown` on falls to a runtime check, exactly as in a
native build.

### Architecture

```
Turmeric WASM module (C/Emscripten)
  -> refine_discharge_all()
       -> [S0..S2 backends, compiled in]   (the whole chain; no network, no bridge)
       -> RT_UNKNOWN => runtime check
```

The web build is simply a normal (non-oracle) build targeting Emscripten. The
CMake option from RT3 already *refuses* `TUR_REFINE_Z3_ORACLE` under
`EMSCRIPTEN`, so there is structurally no way for Z3 to enter the WASM artifact.

### Changes

- None specific to a solver bridge. The only work is confirming the S0--S2
  sources compile clean under Emscripten (no host-only syscalls, arena sizing
  sane for the WASM heap) and that `refine_discharge_all` is reachable from the
  existing `wasm_glue.c` entry points.

### Acceptance Criteria

- The web REPL discharges `refine/proved.tur` statically with no network
  activity whatsoever; `refine/unproved.tur` surfaces `TUR-E0371`.
- The WASM bundle size delta from enabling `refined` is just the S0--S2 object
  code (target: small, single-digit KB), with **zero** third-party solver
  payload.
- No COOP/COEP headers are required for refinement checking (they may still be
  present for unrelated Emscripten threading, but discharge does not depend on
  them).

---

## Phase RT5b: Standard Library Refinement Types

**Goal:** `stdlib/refine.tur` with common predicate-annotated aliases. Can
proceed in parallel with RT5a once RT3 lands.

```turmeric
;;; Nat -- non-negative integer (>= 0)
(deftype Nat #refine{ x : int | (>= x 0) })

;;; Pos -- strictly positive integer (> 0)
(deftype Pos #refine{ x : int | (> x 0) })

;;; NonZero -- integer that is not zero
(deftype NonZero #refine{ x : int | (!= x 0) })

;;; Bounded lo hi -- integer in [lo, hi]
(deftype (Bounded lo hi) #refine{ x : int | (and (>= x lo) (<= x hi)) })

;;; PosFloat -- non-negative float
(deftype PosFloat #refine{ x : double | (>= x 0.0) })
```

`stdlib/refine-vec.tur` wires the `(< i n)` index obligation through
`SizedVec` -- the direct target of S2a difference logic:

```turmeric
;;; refine-vec/get -- index a SizedVec with a statically-bounded index
(defn refine-vec/get [v : (SizedVec n a), i : #refine{ j : int | (< j n) }] : a
  (vec-unsafe-get v i))
```

`vec-unsafe-get` is emitted only when `(< j n)` is proved statically; otherwise
the elaborator falls back to bounds-checked `vec-get`.

### Acceptance Criteria

```turmeric
(import refine)

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))                       ;; no runtime divisor check emitted

(defn main [] : void
  (println (safe-div 10 2)))
```

`d : NonZero` gives `(!= d 0)`; S0 proves the obligation with no Z3 call at all.

---

## Phase RT6: Error Message Quality

**Goal:** Make refinement errors actionable by translating the counterexample
`RefineModel` (in normalized-VC variable terms) back into source syntax.

```
error[TUR-E0371]: refinement predicate cannot be proved statically
  --> src/myfile.tur:14:3
   |
14 |   (defn inc [x : int] : #refine{ r : int | (> r 0) }
   |                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   |
   = counterexample: x = 0, so (+ x 1) = 1 is fine, but x may be <= -1,
     giving (+ x 1) <= 0
   note: the predicate (> r 0) is not always satisfied for arbitrary int x
   hint: constrain the parameter, e.g. x : #refine{ v : int | (>= v 0) }
```

The hint is derived by a second seam query: which additional predicate on the
input would make the goal valid (`(=> <candidate> <goal>)`). The
uninterpreted-symbol origin map (RT2) lets measure/nonlinear terms print as
their source expressions rather than `uf_3(...)`.

### Acceptance Criteria

- `tests/fixtures/refine/error-messages/` holds snapshots for 5 representative
  failing obligations, diffed by `tur run test` (session-types snapshot
  mechanism).

---

## Phase RT7: Incremental Discharge Caching (Follow-up)

**Goal:** Skip re-deciding obligations on unchanged files. Each obligation is
keyed by a hash of its **normalized VC** (captures predicate + hypotheses fully)
plus the compiler version. Results cache in `.tur-cache/refine.db`; a matching
hash skips the whole chain. Invalidation is per-file (conservative, safe). The
cache is always safe to delete.

### Acceptance Criteria

- A second build with no source changes runs zero backend decisions
  (`TUR_REFINE_STATS=1`).
- Editing a file evicts exactly that file's obligations.
- Deleting the cache falls back to full discharge transparently.

---

## Implementation Order and Dependencies

```
RT0  (experiment row + lang layer + discharge bit + pipeline hook)
 |
RT1  (constraint collector)      RT2  (normalized VC + solver seam + SMT-LIB serializer)
 |                                |
 +--------------------------------+
 |
RT3  (discharge pass + Z3 SCAFFOLD)  <-- pipeline testable end-to-end (dev/oracle build)
 |
 +-- S0 -> S1 -> S2a -> S2b -> S3 ==[Z3 SCAFFOLD RETIRED]==> (S2c / S4 tail)
 |        (in-house stages prepend to the chain; each moves obligations off
 |         the scaffold/runtime-fallback; by end of S3 Z3 is deleted)
 |
RT4  (predicate propagation)     RT5a (WASM: in-house in-bundle; no Z3, no bridge)
 |                                |
 |                               RT5b (stdlib types)
 +----------------+---------------+
                  |
                 RT6 (error message quality)
                  |
                 RT7 (incremental caching; follow-up)
```

RT3 stands the pipeline up with the Z3 *scaffold* (dev/oracle builds only);
normal builds discharge only what the in-house chain can. S0--S4 grow behind the
seam on their own schedule, each independently landable, each moving obligations
off the scaffold/runtime-fallback onto a faster dependency-free path. The Z3
scaffold is deleted once S0--S3 meet the retirement criteria. RT5a/RT5b
parallelize after RT3. S2c, S4, and RT7 are deferrable indefinitely.

---

## Effort Estimates

| Phase | Estimated Effort | Notes |
|---|---|---|
| RT0 | 1 day | Experiment row, lang layer, bit field, pipeline hook |
| RT1 | 2 days | Collector; env chain; crossing-point detection |
| RT2 | 3 days | Normalized VC + seam; SMT-LIB serializer; unit tests |
| RT3 | 2 days | Discharge chain; Z3 scaffold backend (system Z3, dev-only); diagnostics |
| S0 | 2--3 days | Normalize + trivial discharge |
| S1 | 1--2 weeks | Congruence closure (EUF) |
| S2a | 3--5 days | Difference logic (Bellman-Ford) |
| S2b | 2--4 weeks | Simplex LRA (Dutertre--de Moura) |
| S3 | 1--2 weeks | Nelson-Oppen combine EUF + LRA |
| S2c | (tail) | Integer branch-and-bound; depth-limited Unknown |
| S4 | (tail) | Small-DNF boolean structure; no DPLL(T) |
| RT4 | 3 days | Template propagation |
| RT5a | 0.5 day | WASM: confirm S0--S2 compile under Emscripten; no bridge to build |
| RT5b | 1.5 days | `refine.tur`; `refine-vec.tur`; integration tests |
| RT6 | 2 days | Counterexample translation; hint generation |
| RT7 | 2 days | Incremental caching (follow-up) |
| Z3 removal | 0.5 day | Delete scaffold backend, `find_package`, CMake option (post-S3) |

RT0--RT3 (~8 days) stands up the end-to-end pipeline validated against the Z3
scaffold. The in-house gradient (S0--S3) is where the multi-week investment
lives *and* is what the shipped product runs on -- it is not optional, because
Z3 leaves. What *is* optional to correctness is the S2c/S4 tail: past those, an
undecided obligation is answered `Unknown` and gets a runtime check, which is
always sound.

---

## Open Questions

1. **Z3 scaffold version.** The oracle uses whatever system Z3 the developer has,
   requiring >= 4.12 (`find_package(Z3 4.12 CONFIG REQUIRED)`); an older one
   makes the oracle option unavailable rather than silently degrading. There is
   no version to *pin in a release*, because no release ships Z3.

2. **Retirement timing vs. coverage confidence.** The scaffold is deleted once
   S0--S3 meet the retirement criteria, but "oracle trust established" is a
   judgement call on soak duration and corpus breadth. Open question: fix a
   concrete bar (e.g. N clean days over the full SMT-LIB QF_UF/QF_IDL/QF_LIA/
   QF_LRA corpora + M generated VCs) before deletion, versus deleting on the
   bootstrap criterion alone and keeping the oracle option a bit longer as a
   belt-and-suspenders dev aid.

3. **In-house solver arena discipline.** S1--S3 allocate union-find nodes,
   simplex tableaux, and equality-propagation queues per obligation. Decide
   whether each obligation gets a fresh arena (simple, leak-clean under the
   compiler's ASan/LSan policy) or the solver state resets in place per query.
   Fresh-arena-per-obligation is the default unless profiling says otherwise.

4. **Oracle-build isolation.** Running both the in-house stage and the Z3
   scaffold on every obligation is confined to `TUR_REFINE_Z3_ORACLE` dev
   builds; the CMake `FATAL_ERROR` guard already forbids the option under
   Release/WASM. Re-verify at each release cut that no shipped artifact links or
   references Z3 (a `grep` for `z3` in the release build's link line belongs in
   the release-cut checklist until the scaffold is deleted).

5. **`:post` with mutable references.** Allow `:post` on functions taking
   `&mut T`, but reject any predicate whose free variables include an `&mut`
   parameter name (`TUR-E0378`). Free-variable check on the predicate `Form*`;
   no alias analysis.

6. **Interaction with typeclasses.** Reject refined types on typeclass method
   signatures in the prototype (`TUR-E0376`). Per-instance discharge deferred.

---

## References

- **Simplify: A Theorem Prover for Program Checking** -- Detlefs, Nelson, Saxe.
  The blueprint for the in-house architecture (congruence closure + simplex +
  Nelson-Oppen + matching), from the pre-Z3 ESC/Java era. Read this before
  writing a line of S0--S4.
- **Handbook of Practical Logic and Automated Reasoning** -- Harrison. Working
  code for congruence closure (S1) and the arithmetic decision procedures (S2).
- **The Calculus of Computation** -- Bradley & Manna. Cleanest exposition of
  EUF, LRA, and Nelson-Oppen combination (S1--S3).
- **A Fast Linear-Arithmetic Solver for DPLL(T)** -- Dutertre & de Moura. The
  incremental, backtrackable simplex formulation used in S2b.
- **Simplification by Cooperating Decision Procedures** -- Nelson & Oppen. The
  theory-combination method used in S3.
- **The Omega Test** -- Pugh; **Cooper's algorithm** -- for the S2c integer tail
  if real completeness is ever wanted.
- **Liquid Types** -- Rondon, Kawaguchi, Jhala (PLDI 2008). The template-based
  *checking* convenience in RT4 (we deliberately skip their *inference* layer).
- **Turmeric Contract Types** -- `docs/guides/contract-types-guide.md`
- **Turmeric Sized Types** -- `docs/guides/sized-types-guide.md`
- **Lang layers** -- `docs/upcoming/lang-layers-plan.md` (the `refined` semantic
  layer, phase L4).
- **Experiment mechanism** -- `docs/upcoming/v1/experimental-flag-mechanism-plan.md`.
