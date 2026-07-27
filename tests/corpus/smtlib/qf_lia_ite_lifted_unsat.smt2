; Formerly unsupported_ite_skip.smt2, the deliberate skip-path exercise from
; before the reader lifted ite to a fresh defined variable.  Ite is inside the
; fragment now, so this benchmark pins the lifting instead: the ite yields 1
; or 2 depending on x, never anything > 5, so the assertion is unsat.  The
; skip-path role passed to unsupported_define_sort_skip.smt2.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (> (ite (> x 0) 1 2) 5))
(check-sat)
(exit)
