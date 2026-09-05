---
title: Solver Integer Tail Plan (S2c-lite)
category: Planning
description: Where the refinement solver's limits actually are once the numeric caps are measured (nowhere near), what shapes ordinary code writes that the solver could not decide, and the first slice of the S2c integer tail plus the encoder axioms that close them.
---

# Solver Integer Tail (S2c-lite)

**Status:** Phase 1 **LANDED 2026-09-05** (this document was written with
it).  Phases 2-4 are open, each with its own trigger.  Nothing here is on the
critical path to v1; every item is additive to a solver that already ships
and is sound, and every step below is an equivalence over the integers, so
the one-directional invariant (never `RT_VALID` unless entailed) is preserved
by construction.

Companion to [solver-extension-plan.md](solver-extension-plan.md) (SX), which
this plan does not replace: SX is about *incrementality and boolean
structure*; this is about *what the arithmetic stage can decide at all*.  SX7
("integer completeness, the long tail") is the SX phase this work belongs
under, and SX7's own note -- prototype in Turmeric first, port to C if the
stats move -- was not followed here, deliberately: the two steps that landed
are ~150 lines of exact int64 arithmetic in the existing C stage, cheaper to
write than to prototype.

## 1. The question, and the measurement that reframed it

The ask was to "expand the limits of the solver's constraints, and possibly
rebuild part of the solver infrastructure."  The word *limits* has two
readings in this solver, and they came apart on measurement.

### 1.1 The numeric caps do not bite

`refine_solver.h` carries nine caps.  `benchmarks/run-cap-sweep.sh` as it
stood before this work (the 2026-09-05 03:21 run of
`benchmarks/cap-sweep-results.md`, byte-identical to the 2026-08-26 baseline
except the header) across 124 corpus benchmarks, 85 in-tree refinement
fixtures and 200 fuzzer programs:

| cap | limit | worst peak anywhere | headroom |
|---|---:|---:|---:|
| `REFINE_MAX_CUBES` | 64 | 40 (a generated benchmark; 4 in real code) | 38% / 94% |
| `REFINE_MAX_CUBE_LITS` | 64 | 13 | 80% |
| `REFINE_MAX_EXPAND_DEPTH` | 256 | 8 | 97% |
| `REFINE_MAX_LA_VARS` | 32 | 9 | 72% |
| `REFINE_MAX_LA_CONSTR` | 512 | 42 | 92% |
| `REFINE_MAX_EUF_TERMS` | 512 | 512, one deliberate 1000-deep stress unit | -- |
| `NO_MAX_SHARED` | 16 | 9 | 44% |
| `RT_CS_PATH_MAX_HYPS` | 8 | 5 | 38% |

Zero hits on any cap on any real obligation.  Raising a number here buys
nothing measurable, and SX0(b) already parked the two phases (SX4 simplex,
SX6 lazy SMT) that a cap hit would have justified.

Regenerated after Phase 1 landed, the file moves in two places only: the
`model_vars` limit column reads 8, and the `la_constr` peaks DROP (corpus 42
-> 10, fuzzer 9 -> 6) because an equality now occupies one constraint rather
than two and is eliminated before Fourier-Motzkin can grow through it.  The
new `refine-int-divmod-by-literal` fixture becomes the in-tree worst unit for
cubes (8 of 64) and cube literals (11 of 64) -- the disjunctive sign axiom
doing exactly what 2.2 says it does, well inside the caps.

### 1.2 The corpus and the tree are already fully covered

- `tests/corpus/smtlib/`: **68 of 68** in-fragment `unsat` benchmarks are
  proved; the one `unsat` answered `unknown` is the deliberately
  out-of-fragment `define-sort` file.  There is no lost proof in the corpus
  for a stronger stage to find.
- In-tree: 89 refinement/GADT fixtures, **131 obligations, 115 proven, 16
  unknown -- and all 16 are fixtures that exist to pin an Unknown**
  (`refine-let-shadow-not-split`, `refine-nonlinear-warn`,
  `refine-match-field-wrong`, ...).
- Outside `tests/`, `#refine{` appears in exactly one file: the named
  refinements in `stdlib/refine.tur`.  There is no user population yet whose
  Unknowns could be counted.

So neither instrument the SX plan gates on could have shown a hole, and
neither did.  That is not the same as there being no hole.

### 1.3 Ordinary shapes the solver could not decide

Nine probes, each an ordinary function a user would write on day one.
Before / after this plan's Phase 1:

| probe | shape | before | after |
|---|---|---|---|
| p1 | `(= (mod n 2) 0) |- (= (mod (+ n 2) 2) 0)` | unknown | **proven** |
| p2 | `n >= 0 |- 0 <= (/ n 2) <= n` | unknown | **proven** |
| p3 | `2v = 2x + 1 |- false` (parity) | unknown | **proven** |
| p4a | 3-param function, false return refinement | refuted | refuted |
| p4b | **4-param** function, same false refinement | **unknown, silent** | **refuted, with witness** |
| p5 | `|- (>= (* x x) 0)` | unknown (`TUR-W0373`) | **proven** (still warns) |
| p7 | two disequalities on a bounded int | proven | proven |
| p8 | `abs` via `if` | proven | proven |
| p9 | `clamp` with three params | proven | proven |

Three of the four failures are one thing: **the solver treated `int` as
`real`**.  Fourier-Motzkin works over the rationals; `(mod n 2)` and
`(/ n 2)` were opaque terms; `2v = 2x + 1` has the rational solution
`v = x + 1/2`.  The grammar in the refinement-types guide has listed
`(/ e <literal>)` and `(mod e <literal>)` as legal predicate syntax the whole
time, so p1 and p2 are the feature as documented not working.  The fourth
(p4b) is the counterexample cap `MODEL_MAX_VARS = 3`, which the 2026-09-04
instrumentation commit had already flagged as "the cap most likely to
surprise" -- and whose sweep read clean only because the fuzzer generates at
most two parameters.

**Why the three swept populations were blind to all of it:** the fuzzer
(`tests/refine-fuzz-src.py`) generates `+ - *` by a constant and comparisons;
it never emits `/`, `mod`, a square, or more than two parameters.  The corpus
has no `div`/`mod` benchmark at all (`grep -l '(mod ' tests/corpus/smtlib
-r` finds nothing).  The in-tree fixtures were written against the solver
that existed.  A measurement can only report on the shapes its population
contains -- which is Phase 4 below.

## 2. Phase 1 -- LANDED 2026-09-05

Four changes, all in the existing stages, no new stage, no flag (every step
is sound by construction and the runtime check is unaffected either way, so
there is nothing to gate).

### 2.1 S2: the Omega test's two exact integer steps (`refine_solver_arith.c`)

- **Integer normalization** (`int_normalize`).  For a constraint whose every
  variable is int-sorted: clear the coefficient denominators, then
  `sum a_i x_i < b  ==>  <= ceil(b) - 1`, `<= b  ==>  <= floor(b)`, then
  divide by `g = gcd(a_i)` and floor the bound again.  Each step is an
  equivalence over the integers, so the constraint keeps exactly its integer
  models and the rational relaxation FM runs on gets tighter.  It subsumes
  the strict-tightening the file always had (`e < 0 -> e <= -1`) and adds
  the gcd step (`2q >= -1 -> q >= 0`).  Applied at assertion time and to
  every constraint FM derives.
- **Equality elimination** (`eq_eliminate`).  Equalities are now kept as
  equalities in `LinC` (`is_eq`) rather than split into two inequalities at
  assertion time.  Before FM, each one gets the **gcd divisibility test**
  (`sum a_i x_i = b` with `g` not dividing `b` has no integer solution -> the
  cube is unsat; this is parity), and then is **substituted away** through
  any unit-coefficient variable, so the constraints it lands in re-normalize
  and the test re-runs on what the substitution produced.  Mixed real/int
  equations substitute through any pivot (exact Gaussian step, no integer
  claim).  An all-integer equation with no unit coefficient (`2x + 3y = 1`)
  falls back to the two-inequality reading -- exactly as complete as before
  this phase existed.

`la_unsat` still works on a private copy, so `la_entails_eq`'s
truncate-and-retry discipline and the S3 exchange are unchanged.  A failed
gcd test leaves a canonical `0 < 0` in the set rather than a flag, so the
same truncation restores it.

### 2.2 Encoder: `(/ a k)` and `(mod a k)` axiomatized (`refine_collect.c`)

For an int-sorted `a` and a nonzero integer literal `k`, `enc_divmod_axioms`
adds two hypotheses about the (still opaque) terms `q = (/ a k)` and
`r = (mod a k)`:

```
a = k*q + r
(0 <= a  and  0 <= r <= |k|-1)  or  (a < 0  and  -(|k|-1) <= r <= 0)
```

These are the **truncating** semantics of C's `/` and `%`, which is what `/`
and `mod` on ints compile to (`builtins.c` maps `mod` to `%`; the
interpreter's `eval.c` uses `%`), so the axioms describe exactly the value
the runtime check would observe.  They are not SMT-LIB's floor/Euclidean
`div`/`mod`: the SMT-LIB reader behind `tur smt` does not go through this
encoder and receives no axioms, which keeps it sound for its own semantics
(the mismatch is pre-existing and is Phase 3(b) below).

The sign clause is a disjunction, so each axiomatized term doubles the cube
count.  After `ENC_MAX_DIVMOD_SPLITS = 4` terms in one VC the weaker
conjunctive bound `-(|k|-1) <= r <= |k|-1` is asserted instead, so an
obligation that used to prove *without* knowing anything about its `mod`
terms cannot be pushed over `REFINE_MAX_CUBES` by learning about them.

### 2.3 Encoder: squares are non-negative

`enc_nonlinear` still abstracts `(* a b)` to an uninterpreted symbol and
still flags `TUR-W0373`, but when `a == b` (structural equality, so
`(* (- x 1) (- x 1))` counts) it also asserts `(<= 0 (* a a))`.  A sign fact
about an opaque term, true for every integer and every real.  Only products:
`x / x` and `x mod x` share the abstraction path and have no such law.

### 2.4 Counterexample search: budget by evaluations, not width

`MODEL_MAX_VARS` 3 -> 8 (now only an array-size backstop) and a new
`MODEL_MAX_EVALS = 131072` on `n_cand ** n_vars`, which is the cost the
odometer actually pays.  A five-variable VC over five candidates (3125
evaluations) is cheaper than a three-variable one over sixteen (4096); the
old cap had that backwards.  Budget declines are counted on their own row
(`model evals out`) under `TUR_REFINE_STATS=1`; the `model vars` /
`model vars run` rows keep their meaning for the width backstop.

**Effect on the corpus:** 8 more `sat` benchmarks (16 of 56, up from 8) now
report a model instead of `unknown`; no `unsat` verdict moved.  On the fuzzer
the count of "gate-off ran clean, gate-on rejected at compile time" cases
(report-only by design, since `TUR-E0371` is universally quantified while
the program exercises one input) is 1 at the checked-in seed, and that one is
a correct refutation (`(* p 2) > 0` with `p >= 0`, witness `p = 0`).

### 2.5 Acceptance, as run

- `tests/unit/refine_solver.c`: 76 checks (was 61), including the
  soundness direction for every new step -- `2v = 2x + 1` over the reals is
  NOT refuted; `2x + 3y = 1` (integer-feasible) is NOT refuted;
  `2q >= -1` does NOT give `q >= 0` over the reals; the six-variable /
  eight-candidate VC declines on the budget row and not the width row.
- Corpus: 125 verdicts, the 8 `unknown -> sat` moves above and nothing else.
- `tests/run-refine-fuzz-src.sh`: 0 soundness bugs, 0 other bug classes.
- In-tree refinement fixtures: verdict counts identical before and after
  (115 proven / 16 unknown, the same 16).
- New fixtures: `refine-int-divmod-by-literal`, `refine-int-parity`,
  `refine-square-nonneg` (each with `--strict-refine` in `flags`, so an
  Unknown is a build failure and the fixture pins PROVEN, not merely
  "ran"), `errors/refine-model-search-four-vars`, and
  `refine-model-vars-cap` re-pointed at the new limits with a second subject
  for the budget row.
- Full `bash tests/run.sh`: 2810 passed, 0 failed (commit dee5563f); refine ctests pass; `benchmarks/run-cap-sweep.sh` regenerated against this compiler -- no cap moved except the `model_vars` limit column and the constraint count an equality now occupies.

## 3. Phase 2 -- the rest of the Omega equality phase (open)

**Trigger:** an obligation whose equation has no unit coefficient shows up
outside a test -- the telemetry to add first is a counter on the
`eq_eliminate` fallback branch (how many equalities were split rather than
eliminated), reported under `TUR_REFINE_STATS=1`.

- **Pugh's sigma-substitution** for `sum a_i x_i = b` with all `|a_i| > 1`:
  pick the smallest `|a_k|`, `m = |a_k| + 1`, introduce `sigma` with
  `a_k x_k = -m sigma + sum (a_i mod-hat m) x_i + ...`, which lowers the
  coefficient magnitudes each round and terminates.  Costs one fresh LA
  variable per round against `REFINE_MAX_LA_VARS` (peak 9 of 32).
- **Size:** ~60 lines in `eq_eliminate`.  Unit tests: `2x + 3y = 1`
  becomes eliminable (still satisfiable -- the test is that the pivot step
  does not manufacture a refutation), and `6x + 10y = 3` (gcd 2, already
  caught) stays caught.

## 4. Phase 3 -- inequality-side integer completeness (open)

Systems that are rationally feasible and integer-infeasible with no equation
in sight: `2x >= 1, 2x <= 1` normalizes to `x >= 1, x <= 0` and IS caught by
Phase 1, but `3x + 3y >= 1, 3x + 3y <= 2` is not (both normalize to bounds on
`x + y` that leave `[1/3, 2/3]`, then floor/ceil to `x + y >= 1, <= 0` -- also
caught; the real gap is multi-variable systems where the integer hull is not
reached by per-constraint rounding).

- **(a) Dark shadow / branch-and-bound**, depth-limited, `RT_UNKNOWN` past
  the limit.  This is what the archived plan and SX7 call S2c proper.
  **Trigger:** a real obligation, not a constructed one.  The probe set in
  1.3 did not produce one; ordinary bounds reasoning does not need it.
- **(b) `tur smt` `div`/`mod` semantics.**  The SMT-LIB reader translates
  `div`/`mod` to `VC_DIV`/`VC_MOD`, whose constant folding and model
  evaluation use C truncation, while SMT-LIB specifies floor division and a
  non-negative remainder.  No corpus benchmark uses either today.  Either
  translate to the truncating form with an explicit sign adjustment, or
  refuse `div`/`mod` in the reader as out-of-fragment until then.  Small,
  and worth doing before anyone adds a `QF_LIA` benchmark with `mod`.

## 5. Phase 4 -- make the instruments able to see this class (open)

The blind spot in 1.3 is the actionable finding for the measurement side,
and it is cheap:

- `tests/refine-fuzz-src.py`: generate `(/ e k)`, `(mod e k)`, `(* e e)`,
  and 3-5 parameters, at low weight.  Every shape in 1.3 then has a
  population, and `run-cap-sweep.sh`'s `model_vars` / `model evals` rows
  start meaning something.  The differential property (elision never turns
  a caught program into a silent one) is exactly the right check for the
  truncation axioms: a wrong sign in 2.2 would show up as a miscompile, not
  as a corpus label.
- `tests/corpus/smtlib/`: a handful of hand-written `QF_LIA` benchmarks with
  `div`/`mod` and parity, labelled by both reference solvers per the corpus
  README -- **after** Phase 3(b), or the labels will disagree with the
  reader's semantics.
- `benchmarks/run-cap-sweep.sh`: nothing to change; it already reports the
  new `model evals out` row as a plain count, the way it reports FM blow-ups.

## 6. Explicitly not doing

- **Not** interpreting `(* x y)`, `(/ x y)`, `(mod x y)` with both sides
  variable.  Nonlinear stays abstracted; the square axiom is the one sign
  law that costs nothing.
- **Not** removing any cap, and **not** raising the numeric caps on this
  evidence -- there is none.
- **Not** loop invariants.  A `while` accumulator is Unknown for a reason
  that has nothing to do with arithmetic; see
  [hold/loop-invariants-plan.md](hold/loop-invariants-plan.md).
- **Not** an incremental simplex.  The S2 rebuild here is the integer
  layer *on top of* FM, behind the same `la_*` seam; SX4 stays parked on
  its own gate.

## 7. References

- Pugh, *The Omega Test: a fast and practical integer programming algorithm
  for dependence analysis* (1991) -- normalization, the equality phase with
  the mod-hat trick, dark and grey shadows.
- Dutertre, de Moura, *A Fast Linear-Arithmetic Solver for DPLL(T)* (2006)
  -- the SX4 simplex this plan does not build.
- [docs/archive/refinement-types-plan.md](../archive/refinement-types-plan.md)
  S2a-S2c; [solver-extension-plan.md](solver-extension-plan.md) SX0(b), SX7.
