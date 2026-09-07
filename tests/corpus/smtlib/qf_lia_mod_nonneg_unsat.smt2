; A Euclidean remainder is never negative, whatever the dividend's sign:
; (mod x 3) is one of 0, 1, 2 for every x.  Under C's `%` it would be -2 for
; x = -2, and a reader/evaluator using that produces a MODEL for this
; contradictory set (the defect this benchmark was written against).
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (< (mod x 3) 0))
(check-sat)
(exit)
