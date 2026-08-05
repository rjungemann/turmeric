; The second disjunct is consistent with x = 11.
(set-logic QF_LIA)
(set-info :status sat)
(declare-fun x () Int)
(assert (> x 10))
(assert (or (< x 0) (> x 10)))
(check-sat)
(exit)
