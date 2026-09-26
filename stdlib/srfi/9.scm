;;; srfi/9 -- SRFI 9, Defining Record Types: built into R7RS.
;;;
;;; Importing (srfi 9) costs nothing: R7RS's define-record-type is SRFI 9's.
;;; The export list is what `(import (only (srfi 9) ...))`, `except`, `prefix`
;;; and `rename` check against.  docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 9)
  (export define-record-type)
  (import (scheme base)))
