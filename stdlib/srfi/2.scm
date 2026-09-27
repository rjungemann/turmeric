;;; srfi/2 -- SRFI 2, AND-LET*: an AND with local bindings, a guarded LET*.
;;;
;;; chibi-scheme's lib/srfi/2.sld, as written.  Copyright (c) 2009-2021 Alex
;;; Shinn; BSD-3, see stdlib/srfi/COPYING.  Like chibi's, a bare clause may
;;; be any expression, where SRFI 2 asks for a variable reference.
;;; docs/upcoming/r7rs-srfi-plan.md, S2.
(define-library (srfi 2)
  (export and-let*)
  (import (scheme base))
  (begin
    (define-syntax and-let*
      (syntax-rules ()
        ((and-let* ())
         #t)
        ((and-let* () . body)
         (let () . body))
        ((and-let* ((var expr)))
         expr)
        ((and-let* ((expr)))
         expr)
        ((and-let* (expr))
         expr)
        ((and-let* ((var expr) . rest) . body)
         (let ((var expr))
           (and var (and-let* rest . body))))
        ((and-let* ((expr) . rest) . body)
         (and expr (and-let* rest . body)))
        ((and-let* (expr . rest) . body)
         (let ((tmp expr))
           (and tmp (and-let* rest . body))))))))
