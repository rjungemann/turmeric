; SMT-LIB has TWO divisions: `div` is integer division, `/` is REAL division.
; The VC has one VC_DIV whose constant folding is integer -- correct for the
; compiler, which only emits it from Turmeric's `/` on ints and rejects
; `(* x (/ 1 2))` outright rather than coercing (TUR-E0042).
;
; Translating SMT-LIB `/` onto that node folded (/ 1 2) to 0, which turned
; `pi/2 > a > 0` into `0 > a > 0` and made this shape come back PROVED
; CONTRADICTORY -- a soundness failure in the harness, found by replaying the
; real library (a meti-tarski QF_LRA benchmark).
;
; Satisfiable: pi = 3.14159265, a = 1.0, x = 0.5 witnesses it.
(set-logic QF_LRA)
(set-info :status sat)
(declare-fun pi () Real)
(declare-fun a () Real)
(declare-fun x () Real)
(assert (not (<= a 0)))
(assert (not (<= a x)))
(assert (not (<= x 0)))
(assert (not (<= (* pi (/ 1 2)) a)))
(assert (not (<= (/ 31415927 10000000) pi)))
(assert (not (<= pi (/ 15707963 5000000))))
(check-sat)
(exit)
