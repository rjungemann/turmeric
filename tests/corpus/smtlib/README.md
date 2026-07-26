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
- `unsupported_define_sort_skip.smt2` -- deliberately OUTSIDE the fragment
  (`define-sort` is an unsupported command). The reader must skip it whole
  rather than guess a translation; its presence keeps that path exercised, so
  a reader that silently mis-parsed would not report a pass for work it did
  not do. This role used to belong to `unsupported_ite_skip.smt2`, until ite
  lifting brought ite inside the fragment -- that file lives on as
  `qf_lia_ite_lifted_unsat.smt2`, pinning the lifting instead.

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

The record metadata for **SMT-LIB release 2025 (non-incremental benchmarks)**
(Zenodo record `16740866`) is committed at
[`tests/corpus/smt-lib-benchmark-data-2025.json`](../smt-lib-benchmark-data-2025.json).
It lists 90 per-logic tarballs -- 4.89 GB in total -- with sizes, md5 checksums
and content URLs. The metadata is committed; the tarballs are not.

The release is **CC-BY-4.0**, so a sample may be redistributed inside this repo
with attribution. `import-smtlib.py` writes an `ATTRIBUTION` file alongside
whatever it imports; leave it in place.

```sh
pip install zstandard
python3 tests/corpus/import-smtlib.py --list                     # what exists
python3 tests/corpus/import-smtlib.py --logics QF_UFLIA --dry-run
python3 tests/corpus/import-smtlib.py --logics QF_UFLIA,QF_UF --sample 25
```

It downloads to a scratch cache, **md5-verifies before extracting**, takes a
deterministic sample per logic (seeded, so an import is reproducible), and
flattens each logic into its own directory. It says how many it kept out of how
many exist, and says so explicitly when it kept fewer than asked -- a silent
shortfall is the same failure mode as a silently-skipped benchmark.

Sizes for the logics in the fragment, so a sample can be scoped:

| logic | compressed | logic | compressed |
|---|---|---|---|
| `QF_UFLIA` | 18.9 MB | `QF_UF` | 54.2 MB |
| `QF_UFIDL` | 35.6 MB | `QF_RDL` | 10.0 MB |
| `QF_LRA` | 181.5 MB | `QF_UFLRA` | 162.3 MB |
| `QF_IDL` | 427.4 MB | `QF_LIA` | 687.5 MB |

After importing, run `validate-labels.py` over the corpus. The library's own
`:status` labels are authoritative, but re-checking is how a truncated or
mis-imported file shows up as a disagreement rather than as a mysterious
soundness failure later.

### If the record's host is blocked: mirror it

`zenodo.org` is blocked by the egress policy of the container this corpus was
built in (the proxy answers 403 to CONNECT, recorded as `connect_rejected` for
`zenodo.org:443`). Two ways around that, neither of which needs the policy
changed.

**Simplest -- import once, commit the sample.** The corpus wants a bounded
sample, not the library: 25 benchmarks per logic across the eight fragment
logics is ~200 small files. Run the importer somewhere with ordinary network
access and commit what it produces. The corpus is then self-contained, which is
the whole point -- labels as data in the repo, no fetch step in the test path.

**Or serve the tarballs from a mirror.** S3 and `raw.githubusercontent.com` are
both reachable from this environment (verified: a public S3 object fetched
200/28 KB). Upload the per-logic tarballs under their own names and point the
importer at the bucket:

```sh
python3 tests/corpus/import-smtlib.py \
    --base-url https://YOUR-BUCKET.s3.amazonaws.com/smtlib \
    --logics QF_UFLIA,QF_UF --sample 25
```

The **md5 from the committed record is still enforced**, so a mirror can only
supply the same bytes the record describes -- it cannot substitute different
data, and a truncated or tampered copy is rejected before anything is
extracted. That is tested: a mirror serving same-length, different-content
bytes fails with a checksum mismatch, imports nothing, and exits non-zero.

Package registries are also reachable and were searched rather than assumed,
since they would be a viable transport for vendored data:

| source | result |
|---|---|
| PyPI (`pysmt` wheel + sdist, `cvc5`, `sudoku-smt-solvers`, full simple-index grep) | no benchmarks |
| npm (`smtlib`, `smtlib-ext`, `smtliblib`) | no `.smt2` at all |
| crates.io (`smt2parser`, `smtlib`, `smtlib-lowlevel`, `easy-smt`) | 38 `.smt2` -- all **standard logic declarations**, not benchmarks |
| `proxy.golang.org` | reachable, no module ships a corpus |

## The external corpus (not vendored here)

A 200-benchmark sample of the real library lives in its own repository:
**<https://github.com/rjungemann/smt-lib-benchmarks>** (25 per logic, seed 1,
from the 2025 release). It is deliberately NOT vendored into this tree.

```sh
git clone https://github.com/rjungemann/smt-lib-benchmarks /tmp/smtlib-bench
TUR_CORPUS_TIMEOUT=3 ./build/tur_refine_corpus /tmp/smtlib-bench/smtlib-2025
```

Why it stays out, in order of weight:

1. **It is 99 MB for 200 files** -- the real library is not small. One QF_LIA
   benchmark in that sample is over a million lines. That does not belong in
   the history of a compiler repo.
2. **Much of it is not decided.** These are competition-grade problems; the
   in-house chain answers `RT_UNKNOWN` on many and exceeds any sane time
   budget on others. A committed regression should be things that are actually
   decided, so a change in the answer means something. The committed corpus
   here is chosen on exactly that basis.
3. **Its value is as a SWEEP, not a gate.** Running it once found three real
   defects in this harness -- no time budget, block-buffered output, and let
   caps sized for hand-written input -- none of which the committed corpus
   could ever have surfaced. That is what it is for: run it when the reader or
   the solver changes, not on every build.

The committed corpus and the external one answer different questions. This one
asks "does a known-answer benchmark still get the known answer"; that one asks
"does anything in the wild break us".

### Reader coverage of the external sample

Last measured sweep (before logic-directed numeral typing landed): **193 of
200 parse**, 7 skips -- 4x "ite branches disagree on sort" (QF_LRA
`spider_benchmarks`, integer-literal ite branches in a Real logic), 1x "let
nested too deeply" and 1x "term nested too deeply" (QF_RDL), 1x "macro
expansion too deep" (QF_UFLRA); 142 decided, 40 over budget, no crashes.

The numeral-typing change (numerals in `QF_LRA`/`QF_RDL`/`QF_UFLRA` are
real-sorted, matching SMT-LIB) targets the four spider skips; the committed
regression pair is `qf_lra_ite_int_numerals_{unsat,sat}.smt2`. The post-fix
sweep has not been run yet -- rerun it where the external clone is available
and replace these numbers with measured ones; do not project.

## Running it

```sh
./build/tur_refine_corpus tests/corpus/smtlib     # or: ctest -R tur_refine_corpus
```
