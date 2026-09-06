; x and x + 1 cannot both be even: (mod x 2) = 0 means x = 2q, so
; x + 1 = 2q + 1 and (mod (+ x 1) 2) = 1.  Needs the div/mod axioms and the
; integer equality phase together, which is what makes it worth a benchmark.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun x () Int)
(assert (= (mod x 2) 0))
(assert (= (mod (+ x 1) 2) 0))
(check-sat)
(exit)
