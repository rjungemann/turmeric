; Regression for logic-directed numeral typing (the spider_benchmarks shape):
; an ite whose branches are INTEGER literals in a Real logic.  In QF_LRA there
; are no Int-sorted terms, so `2` and `1` denote reals.  The inner ite lifts to
; a fresh variable; when numerals were carried as ints that variable came out
; VS_INT and the outer ite saw it opposite a real-sorted term -- skipped as
; "ite branches disagree on sort", which no literal rewrite could fix because
; a declared variable has no literal to rewrite.
; Unsat: x is one of {2, 1, 0.5}, all <= 2.5, yet x > 2.5 is asserted.
(set-logic QF_LRA)
(set-info :status unsat)
(declare-fun c () Bool)
(declare-fun d () Bool)
(declare-fun x () Real)
(assert (= x (ite d (ite c 2 1) 0.5)))
(assert (> x 2.5))
(check-sat)
(exit)
