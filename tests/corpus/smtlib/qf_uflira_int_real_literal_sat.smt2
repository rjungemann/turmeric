; An Int literal and a Real literal for ONE value in one equivalence class.
; `x = 3.0` and `x = n` with `n = 3` are consistent (x = 3.0, n = 3, to-f is
; the identity on 3), so the set has a model.  S1's literal check used to
; call the class {x, to-f(n), 3.0, n, 3} contradictory because `3` and `3.0`
; are distinct hash-consed terms -- a satisfiable cube answered unsat, which
; proved any goal placed under it (the compiler elided a return check on
; exactly this shape: docs/guides/refinement-solver-internals-guide.md, S1).
; The `y < 5.0` conjunct keeps the set open on its own, so `unsat` here can
; only come from the literal pair.
(set-logic QF_UFLIRA)
(set-info :status sat)
(declare-fun x () Real)
(declare-fun n () Int)
(declare-fun y () Real)
(declare-fun to-f (Int) Real)
(assert (= x (to-f n)))
(assert (= n 3))
(assert (= x 3.0))
(assert (< y 5.0))
(check-sat)
(exit)
