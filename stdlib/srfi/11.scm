;;; srfi/11 -- SRFI 11, Syntax for receiving multiple values: built into R7RS.
;;;
;;; Importing (srfi 11) costs nothing: R7RS's let-values and let*-values are
;;; SRFI 11's, dotted rest formals included (Racket's core lacks those, so its
;;; srfi/11 is its own). The export list is what `(import (only (srfi 11)
;;; ...))`, `except`, `prefix` and `rename` check against.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 11)
  (export let-values let*-values)
  (import (scheme base)))
