; xor is encoded directly as (a or b) and not (a and b) -- no VC_XOR needed.
(set-logic QF_LIA)
(set-info :status unsat)
(declare-fun p () Bool)
(declare-fun q () Bool)
(assert p)
(assert q)
(assert (xor p q))
(check-sat)
