;;; srfi/34 -- SRFI 34, Exception Handling for Programs: built into R7RS.
;;;
;;; Importing (srfi 34) costs nothing: R7RS adopted SRFI 34's
;;; with-exception-handler, guard and raise. The export list is what `(import
;;; (only (srfi 34) ...))`, `except`, `prefix` and `rename` check against.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 34)
  (export with-exception-handler guard raise)
  (import (scheme base)))
