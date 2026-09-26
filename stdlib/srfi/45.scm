;;; srfi/45 -- SRFI 45, Primitives for Expressing Iterative Lazy
;;; Algorithms: R7RS's lazy primitives under SRFI 45's names.
;;;
;;; R7RS adopted SRFI 45: its `delay-force` is SRFI 45's `lazy`, its
;;; `make-promise` is `eager`, and `delay`, `force` and `promise?` are the
;;; same.  One difference comes with the adoption: `eager` of a value that
;;; is already a promise returns that promise, as R7RS `make-promise` does,
;;; where SRFI 45's reference implementation wraps it -- so `(force (eager
;;; (delay 7)))` is 7 here.  docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 45)
  (export delay lazy force eager promise?)
  (import (scheme base) (scheme lazy))
  (begin
    (define-syntax lazy
      (syntax-rules ()
        ((_ expr) (delay-force expr))))
    (define (eager x) (make-promise x))))
