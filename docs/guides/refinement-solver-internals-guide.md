# The Refinement Solver (Internals)

> **Audience:** compiler developers and anyone who wants to know *how* the
> compiler proves a refinement predicate at compile time. For the user-facing
> feature -- how to write refinements and read their diagnostics -- see
> [refinement-types-guide.md](refinement-types-guide.md). For the always-on
> runtime half see [contract-types-guide.md](contract-types-guide.md). The
> design of record is
> [../upcoming/v1/refinement-types-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/v1/refinement-types-plan.md).

A contract type `#refine{ x : T | p }` always has a runtime meaning: `p` is
checked when a value crosses into the type. On top of that, the compiler
always *tries to prove* `p` statically and elides the runtime check wherever
the proof succeeds. The prover is a small, staged,
in-house decision procedure that ships inside `tur` -- there is **no SMT
library dependency in any default or release build**, which is also why the
WASM playground gets static checking at zero download cost.

The one fact that shapes every design decision below:

> **Every obligation already has a sound runtime fallback.** So the static
> prover is allowed to give up on any obligation and stay correct -- the
> obligation just keeps the runtime check it would have had anyway. That is why
> a partial, hand-rolled solver is a real feature and not a broken one, and why
> turning `refined` on can never turn a correct program into a wrong one.

---

## Source map

Everything lives under `src/compiler/`, split by pipeline phase (RT1..RT7 are
the plan's phase names):

| File | Phase | Role |
|---|---|---|
| `refine_collect.{c,h}` | RT1 | Collect obligations at crossing points; encode `Form` -> normalized VC |
| `refine_vc.{c,h}` | RT2 | The normalized VC data structure, hash-consing, sorts, the backend seam |
| `refine_smtlib.{c,h}` | RT2 | Serialize a VC to SMT-LIB2 (`TUR_REFINE_DUMP=1` diagnostics) |
| `refine_solver.{c,h}` | RT3 | Shared machinery: NNF, DNF cube expansion, bounded model search |
| `refine_solver_s0.c` | RT3 | **S0** -- normalize + trivial/syntactic discharge |
| `refine_solver_euf.c` | RT3 | **S1** -- congruence closure (EUF) |
| `refine_solver_arith.c` | RT3 | **S2** -- linear arithmetic (Fourier-Motzkin) |
| `refine_solver_no.c` | RT3 | **S3** -- Nelson-Oppen combination of S1 + S2 |
| `refine_discharge.{c,h}` | RT3/RT6/RT7 | The backend chain, memoization, hint generation, diagnostics |

The elaborator wires it in from `elab_fns.c`, `elab_call.c`,
`elab_typeclasses.c`, and friends; the runtime-check codegen lives in
`emit_fns.c`. Diagnostic codes are registered in `diag.c` / `diag.h`.

---

## The pipeline

```
  source #refine{...}
        |
   RT1  |  refine_collect.c   -- find crossing points, build hypothesis env
        v
   [ RefineObligation ]       -- predicate + subject + env + location
        |
   RT2  |  refine_vc_build     -- Form -> normalized RefineVC (sorts, hash-cons)
        v
   [ RefineVC ]               -- vars, ufuncs, hyps[], goal
        |
   RT3  |  refine_discharge_one -> S0 -> S1 -> S2 -> S3 -> UNKNOWN
        v
   verdict: VALID  -> elide the runtime check
            INVALID-> TUR-E0371 with a counterexample
            UNKNOWN -> TUR-W0372, keep the runtime check
```

### RT1: where obligations come from

An obligation is created at each point a value crosses **into** a refined type.
`refine_collect.h` enumerates the crossing points collected today:

- a `defn` whose **return type** is a contract type (subject = the body form);
- a `defn` `:post` predicate (subject = the body, bound variable `result`);
- a **call-site argument** flowing into a contract-typed parameter (subject =
  the argument form; the callee's *other* parameter names are substituted by
  the actual arguments in their slots, so a predicate that mentions a sibling
  parameter is checked against real values).

Hypotheses -- the facts assumed true at the crossing -- come from:

- each parameter declared with a contract type (`v` renamed to the parameter
  name), and
- the function's `:pre` predicate.

Two obligation flags matter for how a failure is reported (`RefineObligation`
in `refine_collect.h:114`):

- **`runtime_guarded`** -- set for call-site crossings. The callee validates
  its own parameters on entry, so an argument we merely *cannot prove* is the
  normal state of a partially annotated program, not an error. Such an
  obligation reports only when it is *definitely* wrong (a closed goal that
  evaluates false, e.g. `(safe-div 10 0)`) or under `--strict-refine`. A
  function's **own** return refinement is a claim it makes about itself, so an
  open counterexample there is a `TUR-E0371` error. The distinction is *who
  owes the proof*.
- **`speculative` / `path_probe`** -- probes used by template inference (RT4)
  and path splitting; they are decided but report nothing and count nothing.

---

## RT2: the normalized verification condition

`refine_vc.h` defines the backend-independent structure every stage consumes. A
`RefineVC` is a set of sorted **variables**, a set of **uninterpreted function
symbols**, a list of **hypotheses**, and a single **goal**:

```c
typedef struct RefineVC {
    VCVar     *vars;    uint32_t n_vars,   cap_vars;   // sorted variables
    VCUFunc   *ufuncs;  uint32_t n_ufuncs, cap_ufuncs; // measures, nonlinear terms
    VCTerm   **hyps;    uint32_t n_hyps,   cap_hyps;    // assumed true
    VCTerm    *goal;                                    // to prove
    bool       has_real;       // any real -> QF_UFLRA rather than QF_UFLIA
    bool       has_nonlinear;  // a nonlinear subterm was abstracted
    ...
} RefineVC;               // refine_vc.h:92
```

### Sorts and operators

Three sorts (`VS_INT`, `VS_REAL`, `VS_BOOL`) and a compact operator set. The
term builder **normalizes as it interns**, so later stages have fewer cases to
handle (`refine_vc.h:44`):

```
  (> a b)     ==>  (< b a)
  (>= a b)    ==>  (<= b a)
  (not= a b)  ==>  (not (= a b))
```

No `VC_GT` / `VC_GE` term ever exists -- every backend handles three relations
(`VC_EQ`, `VC_LT`, `VC_LE`) instead of six.

### Hash-consing

Every term interns through an open-addressing hash table (`RefineVC.htab`), and
constant subterms are folded at construction time: `vc_mk(VC_ADD, [1, 2])`
returns the interned `3`. Structurally equal terms share one `VCTerm*`, so
**`a == b` (pointer equality) is structural equality** (`refine_vc.h:70`). S0's
syntactic checks and S1's congruence closure both lean on this directly.

### Uninterpreted functions, measures, and purity

Two kinds of subterm cannot be reasoned about arithmetically, so RT2 abstracts
them to **uninterpreted function applications** (`VCUFunc`):

1. **Named measures** -- a call to a user function like `size-of` or `len`. The
   solver never unfolds the body; it reasons about the symbol congruentially
   (`f(a) == f(a)`).
2. **Nonlinear terms** -- a `var*var` or `var/var` product the linear theory
   cannot see inside. Abstracting it sets `has_nonlinear` and drives the
   `TUR-W0373` warning.

Treating two occurrences of a call as *the same value* is only valid when the
callee is **pure**, and purity is **earned, not declared**
(`refine_collect.h:53`, fixture `refine-measure-euf`): the compiler walks the
callee's body against a default-deny whitelist (literals, immutable reads,
`if`/`let`/`do`, arithmetic builtins, calls to functions already known pure). A
non-empty `#fx{...}` row is a *veto* only. Anything unrecognized stays impure,
so a missed case costs one runtime check, never a wrong proof. An impure call
gets a **fresh** symbol per occurrence (`RefineVC.fresh_ctr`), which is why
`(- (tick) (tick))` cannot be folded to `t - t` and mis-proved `>= 0`.

### Identity: fingerprint + structural equality (RT7)

`refine_vc_fingerprint` hashes the *meaning* of a VC under a canonical
alpha-renaming (variables and symbols numbered by first occurrence), so
`x > 0 |- x+1 > 0` and `n > 0 |- n+1 > 0` land on the same memo key. A hash is
never trusted alone: a hit is confirmed with `refine_vc_equal`
(`refine_vc.h:142`).

---

## What the assertions look like

To prove `hyps |= goal`, the solver **refutes** `hyps AND (not goal)`. If that
formula is unsatisfiable, the goal is entailed. This is the shape every stage
runs over.

### As SMT-LIB2

`refine_smtlib.c` serializes the VC in exactly this refutation form. The dump
below (from `TUR_REFINE_DUMP=1`) is the obligation for the return value of
`double-pos`, `x > 0 |- 2x > 0`:

```smt
(set-logic QF_UFLIA)
(declare-const x Int)
(assert (< 0 x))            ; the hypothesis  x > 0, normalized
(assert (not (< 0 (* x 2)))) ; the negated goal
(check-sat)                 ; unsat  => goal is valid
```

The logic is `QF_UFLIA` (or `QF_UFLRA` when `has_real`) -- quantifier-free,
uninterpreted functions, linear integer/real arithmetic. Measures and
abstracted nonlinear products appear as plain `declare-fun` applications with no
special-casing, because RT2 already turned them into uninterpreted symbols.

### As cubes (what the in-house stages consume)

`refine_solver.c` puts the refutation formula into **negation normal form**
(De Morgan pushes negations to the leaves) and expands it to a small **DNF**: a
set of **cubes**, each a conjunction of literals. The goal is valid **iff every
cube is unsatisfiable** (`refine_solver.h:16`):

```c
typedef struct VCCube  { VCTerm **lits; uint32_t n; } VCCube;
typedef struct VCCubeSet {
    VCCube  *cubes; uint32_t n;
    bool     overflow;  // a cap was hit -> caller must answer RT_UNKNOWN
    bool     trivial;   // refutation folded to `false` outright -> valid
} VCCubeSet;
```

Two subtleties in the expansion (`refine_solver.c`):

- **A disequality is a disjunction.** `a != b` over a total order is
  `a < b OR b < a`. NNF rewrites a *negated* arithmetic equality into that
  disjunction (keeping the original literal alongside, so EUF still sees the
  disequality). Without it, an equality *goal* that needs any arithmetic --
  the most natural postcondition, `(= r <expr>)` -- would be unprovable, because
  a single linear constraint cannot encode `!=`.
- **Every step is capped.** Cube count (`REFINE_MAX_CUBES = 64`), literals per
  cube (`REFINE_MAX_CUBE_LITS = 64`), expansion depth, LA variables/constraints,
  EUF terms -- each cap, when hit, yields `RT_UNKNOWN` and the runtime check
  survives. This is the "naive S4" the plan sanctions: explicit annotations keep
  the propositional structure tiny, so small-DNF suffices and there is **no
  DPLL(T) engine** (`refine_solver.h:26`).

### A worked mixed example

The canonical assertion that needs both theories (fixture `refine-measure-euf`,
`refine_solver_no.c:4`):

```
hyps:  size-of(v) = n,   0 <= i,   i < n
goal:  i < size-of(v)
```

Linear arithmetic alone cannot see that `size-of(v)` and `n` denote the same
value; congruence closure alone cannot do the `i < n` ordering step. The
Nelson-Oppen combination (S3) exchanges the entailed equality `size-of(v) = n`
and closes it.

---

## RT3: the staged solver

`refine_discharge.c` owns an ordered array of backends and stops at the first
non-`UNKNOWN` verdict (`refine_discharge.c:56`):

```c
static const RefineBackend CHAIN[] = {
    refine_s0_decide,   // trivial / syntactic
    refine_s1_decide,   // congruence closure (EUF)
    refine_s2_decide,   // linear arithmetic
    refine_s3_decide,   // Nelson-Oppen combination
};
```

Backends run in **ascending cost**. Adding a stage only ever moves obligations
*left* in the chain; it never changes an answer, because every stage preserves
the soundness invariant. Each backend implements the `RefineBackend` seam and
returns one of three verdicts.

### The soundness invariant (one-directional, absolute)

```c
typedef enum RefineVerdict {
    RT_UNKNOWN = 0,  // the always-safe answer (zero value on purpose)
    RT_VALID,
    RT_INVALID,
} RefineVerdict;                                  // refine_vc.h:169
```

> A backend **may never** return `RT_VALID` for an obligation that is not
> genuinely entailed. `RT_UNKNOWN` is always permitted and always sound -- it
> falls back to the runtime check. `RT_INVALID` requires a real counterexample
> (see below), never a guess.

`RT_UNKNOWN` being the zero value means a zero-initialized decision is safe by
construction.

### S0 -- trivial / syntactic (`refine_solver_s0.c`)

No real reasoning. Reflexivity, constant folding (already done at intern time),
syntactic entailment (a goal that *is* one of the hypotheses -- pointer
equality thanks to hash-consing), and per-cube literal-level refutation:
a `false` literal, complementary literals `L` and `(not L)`, or an ordering
literal over identical terms (`x < x`). Discharges a surprising fraction of
demo-grade obligations -- `0 <= 3`, `b != 0` after a literal guard.

### S1 -- congruence closure / EUF (`refine_solver_euf.c`)

Union-find + congruence closure decides quantifier-free equality with
uninterpreted functions -- the theory where measures and abstracted products
live. Every non-leaf term is an application of its operator, so congruence
covers `(+ a b)` and `(len v)` uniformly. Three sources of contradiction:

1. a disequality `a != b` whose sides became congruent;
2. two distinct literals in one class (`3` and `5` can never be equal);
3. a positive atom and a negated atom that are congruent.

Naive `O(n^2)` fixpoint; real obligations carry a handful of terms.
`REFINE_MAX_EUF_TERMS` bounds the rest. Textbook treatment: Harrison; Bradley &
Manna (see References).

### S2 -- linear arithmetic (`refine_solver_arith.c`)

**Fourier-Motzkin elimination over exact rationals** (`int64` num/den, every
step overflow-checked). This is a strict superset of difference logic, so index
and bounds reasoning -- `0 <= i`, `i < len`, `i + 1 <= n` -- is covered, and it
decides conjunctions of linear constraints outright.

- **Integers:** rational unsatisfiability implies integer unsatisfiability, so
  every refutation is sound for `int`. A strict integer constraint is
  additionally *tightened* -- `e < 0` becomes `e <= -1` when every variable is
  int-sorted and every coefficient integral -- which is exactly what lets
  `x > 0 |= 2x > 0` go through. Full integer completeness (branch-and-bound /
  Omega) is deliberately not attempted.
- **Purification:** any term the linear encoder cannot see inside -- a `VC_VAR`,
  a measure, an already-abstracted nonlinear product -- becomes an opaque LA
  variable. This is the Nelson-Oppen purification step, and it is what lets S3
  exchange equalities with EUF over exactly these shared terms.
- Weakness is combinatorial blow-up, absorbed by `REFINE_MAX_LA_CONSTR` -> a cap
  hit degrades to a runtime check. If the cap ever bites real code, an
  incremental simplex (Dutertre--de Moura) slots in behind the same `la_*`
  interface.

### S3 -- Nelson-Oppen combination (`refine_solver_no.c`)

Neither theory alone decides a mixed cube. S3 runs both S1 and S2 over the same
cube and has them **exchange the equalities each entails over their shared
terms** (the purified opaque terms, which EUF also holds as nodes), iterating to
a fixpoint (`NO_MAX_ROUNDS = 4`, `NO_MAX_SHARED = 8`). EUF and LRA are both
convex, so this deterministic exchange is complete for them. Integers are
non-convex, which in general forces case-splitting on disjunctions of
equalities; S3 does **not** do that -- it reaches a fixpoint and, if neither
theory reports unsat, answers `RT_UNKNOWN`. Sound, incomplete on exactly the
integer tail the plan reserves for a future S2c.

### Counterexamples (`refine_model_search`, `refine_solver.c:298`)

Failing to *prove* proves nothing, so after the chain comes back `UNKNOWN` a
separate **bounded model search** tries to *refute* the goal. It picks a small
candidate value set -- the integer literals mentioned in the VC, their
immediate neighbours, and a handful of small integers around zero -- enumerates
assignments over an odometer, and **evaluates `hyps AND (not goal)` exactly**. A
satisfying assignment is a genuine counterexample, which is the only thing in
the whole solver allowed to answer `RT_INVALID`, and it does so *with a model*.

Scope is deliberately tiny: integer variables only, at most `MODEL_MAX_VARS = 3`
of them, and it **declines any VC carrying uninterpreted symbols** -- a measure
has no fixed interpretation to evaluate, so guessing one would be dishonest. The
important zero-variable case is a call site with literal arguments
(`(safe-div 10 0)`): the goal is closed, one evaluation decides it.

---

## Memoization and hints

### Within-unit memo (RT7)

`refine_discharge.c` keeps a per-process, open-addressed memo keyed on
`refine_vc_fingerprint`, confirmed with `refine_vc_equal` on a hit. It holds
only decided, non-speculative results. This matters because RT6 hint generation
issues *many* speculative queries; memoizing keeps that affordable.

### Hint generation (RT6)

"Cannot be proved" is only half an answer. The other half -- *which extra fact
would have discharged it* -- is a **second query through the same seam**: add a
candidate hypothesis and re-run the whole chain (`run_chain`,
`refine_discharge.c:139`). A candidate that works was *checked*, not guessed, so
the `help:` line is never a heuristic. Two families are tried:

- comparisons against the integer literals the code already mentions, and
- relations between two variables (this is what produces an index bound like
  `(< i n)`).

A candidate that *contradicts* what is already known is rejected explicitly --
otherwise a contradictory hypothesis would discharge the goal by ex falso and
the compiler would suggest constraining a variable to be both `< 0` and `> 0`.
When nothing consistent helps, no `help:` line is printed.

Example failure with all three parts (claim, witness, remedy):

```
error[TUR-E0371]: refinement on the return value of 'wrong' cannot be proved statically
note: the predicate (> r 0) does not hold for every input here
note: counterexample: x = -2
help: (> x 0) would discharge it -- e.g. declare x : #refine{ v : int | (> v 0) }
```

---

## Integration with the rest of the compiler

- **Elaboration.** The elaborator owns scope, so it supplies the
  `RefineFnResolver` callback (`refine_collect.h:78`) that lets the encoder look
  up a called function's return refinement and purity. A `NULL` resolver
  disables result propagation and purity discrimination -- every call falls back
  to the safe fresh-per-occurrence encoding. Obligations are created from
  `elab_fns.c` (returns / `:post`) and `elab_call.c` (arguments).
- **Effect system.** Purity for congruence is decided against the effect
  whitelist described above; the `#fx{...}` row is a veto. See the
  [effects-system-guide.md](effects-system-guide.md) for the effect rows
  themselves.
- **Typeclasses.** A refinement on a class method's signature is checked against
  each instance: an instance may not *strengthen* a method's precondition
  (`TUR-E0374`) and gets a leniency note where relevant (`TUR-W0377`). See
  fixtures `refine-typeclass-*` and `refine-class-*`, and the archived notes
  `docs/archive/class-param-refinement-not-demanded-of-callers.md`.
- **Codegen.** When an obligation is proven `RT_VALID`, `emit_fns.c` elides the
  runtime contract check it would otherwise inject; on `UNKNOWN`/`INVALID` the
  check is emitted exactly as the always-on contract path (see
  [contract-types-guide.md](contract-types-guide.md)) would.
- **No gate.** Static discharge is unconditional: every compile that sees a
  `#refine{...}` runs the pipeline described here. There is no `EXPERIMENTS[]`
  row and no global to read. The old opt-in spellings survive only as
  compatibility shims that age out one minor line after graduation --
  `--enable=refined`, `:experiments [:refined]`, and the user experiments file
  are accepted as no-ops (`TUR-W0063`, `GRADUATED[]` in
  `src/runtime/experiments.c`), and `#lang turmeric refined` is accepted as a
  no-op layer token (`TUR-W0064`, `GRADUATED_LAYERS[]` in
  `src/compiler/lang_layers.c`). Passing any of them changes nothing about
  what the solver does.
- **`--strict-refine`.** A diagnostic-strictness knob (not an experiment) that
  turns every `UNKNOWN`/open-`INVALID` obligation into a hard error, for builds
  that must discharge every refinement statically.

---

## Diagnostics

Registered in `diag.c` / `diag.h` (the `RT3` block, `diag.c:241`):

| Code | Kind | Meaning |
|---|---|---|
| `TUR-E0370` | error | refinement predicate is ill-typed (code allocated) |
| `TUR-E0371` | error | predicate genuinely does not hold (own claim falsifiable, or argument definitely wrong at its call site) |
| `TUR-W0372` | warn | nothing decided it; the runtime check is kept |
| `TUR-W0373` | warn | a nonlinear subterm was abstracted; arithmetic reasoning is incomplete for it |
| `TUR-E0374` | error | a typeclass instance's refinement is stronger than the method's |
| `TUR-E0375` | error | an effectful predicate where a pure one is required |
| `TUR-E0376` | error | refinement over a type parameter (a refinement alias with type parameters is rejected at `elab_types.c` with a plain diagnostic) |
| `TUR-W0377` | warn | instance-leniency note (paired with the class-method checks) |
| `TUR-E0378` | error | refinement appears inside a function type |
| `TUR-I0379` | internal | **RETIRED in 0.32.5**, never emitted: reported a Z3-oracle mismatch back when a dev build could link one. Code reserved, not reused |
| `TUR-W0063` | warn | a lingering `--enable=refined` / `:experiments [:refined]`; accepted, no effect |
| `TUR-W0064` | warn | a lingering `#lang turmeric refined` token; accepted, no effect |

`TUR-E0371` and `TUR-W0372` both leave the program safe -- the runtime check
survives either way. `tur explain TUR-W0372` prints the long form of any code.

---

## Caps reference

Every cap, when hit, degrades to `RT_UNKNOWN` -> runtime check
(`refine_solver.h:28`, `refine_solver.c:221`):

| Cap | Value | Guards |
|---|---|---|
| `REFINE_MAX_CUBES` | 64 | DNF cube count |
| `REFINE_MAX_CUBE_LITS` | 64 | literals per cube |
| `REFINE_MAX_EXPAND_DEPTH` | 256 | DNF product recursion backstop |
| `REFINE_MAX_LA_VARS` | 32 | linear-arithmetic variables |
| `REFINE_MAX_LA_CONSTR` | 512 | linear-arithmetic constraints |
| `REFINE_MAX_EUF_TERMS` | 512 | congruence-closure terms |
| `MODEL_MAX_VARS` | 3 | counterexample-search variables |
| `MODEL_MAX_CANDS` | 16 | counterexample candidate values |

---

## Debugging

```sh
# Per-unit stats: how many obligations were proven / refuted / unknown.
TUR_REFINE_STATS=1 tur build main.tur
# refine: 3 obligation(s): 2 proven, 0 refuted, 1 unknown (7 backend call(s))

# Dump each VC as SMT-LIB2 (the refutation form shown earlier).
TUR_REFINE_DUMP=1 tur emit-c main.tur

# Suppress static discharge entirely: every obligation declines, so nothing is
# elided and every refinement keeps its runtime check.
TUR_REFINE_NO_DISCHARGE=1 tur build main.tur
```

`TUR_REFINE_NO_DISCHARGE` is a **test seam, not a feature gate** -- env-only,
with no `--enable`, no `EXPERIMENTS[]` row and no CLI flag. It exists because
the source-level fuzzer needs a reference build in which every refinement keeps
its runtime check, which is the ground truth for "does this program actually
violate its own refinement". That is what the `refined` experiment gate
supplied until refinement types graduated in v0.33.0, and no shipping flag
reconstructs it: `--no-contracts` emits *no* checks, `--keep-contracts` emits
checks *minus* whatever discharge elided, and the elided set is precisely what
the fuzzer's miscompile property is about. It is implemented as one early
return in `refine_collect_obligation` -- the single chokepoint every obligation
flows through -- so nothing is collected, nothing is decided, and no refinement
diagnostic fires. Reach for it when you want to know whether a behavior you are
looking at came from discharge or was there anyway.

### The Z3 oracle -- RETIRED in 0.32.5

There used to be a dev-only Z3 backend (`refine_libz3.c`, behind a
`TUR_REFINE_Z3_ORACLE` CMake option) that ran after the in-house chain and
flagged any stage claiming `VALID` where Z3 said `INVALID`. It is **gone** --
file, option, `find_package` block, and the VC-level differential fuzzer that
depended on it. There is no way to link a solver into `tur` today, and nothing
to enable.

It was scaffolding with a defined end: a bootstrap while the in-house stages
were thin, and an oracle while their trustworthiness was still being
established. Both jobs finished (see `docs/upcoming/v1/refinement-types-plan.md`,
"Z3 retirement criteria"), and keeping a second solver around past that point
buys nothing while implying the shipped compiler has a solver dependency it
never had.

**What replaced it, and why the replacement is better:**

- `tests/corpus/smtlib/` + the `tur_refine_corpus` ctest target -- 125
  benchmarks whose `sat`/`unsat` labels are **data in the repo**, replayed
  against the in-house chain with no solver linked. A live oracle only worked
  on a machine that had Z3 installed; this runs in every build, including CI
  and WASM, which are exactly the ones that never had one.
- `tests/refine-fuzz-src.py` -- a **source-level** differential fuzzer that
  compiles generated programs twice (`TUR_REFINE_NO_DISCHARGE=1` as the
  reference leg, then a normal build) and compares what happens. It needs no
  oracle at all, and it covers the part that actually broke: both known
  soundness bugs lived in the source-to-VC encoder, *above* where the deleted
  VC-level fuzzer started, which is why ~17,300 generated VCs across six seeds
  stayed clean through both of them.

To debug a specific VC against an external solver by hand, dump it with
`TUR_REFINE_DUMP=1` and feed the SMT-LIB2 to whatever solver you like.

---

## Tests

Fixtures live in `tests/fixtures/refine-*` and `tests/fixtures/refined-*`
(plus `gadt-refine-*` for GADT interaction) -- ~45 directories exercising the
whole pipeline. Representative cases:

| Fixture | Exercises |
|---|---|
| `refine-proved` | S0/S2 -- `x > 0 |= 2x > 0`, check elided |
| `refine-measure-euf` | S1+S3 -- measure-as-uninterpreted, Nelson-Oppen |
| `refine-call-site` | argument crossings, sibling-parameter substitution |
| `refine-result-propagation` | using a callee's return refinement as a fact |
| `refine-disjunctive-goal` | DNF cube expansion on a disjunctive postcondition |
| `refine-equality-goal` | the disequality-as-disjunction NNF rewrite |
| `refine-cube-expansion-bounded` | cap-hit degrades to a kept runtime check |
| `refine-nonlinear-warn` | `TUR-W0373` on an abstracted product |
| `refine-match-impure-arm` | fresh-per-occurrence encoding for impure calls |
| `refine-runtime-check-still-fires` | an undischarged obligation keeps its runtime contract |
| `refine-graduated-enable-noop`, `refine-graduated-lang-layer-noop` | the `TUR-W0063` / `TUR-W0064` compatibility shims |
| `refined-bounded-idx`, `refined-nonempty` | index/non-empty bounds end to end |

Beyond the fixtures, `tests/refine-fuzz-src.py` fuzzes at the source level:
it generates whole programs and compares gate-off against gate-on behavior, so
its oracle is the runtime contract layer rather than a second solver. It
targets the soundness invariant -- the failure it hunts for is a program the
gate-off build catches and the gate-on build lets through.

(A second, VC-level fuzzer cross-checked the chain against Z3 until 0.32.5. It
went with the oracle; see "The Z3 oracle -- RETIRED in 0.32.5" above for why
that costs less coverage than it sounds like.)

Run the suite with the mandatory 12-minute timeout:

```sh
bash tests/run.sh 2>&1 | grep -E '^(FAIL|summary)'
```

---

## References

**In-tree**

- [refinement-types-guide.md](refinement-types-guide.md) -- user-facing feature guide.
- [contract-types-guide.md](contract-types-guide.md) -- the always-on runtime half.
- [../upcoming/v1/refinement-types-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/v1/refinement-types-plan.md) -- design of record; the RT1..RT7 and S0..S4 phase names used throughout this doc.
- [experimental-flags-guide.md](experimental-flags-guide.md), [effects-system-guide.md](effects-system-guide.md), [compiler-internals.md](compiler-internals.md).
- Archived decisions: `docs/archive/impure-refinement-predicates-accepted.md`, `docs/archive/class-param-refinement-not-demanded-of-callers.md`, `docs/archive/crossing-shadowed-binder-false-proof.md`.
- Source: `src/compiler/refine_*.{c,h}`, `src/runtime/experiments.c`, `src/compiler/diag.{c,h}`.

**External (the textbook procedures the stages implement)**

- J. Harrison, *Handbook of Practical Logic and Automated Reasoning*, Cambridge University Press, 2009 -- congruence closure, DNF/NNF, Nelson-Oppen.
- A. Bradley & Z. Manna, *The Calculus of Computation*, Springer, 2007 -- EUF, linear arithmetic, theory combination.
- G. Nelson & D. Oppen, "Simplification by Cooperating Decision Procedures", *ACM TOPLAS* 1(2), 1979 -- the combination method S3 implements.
- Fourier--Motzkin elimination (S2); B. Dutertre & L. de Moura, "A Fast Linear-Arithmetic Solver for DPLL(T)", *CAV* 2006 -- the simplex that would replace S2 if the cap ever bites.
- The SMT-LIB2 standard and the `QF_UFLIA` / `QF_UFLRA` logics (smt-lib.org).
