# The last 7 skips in the SMT-LIB corpus reader

**Status:** not started. Small, well-characterised, and **optional** -- read the
value section before picking this up, because the honest case for doing it is
weaker than the case for having done the first 73.

`tests/unit/refine_corpus.c` replays labelled SMT-LIB benchmarks against the
in-house chain with no solver linked (see
[the corpus README](../../../tests/corpus/smtlib/README.md)). Against the
200-benchmark external sample it now parses 193; this plan is about the
remaining 7.

## Where the 7 are

Measured on the sample at `github.com/rjungemann/smt-lib-benchmarks`:

| cause | count | logic |
|---|---|---|
| `ite` branches disagree on sort | 4 | QF_LRA (`spider_benchmarks`) |
| `let` nested too deeply | 1 | QF_RDL |
| term nested too deeply | 1 | QF_RDL |
| macro expansion too deep | 1 | QF_UFLRA |

## 1. The four `ite` sort mismatches -- the interesting one

These are QF_LRA benchmarks whose `ite` branches are integer literals:

```
(ite ?v_7 2 1)
```

In a Real logic there are no Int-sorted terms; `2` and `1` denote **reals**. The
reader carries numerals as integers and only coerces at the points it knows
about -- `/`, and `ite` branches where one side is already a real literal.
`tr_as_real` rewrites a `VC_CONST_INT` node, so it cannot fix the case that
actually occurs here: a **lifted `ite` variable** declared `VS_INT` sitting
opposite a real-sorted term. A declared variable has no literal to rewrite.

Two fixes, and the cheap one is also the principled one:

- **Type numerals by the declared logic.** In a pure-Real logic (`QF_LRA`,
  `QF_RDL`, `QF_UFLRA`), `tr_numeral` produces a real literal. That is what
  SMT-LIB means in those logics, it fixes all four, and it removes a whole class
  of latent coercion bugs rather than the four instances of it. The reader
  already records `set-logic`; it just ignores it.
- **Propagate an expected sort downward** through `tr_term`. General, handles
  mixed logics like `QF_UFLIA` where both sorts genuinely coexist, and is a
  much larger change to a file whose value is that it is simple enough to audit.

**Do the first.** The second is only warranted if a mixed-sort logic later turns
up the same problem, and none has.

Note this is the same class of bug as the real-division defect the corpus caught
(`(/ 1 2)` folding to 0, which proved a satisfiable benchmark contradictory).
That one was a soundness failure; this one is a skip. The difference is luck --
which is an argument for the logic-directed fix over another point patch.

## 2. The three depth caps -- mechanical

`TR_MAX_LET_DEPTH` (4000), `TR_MAX_TERM_DEPTH` (6000) and the macro-expansion
bound each stop one benchmark. All three are the same underlying limit:
`tr_term` recurses on the **C stack**, and a stack overflow in the forked child
is reported as `CRASH!` -- counted as a failure, which is loud but wrong for
what is really a reader limit.

Options, in increasing order of effort:

- **Raise the caps and measure.** Cheapest. The caps were already raised once
  (512 -> 4000, 2000 -> 6000) with no child crashing, so there is headroom, but
  "raise until it stops failing" is how you find the stack overflow the hard
  way. If this is the chosen route, raise them **with a deliberate probe** of
  where the child actually dies, so the new cap is set below a measured limit
  rather than above an unmeasured one.
- **Give the child a bigger stack** (`setrlimit(RLIMIT_STACK)` before `fork`,
  or run the translation on a thread with an explicit stack size). Moves the
  real limit rather than the guess at it.
- **Make `tr_term` iterative.** Removes the class entirely. Also removes the
  property that makes this reader reviewable in one sitting. Hard to justify for
  three benchmarks.

## Why this is optional

**The bottleneck is no longer the reader.** Of the 200 benchmarks, 193 parse and
189 carry a `:status`; 142 are decided and **40 exceed the time budget**.
Landing all seven of these would move parsing from 193 to 200 and decided from
142 to at most 149 -- and probably fewer, since the benchmarks that defeat the
reader are the large ones, which are exactly the ones that then time out in the
solver.

So the realistic yield is **a handful of benchmarks, several of which will
convert from "skipped" to "over budget"** rather than to "decided". That is not
nothing -- a skip and a timeout are different failures and the first is the
reader's fault -- but it is not coverage.

The case for doing it anyway is (1) the sort-mismatch fix is principled and
closes a bug class rather than four instances, and (2) skips are the category a
reader can actually fix, so leaving them is leaving the one thing under our
control. The case against is that solver throughput now dominates and effort
spent here does not touch it.

**Recommendation: do item 1, skip item 2.** The logic-directed numeral typing is
worth landing on its own merits. The three depth caps are one benchmark each,
and the honest fix for them is work out of proportion to the return.

## Acceptance criteria

- Numerals in `QF_LRA` / `QF_RDL` / `QF_UFLRA` are real-sorted; the four
  `spider_benchmarks` files parse.
- A committed regression pinning the shape -- an `ite` whose branches are
  integer literals in a Real logic -- with its label confirmed by both z3 and
  cvc5, as every corpus label is.
- No regression in the committed corpus, and no new `CRASH!` in the external
  sample.
- The skip tally in the corpus README updated with real numbers, not projected
  ones.
