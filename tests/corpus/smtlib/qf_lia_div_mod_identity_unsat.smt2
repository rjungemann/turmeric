; The defining identity of Euclidean division: m = n * (div m n) + (mod m n)
; for every m and nonzero n.  Its negation has no model.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (not (= x (+ (* 2 (div x 2)) (mod x 2)))))
(check-sat)
(exit)
