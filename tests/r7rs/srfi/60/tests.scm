;;; tests/r7rs/srfi/60/tests.scm -- SRFI 60's tests, as top-level forms for
;;; tests/r7rs/run-conformance.py (r7rs-srfi-plan D7).  SRFI 60 ships no
;;; suite, so this is every example in its document (Copyright (C) Aubrey
;;; Jaffer (2004, 2005), MIT), then Turmeric's: bignums, negatives, and a
;;; cross-check of the fixnum fast path against the bignum path (the same
;;; operation on both, shifted past 64 bits, must agree).

(test-begin "srfi-60: integers as bits")

(define (iota* n)
  (let lp ((i (- n 1)) (acc '()))
    (if (< i 0) acc (lp (- i 1) (cons i acc)))))

;; ---- the document's examples ----

(test "1000" (number->string (logand #b1100 #b1010) 2))
(test "1110" (number->string (logior #b1100 #b1010) 2))
(test "110" (number->string (logxor #b1100 #b1010) 2))
(test "-10000001" (number->string (lognot #b10000000) 2))
(test "-1" (number->string (lognot #b0) 2))
(test #f (logtest #b0100 #b1011))
(test #t (logtest #b0100 #b0111))
(test 4 (logcount #b10101010))
(test 0 (logcount 0))
(test 1 (logcount -2))
(test 8 (integer-length #b10101010))
(test 0 (integer-length 0))
(test 4 (integer-length #b1111))
(test '(-1 0 1 0 2 0 1 0 3 0 1 0 2 0 1 0 4)
      (map (lambda (i) (log2-binary-factors (- i))) (iota* 17)))
(test '(-1 0 1 0 2 0 1 0 3 0 1 0 2 0 1 0 4)
      (map log2-binary-factors (iota* 17)))
(test #t (logbit? 0 #b1101))
(test #f (logbit? 1 #b1101))
(test #t (logbit? 2 #b1101))
(test #t (logbit? 3 #b1101))
(test #f (logbit? 4 #b1101))
(test "1" (number->string (copy-bit 0 0 #t) 2))
(test "100" (number->string (copy-bit 2 0 #t) 2))
(test "1011" (number->string (copy-bit 2 #b1111 #f) 2))
(test "1010" (number->string (bit-field #b1101101010 0 4) 2))
(test "10110" (number->string (bit-field #b1101101010 4 9) 2))
(test "1101100000" (number->string (copy-bit-field #b1101101010 0 0 4) 2))
(test "1101101111" (number->string (copy-bit-field #b1101101010 -1 0 4) 2))
(test "110100111110000"
      (number->string (copy-bit-field #b110100100010000 -1 5 9) 2))
(test "1000" (number->string (ash #b1 3) 2))
(test "101" (number->string (ash #b1010 -1) 2))
(test "10" (number->string (rotate-bit-field #b0100 3 0 4) 2))
(test "10" (number->string (rotate-bit-field #b0100 -1 0 4) 2))
(test "110100010010000"
      (number->string (rotate-bit-field #b110100100010000 -1 5 9) 2))
(test "110100000110000"
      (number->string (rotate-bit-field #b110100100010000 1 5 9) 2))
(test "e5" (number->string (reverse-bit-field #xa7 0 8) 16))

;; ---- bits as booleans ----

(test '(#t #t #f) (integer->list 6))
(test '(#f #f #t #t #f) (integer->list 6 5))
(test '() (integer->list 0))
(test 6 (list->integer '(#t #t #f)))
(test 0 (list->integer '()))
(test 13 (booleans->integer #t #t #f #t))
(test 12345678901234567890123
      (list->integer (integer->list 12345678901234567890123)))

;; ---- the aliases are the same procedures ----

(test '(8 14 6 -13)
      (list (bitwise-and 12 10) (bitwise-ior 12 10) (bitwise-xor 12 10)
            (bitwise-not 12)))
(test '(#t 3 #t 2)
      (list (any-bits-set? 4 7) (bit-count 7) (bit-set? 2 4)
            (first-set-bit 12)))
(test (bitwise-if #b1100 #b1010 #b0101) (bitwise-merge #b1100 #b1010 #b0101))
(test #b1001 (bitwise-if #b1100 #b1010 #b0101))
(test 1024 (arithmetic-shift 1 10))

;; ---- arity ----

(test -1 (logand))
(test 0 (logior))
(test 0 (logxor))
(test 7 (logand 7))
(test 15 (logior 1 2 4 8))
(test 0 (logand 1 2 4 8))
(test 1 (logxor 1 1 1))

;; ---- negatives, two's complement ----

(test -8 (logand -1 -8))
(test -3 (logior -4 1))
(test 7 (logxor -8 -1))
(test -1 (ash -1 -1))
(test -3 (ash -5 -1))
(test -1 (ash -5 -100))
(test 0 (ash 5 -100))
(test -20 (ash -5 2))
(test 0 (logcount -1))
(test 63 (integer-length (- (expt 2 63))))
(test 64 (integer-length (expt 2 63)))

;; ---- bignums ----

(test (expt 2 99) (logand (- (expt 2 100) 1) (expt 2 99)))
(test (+ (expt 2 100) 1) (logior (expt 2 100) 1))
(test (- (expt 2 70) 1) (logxor (- (expt 2 70)) -1))
(test (expt 2 80) (logand -1 (expt 2 80)))
(test (- (expt 2 80)) (logand (- (expt 2 80)) (- (expt 2 70))))
(test (- (expt 2 70)) (logior (- (expt 2 80)) (- (expt 2 70))))
(test (expt 2 100) (ash 1 100))
(test 4 (ash (expt 2 100) -98))
(test -4 (ash (- (expt 2 100)) -98))
(test -5 (ash (- 1 (* 5 (expt 2 100))) -100))
(test 4 (ash (- (* 5 (expt 2 100)) 1) -100))
(test 101 (integer-length (expt 2 100)))
(test 100 (logcount (- (expt 2 100) 1)))
(test 100 (log2-binary-factors (expt 2 100)))
(test #t (logbit? 100 (expt 2 100)))
(test #f (logbit? 99 (expt 2 100)))
(test (+ (expt 2 100) 1) (copy-bit 0 (expt 2 100) #t))
(test #b1011 (bit-field (+ (* #b1011 (expt 2 90)) 12345) 90 94))
(test (expt 2 65) (rotate-bit-field 1 65 0 70))
(test (- (expt 2 64) 1) (lognot (- (expt 2 64))))

;; ---- the fast path against the bignum path ----

(define big (expt 2 70))
(define samples
  '(0 1 -1 2 -2 5 -5 12 -12 255 -256 1023 123456789 -987654321
    4611686018427387903 -4611686018427387904))
(define (all-agree? op)
  ;; (op a b), shifted up 70 bits, is (op (* a 2^70) (* b 2^70)): the left
  ;; side is two fixnums, the right two bignums.
  (let loop ((as samples))
    (or (null? as)
        (and (let inner ((bs samples))
               (or (null? bs)
                   (and (= (* (op (car as) (car bs)) big)
                           (op (* (car as) big) (* (car bs) big)))
                        (inner (cdr bs)))))
             (loop (cdr as))))))
(test #t (all-agree? logand))
(test #t (all-agree? logior))
(test #t (all-agree? logxor))
(test #t (let loop ((ns samples))
           (or (null? ns)
               (and (= (ash (car ns) -3) (ash (* (car ns) big) -73))
                    (loop (cdr ns))))))
(test #t (let loop ((ns (cdr samples)))    ; not 0, whose length is 0
           (or (null? ns)
               (and (= (integer-length (car ns))
                       (- (integer-length (* (car ns) big)) 70))
                    (loop (cdr ns))))))

;; ---- errors ----

(test-error (logand 1.5 1))
(test-error (logior 1 'a))
(test-error (lognot "1"))
(test-error (ash 1.5 1))

(test-end)
