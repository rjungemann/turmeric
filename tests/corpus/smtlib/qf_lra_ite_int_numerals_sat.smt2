; Sat twin of qf_lra_ite_int_numerals_unsat: same shape, weaker bound.
; Sat: with d and c both true, x = 2 > 1.5.  The chain must never prove this
; contradictory -- the soundness direction of the numeral-typing change.
(set-logic QF_LRA)
(set-info :status sat)
(declare-fun c () Bool)
(declare-fun d () Bool)
(declare-fun x () Real)
(assert (= x (ite d (ite c 2 1) 0.5)))
(assert (> x 1.5))
(check-sat)
(exit)
