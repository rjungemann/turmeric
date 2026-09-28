;;; srfi/31 -- SRFI 31, A special form `rec` for recursive evaluation.
;;;
;;; The SRFI document's implementation, as written.  Copyright (C) Dr. Mirko
;;; Luedde (2002); MIT licence, see stdlib/srfi/COPYING.
;;; docs/archive/r7rs-srfi-plan.md, S2.
(define-library (srfi 31)
  (export rec)
  (import (scheme base))
  (begin
    (define-syntax rec
      (syntax-rules ()
        ((rec (NAME . VARIABLES) . BODY)
         (letrec ((NAME (lambda VARIABLES . BODY))) NAME))
        ((rec NAME EXPRESSION)
         (letrec ((NAME EXPRESSION)) NAME))))))
