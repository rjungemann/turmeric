;;; srfi/23 -- SRFI 23, Error reporting mechanism: built into R7RS.
;;;
;;; Importing (srfi 23) costs nothing: R7RS's error is SRFI 23's, as Racket's
;;; srfi/23 re-exports its core's. The export list is what `(import (only
;;; (srfi 23) ...))`, `except`, `prefix` and `rename` check against.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 23)
  (export error)
  (import (scheme base)))
