;;; srfi/39 -- SRFI 39, Parameter objects: built into R7RS.
;;;
;;; Importing (srfi 39) costs nothing: R7RS's parameter objects are SRFI 39's,
;;; the converter applied to the initial value and to each parameterize, as
;;; Racket's srfi/39 re-exports its core's. The export list is what `(import
;;; (only (srfi 39) ...))`, `except`, `prefix` and `rename` check against.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 39)
  (export make-parameter parameterize)
  (import (scheme base)))
