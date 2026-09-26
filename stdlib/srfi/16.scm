;;; srfi/16 -- SRFI 16, Syntax for procedures of variable arity: built into
;;; R7RS.
;;;
;;; Importing (srfi 16) costs nothing: R7RS's case-lambda is SRFI 16's, as
;;; Racket's srfi/16 re-exports its core's. The export list is what `(import
;;; (only (srfi 16) ...))`, `except`, `prefix` and `rename` check against.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 16)
  (export case-lambda)
  (import (scheme case-lambda)))
