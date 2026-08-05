; No integer lies strictly between 0 and 1.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun n () Int)
(assert (< 0 n))
(assert (< n 1))
(check-sat)
(exit)
