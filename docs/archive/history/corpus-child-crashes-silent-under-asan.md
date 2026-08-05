# Corpus child crashes are silent under ASan; linearize overflows below the reader cap

**RESOLVED 2026-07-26**, same day as found: both fixes landed with the
item-2 sequence of
[docs/upcoming/v1/corpus-reader-tail-plan.md](corpus-reader-tail-plan.md).
Defect 1: the outcome enum moved to 40..46 and the parent classifies any
unexpected exit status as `CRASH!` (verified end-to-end: a 256k-deep probe
that ASan-kills the child in `sx_read` now reports `CRASH!` where it used
to tally "unlabelled"). Defect 2: `linearize` is depth-bounded at
`LA_MAX_LINEARIZE_DEPTH` (500) via its existing `bad`-constraint drop;
committed regression `tests/corpus/smtlib/qf_lra_deep_arith_chain_sat.smt2`
pins the exact shape that used to crash. Original report follows.

**Severity:** medium (regression-net integrity, not compiler soundness).
Found 2026-07-26 while probing item 2 of the corpus-reader-tail plan.

Two compounding defects in `tur_refine_corpus` (`tests/unit/refine_corpus.c`):

1. **A sanitizer-detected crash in the forked child counts as a pass.**
   ASan exits with code 1 on a deadly signal, which collides with
   `OUT_UNLABELLED`, so a child stack overflow tallies as "unlabelled"
   instead of `CRASH!` and the suite stays green. Only signal deaths are
   loud; in the Debug/ASan build (the one the suite runs) crashes are
   laundered. Fix (~5 lines): move the outcome enum to distinctive values
   (e.g. 40..46) and classify any unexpected exit status as a crash.

2. **`linearize` (refine_solver_arith.c:179) stack-overflows at ~1000-deep
   arithmetic terms** -- measured: survives 750, dies at 1000 (ASan build,
   8 MB stack) -- while the reader admits terms up to `TR_MAX_TERM_DEPTH`
   (6000). A legal, in-cap corpus file crashes the child today, and defect
   1 hides it. Cause: `LinExp` (~800 B) passed/returned by value, several
   copies per ASan-inflated frame. Fix (~10 lines): depth-bound
   `linearize` via its existing `bad` escape (a `bad` constraint is
   dropped, which only weakens refutations -- sound by construction),
   bound ~500.

Minimal repro (crashes the child; suite reports it as "unlabelled"):

```sh
python3 -c 'd=1000; print("(set-logic QF_RDL)(set-info :status sat)"
  "(declare-fun x () Real)(assert (< x " + "(+ 1 "*d + "x" + ")"*d + "))"
  "(check-sat)(exit)")' > /tmp/deep.smt2   # one line, depth 1000
mkdir -p /tmp/deep-corpus && mv /tmp/deep.smt2 /tmp/deep-corpus/
./build/tur_refine_corpus /tmp/deep-corpus   # ASan stack-overflow in linearize,
                                             # tallied "unlabelled", not CRASH!
```

Fix defect 1 first: it is what makes defect 2 (and anything like it)
visible in the suite.
