; A NEGATIVE divisor: the remainder is still non-negative and below |n|, and
; the quotient absorbs the sign.  7 = (-2) * (-3) + 1 with 0 <= 1 < 2, so
; (div 7 -2) = -3 and (mod 7 -2) = 1; x = 7 is a model.
(set-logic QF_LIA)
(set-info :status sat)
(declare-fun x () Int)
(assert (= x 7))
(assert (= (div x (- 2)) (- 3)))
(assert (= (mod x (- 2)) 1))
(check-sat)
(exit)
