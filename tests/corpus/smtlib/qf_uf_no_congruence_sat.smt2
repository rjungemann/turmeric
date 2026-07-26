; Without a = b, f(a) and f(b) are free to differ.
(set-logic QF_UF)
(set-info :status sat)
(declare-fun a () Int)
(declare-fun b () Int)
(declare-fun f (Int) Int)
(assert (not (= (f a) (f b))))
(check-sat)
(exit)
