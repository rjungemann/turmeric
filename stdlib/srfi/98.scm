;;; srfi/98 -- SRFI 98, An interface to access environment variables: built
;;; into R7RS.
;;;
;;; Importing (srfi 98) costs nothing: R7RS's (scheme process-context) has
;;; both procedures; importing (srfi 98) loads that library, as importing it
;;; directly would. The export list is what `(import (only (srfi 98) ...))`,
;;; `except`, `prefix` and `rename` check against.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 98)
  (export get-environment-variable get-environment-variables)
  (import (scheme process-context)))
