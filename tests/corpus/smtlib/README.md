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

Every label agrees with Z3. Two scripts, both **development scaffolding** in
exactly the way `refine_libz3.c` is -- neither is built, linked, or run by the
test suite, and `tur_refine_corpus` does not import them:

```sh
pip install z3-solver
python3 tests/corpus/validate-labels.py          # every label vs z3
python3 tests/corpus/generate-corpus.py --out DIR --n 2000 --seed 7
```

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

External SMT-LIB benchmarks (the official QF_UF/QF_IDL/QF_LIA/QF_LRA
distributions) can be dropped into `generated/`'s sibling directories as-is:
they already carry `(set-info :status ...)`, which is the only thing the runner
needs. The reader skips whatever falls outside the fragment, so an import does
not have to be filtered by hand first.

## Running it

```sh
./build/tur_refine_corpus tests/corpus/smtlib     # or: ctest -R tur_refine_corpus
```
