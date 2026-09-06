; Euclidean division of a negative dividend by a positive divisor rounds
; DOWN: -7 = 2 * (-4) + 1, so (div -7 2) = -4 and (mod -7 2) = 1.  The
; truncating quotient is -3 (remainder -1); asserting it is a contradiction.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (= x (- 7)))
(assert (= (div x 2) (- 3)))
(check-sat)
(exit)
