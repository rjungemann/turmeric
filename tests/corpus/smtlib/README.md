# Labelled SMT-LIB2 corpus

A standing regression for the in-house staged decision procedure (S0--S3),
replayed by `tur_refine_corpus` with **no solver linked**.

This exists because of the Z3 retirement criteria in
[docs/upcoming/v1/refinement-types-plan.md](../../../docs/upcoming/v1/refinement-types-plan.md).
`refine_libz3.c` is a *live* cross-check that only exists on a dev build with a
system Z3; the moment it is deleted, that safety net goes with it. What has to
survive is a corpus whose labels are **data in the repo** -- which is what this
is.

## What is checked

SMT-LIB `:status` is a claim about the SATISFIABILITY of the assertion set. The
chain decides ENTAILMENT. The two line up by taking every `(assert phi)` as a
hypothesis and `false` as the goal:

```
hyps |- false   is VALID   iff   hyps is UNSAT
```

which gives the check its shape:

| `:status` | chain answers `RT_VALID`      | chain answers anything else |
|-----------|-------------------------------|-----------------------------|
| `unsat`   | correct -- it found the proof | acceptable (incomplete)     |
| `sat`     | **SOUNDNESS FAILURE**         | correct                     |

A `sat` benchmark answered VALID is the one-directional soundness invariant
broken: the chain claimed a contradiction in a constraint set that has a model.
That is the property the corpus defends, and checking it needs only the label --
never a live solver.

Incompleteness is never a failure. `RT_UNKNOWN` is always a safe answer, and a
stage that legitimately decides more later must not break this corpus.

## Layout

- `*.smt2` at the top level -- **hand-written**, one idea each, chosen to cover
  the fragment: QF_UF congruence (including at arity 2 and through
  transitivity), QF_IDL negative-weight cycles, QF_LIA integer-gap and scaling,
  QF_LRA strictness over the reals where the integer version is unsat,
  QF_UFLIA cases needing both theories to combine, and boolean structure
  (`or`, `=>`, `distinct`, `let`).
- `generated/` -- **machine-generated**, labelled by Z3, curated to 10 per
  (theory, status) bucket so the set stays balanced and reviewable.
- `unsupported_ite_skip.smt2` -- deliberately OUTSIDE the fragment. The reader
  must skip it whole rather than guess a translation; its presence keeps that
  path exercised, so a reader that silently mis-parsed would not report a pass
  for work it did not do.

## Provenance of the labels

Every label agrees with **both Z3 and cvc5**. Two scripts, both **development
scaffolding** in exactly the way `refine_libz3.c` is -- neither is built,
linked, or run by the test suite, and `tur_refine_corpus` does not import them:

```sh
pip install z3-solver          # required
pip install cvc5               # optional, recommended
python3 tests/corpus/validate-labels.py          # every label vs both
python3 tests/corpus/generate-corpus.py --out DIR --n 2000 --seed 7
```

Two solvers rather than one, because **Z3 is the thing being retired**. A label
confirmed only by Z3 inherits whatever Z3 gets wrong, and this corpus exists
precisely so the in-house chain can be trusted once Z3 is gone. cvc5 is a
different implementation lineage, so agreement between them is meaningfully
stronger than either alone. cvc5 is optional; without it the script still
checks Z3 and says which seal each label carries (`z3+cvc5` or `z3 only`).

`validate-labels.py` is the one that matters for review: it re-checks the
committed labels, including the generated ones, so a rendering bug in the
generator cannot quietly install a wrong label. A wrong label is worse than no
corpus:

- a benchmark wrongly labelled `sat` that is really unsat can never fail, so it
  silently stops testing anything;
- a benchmark wrongly labelled `unsat` that is really sat inverts the check --
  the harness would demand a proof of something false and read the correct
  refusal as weakness.

## Adding benchmarks

Prefer a hand-written benchmark with a comment saying *why* the status holds;
that comment is what a reviewer checks against, and it is what makes a wrong
label catchable without running anything. Run `validate-labels.py` before
committing.

Generated benchmarks are for breadth, and for the soak: generate thousands into
a scratch directory and replay them looking for a soundness failure. Only a
bounded, curated subset belongs in the repo -- the point is a regression net,
not a benchmark farm.

## Importing the SMT-LIB benchmark library

A note on names, because they are easy to conflate and this repo got it wrong
once. **"SMT-LIB" is two different things:**

1. **The standard** -- a specification: the SMT-LIB 2.6 language, the theory
   declarations (`Ints`, `Reals`, `ArraysEx`, ...), and the logic declarations
   (`QF_UF`, `QF_LIA`, `QF_NRA`, ...). These are reference *documents*. The
   `(set-info :status ...)` attribute this corpus depends on is defined here.
2. **The benchmark library** -- a separate, much larger *data* artifact: the
   collection of benchmark files, per logic, each carrying a `:status` label.
   This is the thing worth importing.

**SMT-COMP is neither.** It is the annual solver competition (run alongside the
SMT workshop). It *draws* its problems from the benchmark library and publishes
results and tooling; it is not itself a benchmark distribution. An earlier
version of this file said benchmarks could be fetched "from the SMT-COMP
archives", which was wrong.

The distinction has teeth. The crates.io search below found 38 `.smt2` files in
the Rust `smtlib` crate -- and every one is a **logic declaration from the
standard**:

```
(logic QF_NRA
 :smt-lib-version 2.6
 :written-by "Cesare Tinelli"
 ...
```

No assertions, no `:status`, nothing to solve. The crate vendored the
*reference*, which is what a parser needs and what a corpus does not.

### Getting the real thing

The benchmark library is released as versioned collections and mirrored as
per-logic repositories by the Iowa CLC group; recent releases are archived with
DOIs. Any of those routes works -- the harness needs nothing but files with
`(set-info :status ...)`.

They are not here because those hosts are unreachable from the development
container used to build this (proxy policy denies the CONNECT). Package
registries ARE reachable, and were searched rather than assumed, since they
would be a viable transport for vendored data:

| source | result |
|---|---|
| PyPI (`pysmt` wheel + sdist, `cvc5`, `sudoku-smt-solvers`, full simple-index grep) | no benchmarks |
| npm (`smtlib`, `smtlib-ext`, `smtliblib`) | no `.smt2` at all |
| crates.io (`smt2parser`, `smtlib`, `smtlib-lowlevel`, `easy-smt`) | 38 `.smt2` -- all **standard logic declarations**, not benchmarks |
| `proxy.golang.org` | reachable, no module ships a corpus |

So the registries are a viable transport, but the benchmark library is not
published through them. Anyone with ordinary network access can fetch it and
drop it into a subdirectory unfiltered: the reader takes `(set-info :status
...)` as the label, skips whatever falls outside the fragment rather than
guessing, and `run_dir` recurses. Nothing in the harness needs to change.

## Running it

```sh
./build/tur_refine_corpus tests/corpus/smtlib     # or: ctest -R tur_refine_corpus
```
