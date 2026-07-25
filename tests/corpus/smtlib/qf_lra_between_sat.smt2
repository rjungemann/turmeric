; Unlike the integers, a real DOES lie strictly between 0 and 1.
(set-logic QF_LRA)
(set-info :status sat)
(declare-fun r () Real)
(assert (< 0.0 r))
(assert (< r 1.0))
(check-sat)
(exit)
