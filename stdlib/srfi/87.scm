;;; srfi/87 -- SRFI 87, => in case clauses: built into R7RS.
;;;
;;; Importing (srfi 87) costs nothing: R7RS's case takes => clauses. The
;;; export list is what `(import (only (srfi 87) ...))`, `except`, `prefix`
;;; and `rename` check against.  docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 87)
  (export case)
  (import (scheme base)))
