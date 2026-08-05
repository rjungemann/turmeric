; CAPTURE: the macro parameter is named x, and the argument mentions the
; CALLER's own x. Expanding by pushing the parameter binding BEFORE translating
; the argument would re-read x as the parameter and capture it. The reader
; translates arguments in the caller's scope first, which is what this pins.
; With x = 1, (addone x) is 2, so asserting it equals 5 is unsat.
(set-logic QF_LIA)
(set-info :status unsat)
(define-fun addone ((x Int)) Int (+ x 1))
(declare-fun x () Int)
(assert (= x 1))
(assert (= (addone x) 5))
(check-sat)
