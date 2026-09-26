;;; srfi/30 -- SRFI 30, Nested Multi-line Comments: built into R7RS.
;;;
;;; `#| ... |#` nests in every #lang r7rs file, so there is nothing to import.
;;; The library exists, empty, so `(import (srfi 30))` works as it does in
;;; Racket, whose srfi/30 is "Supported by core PLT, nothing to provide".
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 30)
  (export)
  (import (scheme base)))
