# Corpus child crashes are silent under ASan; linearize overflows below the reader cap

**Severity:** medium (regression-net integrity, not compiler soundness).
Found 2026-07-26 while probing item 2 of
[docs/upcoming/v1/corpus-reader-tail-plan.md](../upcoming/v1/corpus-reader-tail-plan.md)
-- full measurements and fix directions live in that plan's "Item 2
findings" section; this note exists so the defects are not lost if that
plan is shelved.

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
