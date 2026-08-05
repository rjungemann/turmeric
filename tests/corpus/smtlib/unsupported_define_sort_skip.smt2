; Deliberately outside the fragment: `define-sort` is an unsupported command,
; so the reader must SKIP this whole benchmark rather than translate around
; it.  Its presence keeps the skip path exercised -- a reader that silently
; mis-parsed it would report a pass for work it did not do.  (This role used
; to belong to unsupported_ite_skip.smt2, until ite lifting brought ite
; inside the fragment; that file lives on as qf_lia_ite_lifted_unsat.smt2.)
; The label is still real and solver-confirmed: no integer lies strictly
; between 0 and 1, so the assertions are unsat.
(set-logic QF_LIA)
(set-info :status unsat)
(define-sort MyInt () Int)
(declare-fun x () MyInt)
(assert (> x 0))
(assert (< x 1))
(check-sat)
(exit)
