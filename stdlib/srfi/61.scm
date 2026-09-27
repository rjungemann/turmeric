;;; srfi/61 -- SRFI 61, A more general cond clause.
;;;
;;; R7RS's `cond` with one more clause shape, `(generator guard =>
;;; receiver)`: generator's values go to guard, and when guard answers true,
;;; to receiver, whose values are the cond's.  The clause is part of the
;;; lowering's `cond` (src/compiler/scheme_lower.c, srfi61_clause), turned on
;;; in a unit that imports this library's `cond` -- by any name -- so the
;;; export is R7RS's own `cond`, and importing it beside (scheme base) is one
;;; binding, not two.  The body is empty: nothing is spliced.
;;; docs/upcoming/r7rs-srfi-plan.md, S2.
(define-library (srfi 61)
  (export cond)
  (import (scheme base))
  (begin))
