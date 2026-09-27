;;; srfi/66 -- SRFI 66, Octet Vectors: R7RS bytevectors under SRFI 66's
;;; names.
;;;
;;; An octet vector is a bytevector: u8vector? is bytevector?, and the
;;; constructors, accessors and u8vector-copy are R7RS's.  An element that
;;; is not an octet, or an argument that is not an octet vector, raises an
;;; error object naming the SRFI 66 procedure.
;;; What R7RS lacks is here, written for the SRFI from its document (MIT,
;;; see stdlib/srfi/COPYING): the list conversions, u8vector=?,
;;; u8vector-compare, and u8vector-copy!, whose arguments come in R6RS's
;;; order (source first, then a count), not bytevector-copy!'s.  SRFI 4
;;; re-exports these, so importing both binds one u8vector-ref.
;;; docs/upcoming/r7rs-srfi-plan.md, S7.
(define-library (srfi 66)
  (export
    u8vector? make-u8vector u8vector u8vector->list list->u8vector
    u8vector-length u8vector-ref u8vector-set!
    u8vector=? u8vector-compare u8vector-copy! u8vector-copy)
  (import (scheme base))
  (begin
    ;; Each procedure checks its octet vectors first and raises, so the error
    ;; names the octet vector and the SRFI 66 procedure; a wrong type used to
    ;; panic below it (docs/archive/r7rs-type-errors-are-uncatchable-panics.md).
    (define (octets who u8vector)
      (if (bytevector? u8vector)
          u8vector
          (error (string-append (symbol->string who) ": not an octet vector")
                 u8vector)))

    ;; x, which must be an octet, for `who`.
    (define (octet who x)
      (if (and (exact-integer? x) (<= 0 x 255))
          x
          (error (string-append (symbol->string who) ": not an octet (0..255)")
                 x)))

    (define (u8vector? obj) (bytevector? obj))
    (define (make-u8vector k . fill)
      (if (null? fill)
          (make-bytevector k)
          (make-bytevector k (octet 'make-u8vector (car fill)))))
    (define (u8vector . elements) (octets->u8vector 'u8vector elements))
    (define (list->u8vector elements)
      (octets->u8vector 'list->u8vector elements))
    (define (u8vector-length u8vector)
      (bytevector-length (octets 'u8vector-length u8vector)))
    (define (u8vector-ref u8vector k)
      (bytevector-u8-ref (octets 'u8vector-ref u8vector) k))
    (define (u8vector-set! u8vector k x)
      (bytevector-u8-set! (octets 'u8vector-set! u8vector) k
                          (octet 'u8vector-set! x)))
    (define (u8vector-copy u8vector)
      (bytevector-copy (octets 'u8vector-copy u8vector)))

    (define (u8vector->list u8vector)
      (octets 'u8vector->list u8vector)
      (let loop ((i (- (bytevector-length u8vector) 1)) (acc '()))
        (if (< i 0)
            acc
            (loop (- i 1) (cons (bytevector-u8-ref u8vector i) acc)))))

    (define (octets->u8vector who elements)
      (let ((v (make-bytevector (length elements))))
        (let loop ((i 0) (os elements))
          (if (null? os)
              v
              (begin
                (bytevector-u8-set! v i (octet who (car os)))
                (loop (+ i 1) (cdr os)))))))

    ;; -1, 0 or 1: a shorter vector is smaller, and vectors of one length
    ;; compare lexicographically.
    (define (u8vector-compare u8vector-1 u8vector-2)
      (octets 'u8vector-compare u8vector-1)
      (octets 'u8vector-compare u8vector-2)
      (let ((len-1 (bytevector-length u8vector-1))
            (len-2 (bytevector-length u8vector-2)))
        (cond
         ((< len-1 len-2) -1)
         ((> len-1 len-2) 1)
         (else
          (let loop ((i 0))
            (if (= i len-1)
                0
                (let ((a (bytevector-u8-ref u8vector-1 i))
                      (b (bytevector-u8-ref u8vector-2 i)))
                  (cond ((< a b) -1)
                        ((> a b) 1)
                        (else (loop (+ i 1)))))))))))

    (define (u8vector=? u8vector-1 u8vector-2)
      (= 0 (u8vector-compare u8vector-1 u8vector-2)))

    ;; R7RS's bytevector-copy! already copies correctly between overlapping
    ;; regions of one bytevector.
    (define (u8vector-copy! source source-start target target-start n)
      (octets 'u8vector-copy! source)
      (octets 'u8vector-copy! target)
      (bytevector-copy! target target-start
                        source source-start (+ source-start n)))))
