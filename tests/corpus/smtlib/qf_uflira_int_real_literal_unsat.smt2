; The companion of qf_uflira_int_real_literal_sat: the same shape with the
; two literals naming DIFFERENT values (`x = 3.0`, `n = 4`) is contradictory,
; and S1 must still see it -- the fix compares the literals' values, it does
; not stop comparing them.
(set-logic QF_UFLIRA)
(set-info :status unsat)
(declare-fun x () Real)
(declare-fun n () Int)
(declare-fun to-f (Int) Real)
(assert (= x (to-f n)))
(assert (= n 4))
(assert (= x 3.0))
(assert (= (to-f n) n))
(check-sat)
(exit)
