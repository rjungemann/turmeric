;;; srfi/26 -- SRFI 26, Notation for Specializing Parameters without
;;; Currying: `cut` and `cute`.
;;;
;;; The SRFI's reference implementation (cut.scm), as written: by Al
;;; Petrofsky, adapted by Sebastian Egner, placed in the public domain.  See
;;; stdlib/srfi/COPYING.  docs/upcoming/r7rs-srfi-plan.md, S2.
(define-library (srfi 26)
  (export cut cute)
  (import (scheme base))
  (begin
    ;; (srfi-26-internal-cut slot-names combination . se): slot-names are
    ;; the internal names of the slots, combination the procedure being
    ;; specialized followed by its arguments, se the slots-or-exprs left.
    (define-syntax srfi-26-internal-cut
      (syntax-rules (<> <...>)
        ;; construct fixed- or variable-arity procedure:
        ;;   (begin proc) throws an error if proc is not an <expression>
        ((srfi-26-internal-cut (slot-name ...) (proc arg ...))
         (lambda (slot-name ...) ((begin proc) arg ...)))
        ((srfi-26-internal-cut (slot-name ...) (proc arg ...) <...>)
         (lambda (slot-name ... . rest-slot) (apply proc arg ... rest-slot)))
        ;; process one slot-or-expr
        ((srfi-26-internal-cut (slot-name ...)   (position ...)      <>  . se)
         (srfi-26-internal-cut (slot-name ... x) (position ... x)        . se))
        ((srfi-26-internal-cut (slot-name ...)   (position ...)      nse . se)
         (srfi-26-internal-cut (slot-name ...)   (position ... nse)      . se))))
    ;; (srfi-26-internal-cute slot-names nse-bindings combination . se):
    ;; nse-bindings are let-style bindings for the non-slot expressions.
    (define-syntax srfi-26-internal-cute
      (syntax-rules (<> <...>)
        ;; If there are no slot-or-exprs to process, then:
        ;; construct a fixed-arity procedure,
        ((srfi-26-internal-cute
          (slot-name ...) nse-bindings (proc arg ...))
         (let nse-bindings (lambda (slot-name ...) (proc arg ...))))
        ;; or a variable-arity procedure
        ((srfi-26-internal-cute
          (slot-name ...) nse-bindings (proc arg ...) <...>)
         (let nse-bindings (lambda (slot-name ... . x) (apply proc arg ... x))))
        ;; otherwise, process one slot:
        ((srfi-26-internal-cute
          (slot-name ...)         nse-bindings  (position ...)   <>  . se)
         (srfi-26-internal-cute
          (slot-name ... x)       nse-bindings  (position ... x)     . se))
        ;; or one non-slot expression
        ((srfi-26-internal-cute
          slot-names              nse-bindings  (position ...)   nse . se)
         (srfi-26-internal-cute
          slot-names ((x nse) . nse-bindings) (position ... x)       . se))))
    (define-syntax cut
      (syntax-rules ()
        ((cut . slots-or-exprs)
         (srfi-26-internal-cut () () . slots-or-exprs))))
    (define-syntax cute
      (syntax-rules ()
        ((cute . slots-or-exprs)
         (srfi-26-internal-cute () () () . slots-or-exprs))))))
