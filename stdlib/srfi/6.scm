;;; srfi/6 -- SRFI 6, Basic String Ports: built into R7RS.
;;;
;;; Importing (srfi 6) costs nothing: R7RS adopted SRFI 6, so these are
;;; (scheme base)'s own procedures, as Racket's srfi/6 re-exports its core's.
;;; The export list is what `(import (only (srfi 6) ...))`, `except`, `prefix`
;;; and `rename` check against.  docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 6)
  (export open-input-string open-output-string get-output-string)
  (import (scheme base)))
