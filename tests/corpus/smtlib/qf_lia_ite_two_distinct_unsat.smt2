; Two SEPARATE ite terms must get SEPARATE fresh variables. Reusing one name
; would silently equate two unrelated conditionals, which is the way the
; lifting could go wrong without looking wrong.
; two SEPARATE ite terms must not be equated by reusing a fresh name
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(declare-fun y () Int)
(assert (> x 0))
(assert (< y 0))
(assert (= (ite (> x 0) 1 2) 1))
(assert (= (ite (> y 0) 1 2) 1))
(check-sat)
