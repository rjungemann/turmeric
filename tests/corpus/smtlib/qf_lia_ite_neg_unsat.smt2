; ite under a NEGATION. The VC has no VC_ITE; an arithmetic ite is lifted to a
; fresh variable with its definition asserted at top level. That is
; equisatisfiable REGARDLESS OF POLARITY -- the two implications define the
; fresh variable uniquely, so a negated occurrence is not a special case. This
; pins that claim.
; ite under a NEGATION -- the polarity case the lifting argument depends on
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (<= x 0))
(assert (not (= (ite (> x 0) 5 7) 7)))
(check-sat)
