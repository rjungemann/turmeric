;;; srfi/17 -- SRFI 17, Generalized set!.
;;;
;;; `(set! (f arg ...) v)` is `((setter f) arg ... v)`.  That arm is part of
;;; the lowering's `set!` (src/compiler/scheme_lower.c, srfi17_place), turned
;;; on in a unit that imports this library's `set!` -- by any name -- so the
;;; export is R7RS's own `set!`, and importing it beside (scheme base) is one
;;; binding, not two.  Without the import the shape is an error naming this
;;; SRFI, rather than Turmeric's own "set! target must be a symbol" (which is
;;; what `(set! (.field x) v)` still means: its head is a `.field`, not a
;;; Scheme identifier, so this arm never sees it).
;;;
;;; `setter` is an association list keyed by procedure identity, which R7RS
;;; 6.1 gives a procedure and this implementation keeps
;;; (docs/archive/r7rs-prelude-procedures-lose-identity.md): `(setter car)`
;;; is `set-car!` because `(eqv? car car)` is `#t`.
;;;
;;; `setter`, `set-setter!` and `getter-with-setter` are SRFI 17's reference
;;; implementation: Copyright (C) Per Bothner (2000); MIT licence, see
;;; stdlib/srfi/COPYING.  The standard entries are written for Turmeric.
;;; docs/archive/r7rs-srfi-plan.md, S2.
(define-library (srfi 17)
  (export set! setter getter-with-setter)
  (import (scheme base) (scheme cxr))
  (begin
    ;; The table is built the first time something reaches it, as SRFI 14's
    ;; standard char sets are: an initializer that allocates is not a
    ;; candidate for S3's pruning pass, so a program that imports this SRFI
    ;; and never uses it would otherwise carry the whole c[ad]r family.
    (define setters #f)

    (define (setter-table)
      (if (not setters) (set! setters (standard-setters)))
      setters)

    ;; `(set! (setter f) s)` lowers to `((setter setter) f s)`, so `setter`
    ;; is itself an entry in the table, mapped to this.
    (define (set-setter! proc set)
      (set! setters (cons (cons proc set) (setter-table))))

    (define (setter proc)
      (let ((probe (assv proc (setter-table))))
        (if probe
            (cdr probe)
            (error "no setter for procedure" proc))))

    (define (getter-with-setter get set)
      (let ((proc (lambda args (apply get args))))
        (set-setter! proc set)
        proc))

    ;; `c<first><rest>r` stores through its FIRST step: the rest of the name
    ;; is the accessor, and the first letter says which half of that pair to
    ;; store into.  So `caddr` is `(set-car! (cddr p) v)`.
    (define (through get set)
      (lambda (p v) (set (get p) v)))

    ;; The settable standard procedures: SRFI 17's own list -- `car`, `cdr`,
    ;; the c[ad]r family, `string-ref` and `vector-ref` -- and R7RS's
    ;; `bytevector-u8-ref` beside them.  `car` and `cdr` lead, being the ones
    ;; a program writes.
    (define (standard-setters)
      (list (cons car set-car!)
            (cons cdr set-cdr!)
            (cons vector-ref vector-set!)
            (cons string-ref string-set!)
            (cons bytevector-u8-ref bytevector-u8-set!)
            (cons setter set-setter!)
            (cons caar (through car set-car!))
            (cons cadr (through cdr set-car!))
            (cons cdar (through car set-cdr!))
            (cons cddr (through cdr set-cdr!))
            (cons caaar (through caar set-car!))
            (cons caadr (through cadr set-car!))
            (cons cadar (through cdar set-car!))
            (cons caddr (through cddr set-car!))
            (cons cdaar (through caar set-cdr!))
            (cons cdadr (through cadr set-cdr!))
            (cons cddar (through cdar set-cdr!))
            (cons cdddr (through cddr set-cdr!))
            (cons caaaar (through caaar set-car!))
            (cons caaadr (through caadr set-car!))
            (cons caadar (through cadar set-car!))
            (cons caaddr (through caddr set-car!))
            (cons cadaar (through cdaar set-car!))
            (cons cadadr (through cdadr set-car!))
            (cons caddar (through cddar set-car!))
            (cons cadddr (through cdddr set-car!))
            (cons cdaaar (through caaar set-cdr!))
            (cons cdaadr (through caadr set-cdr!))
            (cons cdadar (through cadar set-cdr!))
            (cons cdaddr (through caddr set-cdr!))
            (cons cddaar (through cdaar set-cdr!))
            (cons cddadr (through cdadr set-cdr!))
            (cons cdddar (through cddar set-cdr!))
            (cons cddddr (through cdddr set-cdr!))))))
