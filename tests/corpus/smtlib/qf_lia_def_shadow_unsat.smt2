; a let INSIDE the caller shadows a name the macro body also uses
(set-logic QF_LIA)
(set-info :status unsat)
(define-fun useg ((a Int)) Int (+ a 100))
(declare-fun y () Int)
(assert (= y 1))
(assert (let ((a 50)) (= (useg y) a)))
(check-sat)
