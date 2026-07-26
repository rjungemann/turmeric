; Over the reals: r < 1/2 and r > 1/2 cannot both hold.
(set-logic QF_LRA)
(set-info :status unsat)
(declare-fun r () Real)
(assert (< r 0.5))
(assert (> r 0.5))
(check-sat)
(exit)
