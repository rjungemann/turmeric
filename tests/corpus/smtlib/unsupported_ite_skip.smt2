; Deliberately outside the fragment: the VC has no ITE, so the harness must
; SKIP this whole benchmark rather than guess a translation. Its presence keeps
; the skip path exercised -- a reader that silently mis-parsed it would report a
; pass for work it did not do.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (> (ite (> x 0) 1 2) 5))
(check-sat)
(exit)
