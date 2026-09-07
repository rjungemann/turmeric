; SMT-LIB `mod` is EUCLIDEAN: 0 <= (mod m n) < |n| for every m, including a
; negative one.  x = -1 is a model: -1 = 2 * (-1) + 1, so (mod -1 2) = 1.
; Under C's truncating `%` the remainder takes the dividend's sign
; ((-1) % 2 = -1) and this set would have NO model -- a reader that read
; `mod` as `%` answers unsat here, and this label catches it.
(set-logic QF_LIA)
(set-info :status sat)
(declare-fun x () Int)
(assert (= (mod x 2) 1))
(assert (< x 0))
(check-sat)
(exit)
