; Both disjuncts contradict x = 5.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (= x 5))
(assert (or (< x 0) (> x 10)))
(check-sat)
(exit)
