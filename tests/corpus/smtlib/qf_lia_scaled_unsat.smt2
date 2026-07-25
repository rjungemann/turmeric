; x > 0 forces 2x > 0, so 2x <= 0 cannot hold.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (> x 0))
(assert (<= (+ x x) 0))
(check-sat)
(exit)
