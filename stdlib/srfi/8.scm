;;; srfi/8 -- SRFI 8, RECEIVE: Binding to multiple values.
;;;
;;; The SRFI document's reference implementation, as written.  Copyright (C)
;;; John David Stone (1999); MIT licence, see stdlib/srfi/COPYING.
;;; docs/archive/r7rs-srfi-plan.md, S2.
(define-library (srfi 8)
  (export receive)
  (import (scheme base))
  (begin
    (define-syntax receive
      (syntax-rules ()
        ((receive formals expression body ...)
         (call-with-values (lambda () expression)
                           (lambda formals body ...)))))))
