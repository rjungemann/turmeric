;;; srfi/60 -- SRFI 60, Integers as Bits.
;;;
;;; The SRFI's implementation (SLIB's logical.scm, as the SRFI document
;;; carries it), Copyright (C) Aubrey Jaffer (2004, 2005); MIT, see
;;; stdlib/srfi/COPYING.  Integers are two's complement of unbounded width,
;;; negatives included.  Changes for Turmeric are marked "Turmeric:":
;;;   - logand, logior and logxor take two fixnums to the prelude's bit
;;;     operations (r7rs-fx-and__ and friends), and a bignum 30 bits at a
;;;     time by floor-quotient and modulo, where the reference went 4 bits
;;;     at a time through two 16x16 tables.  Bignums are base-1e9 limbs,
;;;     not bits (r7rs-srfi-plan 2.5), so their bits are arithmetic here;
;;;   - arithmetic-shift shifts a fixnum right with the prelude's
;;;     r7rs-fx-shr__;
;;;   - an argument that is not an exact integer is an error, raised.
;;; docs/archive/r7rs-srfi-plan.md, S7.
(define-library (srfi 60)
  (export
    logand bitwise-and logior bitwise-ior logxor bitwise-xor
    lognot bitwise-not bitwise-if bitwise-merge logtest any-bits-set?
    logcount bit-count integer-length log2-binary-factors first-set-bit
    logbit? bit-set? copy-bit bit-field copy-bit-field ash arithmetic-shift
    rotate-bit-field reverse-bit-field
    integer->list list->integer booleans->integer)
  (import (scheme base))
  (begin

    ;; Turmeric: the two-argument operations.  A fixnum pair is one prelude
    ;; call.  Otherwise the integers are taken 30 bits at a time, low bits
    ;; first: modulo 2^30 is a chunk's bits (floor division keeps two's
    ;; complement), floor-quotient the shift right.  Once both are 0 or -1,
    ;; the rest is sign bits, which the fixnum operation extends.
    (define (logical:check who n)
      (if (not (exact-integer? n))
          (error (string-append (symbol->string who)
                                ": not an exact integer")
                 n)))

    (define logical:chunk 1073741824)

    (define (logical:bignum-op fx-op n1 n2)
      (let loop ((n1 n1) (n2 n2) (scl 1) (acc 0))
        (if (and (or (eqv? n1 0) (eqv? n1 -1))
                 (or (eqv? n2 0) (eqv? n2 -1)))
            (+ acc (* scl (fx-op n1 n2)))
            (loop (floor-quotient n1 logical:chunk)
                  (floor-quotient n2 logical:chunk)
                  (* scl logical:chunk)
                  (+ acc (* scl (fx-op (modulo n1 logical:chunk)
                                       (modulo n2 logical:chunk))))))))

    (define (logical:binary who fx-op)
      (lambda (n1 n2)
        (if (and (r7rs-fixnum?__ n1) (r7rs-fixnum?__ n2))
            (fx-op n1 n2)
            (begin
              (logical:check who n1)
              (logical:check who n2)
              (logical:bignum-op fx-op n1 n2)))))

    (define logical:and2
      (logical:binary 'logand (lambda (a b) (r7rs-fx-and__ a b))))
    (define logical:ior2
      (logical:binary 'logior (lambda (a b) (r7rs-fx-ior__ a b))))
    (define logical:xor2
      (logical:binary 'logxor (lambda (a b) (r7rs-fx-xor__ a b))))

    (define (logical:reduce op2 ident)
      (lambda args
        (do ((res ident (op2 res (car rgs)))
             (rgs args (cdr rgs)))
            ((null? rgs) res))))

    ;@
    (define logand (logical:reduce logical:and2 -1))
    ;@
    (define logior (logical:reduce logical:ior2 0))
    ;@
    (define logxor (logical:reduce logical:xor2 0))

    ;; The reference from here, but for arithmetic-shift's fast path.
    (define (logical:ash-4 x)
      (if (negative? x)
          (+ -1 (quotient (+ 1 x) 16))
          (quotient x 16)))
    ;@
    (define (lognot n)
      (logical:check 'lognot n)
      (- -1 n))
    ;@
    (define (logtest n1 n2)
      (not (zero? (logand n1 n2))))
    ;@
    (define (logbit? index n)
      (logtest (expt 2 index) n))
    ;@
    (define (copy-bit index to bool)
      (if bool
          (logior to (arithmetic-shift 1 index))
          (logand to (lognot (arithmetic-shift 1 index)))))
    ;@
    (define (bitwise-if mask n0 n1)
      (logior (logand mask n0)
              (logand (lognot mask) n1)))
    ;@
    (define (bit-field n start end)
      (logand (lognot (ash -1 (- end start)))
              (arithmetic-shift n (- start))))
    ;@
    (define (copy-bit-field to from start end)
      (bitwise-if (arithmetic-shift (lognot (ash -1 (- end start))) start)
                  (arithmetic-shift from start)
                  to))
    ;@
    (define (rotate-bit-field n count start end)
      (define width (- end start))
      (set! count (modulo count width))
      (let ((mask (lognot (ash -1 width))))
        (define zn (logand mask (arithmetic-shift n (- start))))
        (logior (arithmetic-shift
                 (logior (logand mask (arithmetic-shift zn count))
                         (arithmetic-shift zn (- count width)))
                 start)
                (logand (lognot (ash mask start)) n))))
    ;@
    (define (arithmetic-shift n count)
      (logical:check 'arithmetic-shift n)
      (cond
       ;; Turmeric: a fixnum shifted right by the prelude's bit operations.
       ((and (negative? count) (r7rs-fixnum?__ n))
        (if (< count -63)
            (if (negative? n) -1 0)
            (r7rs-fx-shr__ n (- count))))
       ((negative? count)
        (let ((k (expt 2 (- count))))
          (if (negative? n)
              (+ -1 (quotient (+ 1 n) k))
              (quotient n k))))
       (else
        (* (expt 2 count) n))))
    ;@
    (define integer-length
      (letrec ((intlen (lambda (n tot)
                         (case n
                           ((0 -1) (+ 0 tot))
                           ((1 -2) (+ 1 tot))
                           ((2 3 -3 -4) (+ 2 tot))
                           ((4 5 6 7 -5 -6 -7 -8) (+ 3 tot))
                           (else (intlen (logical:ash-4 n) (+ 4 tot)))))))
        (lambda (n)
          (logical:check 'integer-length n)
          (intlen n 0))))
    ;@
    (define logcount
      (letrec ((logcnt (lambda (n tot)
                         (if (zero? n)
                             tot
                             (logcnt (quotient n 16)
                                     (+ (vector-ref
                                         '#(0 1 1 2 1 2 2 3 1 2 2 3 2 3 3 4)
                                         (modulo n 16))
                                        tot))))))
        (lambda (n)
          (logical:check 'logcount n)
          (cond ((negative? n) (logcnt (lognot n) 0))
                ((positive? n) (logcnt n 0))
                (else 0)))))
    ;@
    (define (log2-binary-factors n)
      (+ -1 (integer-length (logand n (- n)))))

    (define (bit-reverse k n)
      (do ((m (if (negative? n) (lognot n) n) (arithmetic-shift m -1))
           (k (+ -1 k) (+ -1 k))
           (rvs 0 (logior (arithmetic-shift rvs 1) (logand 1 m))))
          ((negative? k) (if (negative? n) (lognot rvs) rvs))))
    ;@
    (define (reverse-bit-field n start end)
      (define width (- end start))
      (let ((mask (lognot (ash -1 width))))
        (define zn (logand mask (arithmetic-shift n (- start))))
        (logior (arithmetic-shift (bit-reverse width zn) start)
                (logand (lognot (ash mask start)) n))))
    ;@
    (define (integer->list k . len)
      (if (null? len)
          (do ((k k (arithmetic-shift k -1))
               (lst '() (cons (odd? k) lst)))
              ((<= k 0) lst))
          (do ((idx (+ -1 (car len)) (+ -1 idx))
               (k k (arithmetic-shift k -1))
               (lst '() (cons (odd? k) lst)))
              ((negative? idx) lst))))
    ;@
    (define (list->integer bools)
      (do ((bs bools (cdr bs))
           (acc 0 (+ acc acc (if (car bs) 1 0))))
          ((null? bs) acc)))
    (define (booleans->integer . bools)
      (list->integer bools))

    ;;;;@ SRFI-60 aliases
    (define ash arithmetic-shift)
    (define bitwise-ior logior)
    (define bitwise-xor logxor)
    (define bitwise-and logand)
    (define bitwise-not lognot)
    (define bit-count logcount)
    (define bit-set?   logbit?)
    (define any-bits-set? logtest)
    (define first-set-bit log2-binary-factors)
    (define bitwise-merge bitwise-if)))
