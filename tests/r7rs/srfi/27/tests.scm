;;; tests/r7rs/srfi/27/tests.scm -- SRFI 27's tests, as top-level forms for
;;; tests/r7rs/run-conformance.py (r7rs-srfi-plan D7), from two sources:
;;;
;;;   - the reference implementation's conftest.scm (Sebastian Egner, 2002;
;;;     MIT, as the SRFI): check-basics-1 and check-mrg32k3a, each check a
;;;     test.  Its last check, a sum over 10^7 reals, is left out for time;
;;;     the pseudo-randomize! state it also checks pins the same arithmetic.
;;;   - chibi-scheme's lib/srfi/27/test.sld: the sanity checks and the
;;;     chi-squared histograms.  Its flloggamma ((scheme flonum)) is a
;;;     Lanczos approximation here, and its vector-fold ((scheme vector)) a
;;;     local fold, and each histogram draws 1000 numbers, not 10000: the
;;;     interpreter spends about 3 ms on a bignum draw, and the seed is
;;;     fixed, so the smaller sample is as deterministic a check.  (c)
;;;     2009-2021 Alex Shinn, BSD-3, see tests/r7rs/CHIBI-COPYING.

(test-begin "srfi-27: random")

;; ---- conftest.scm: check-basics-1 ----

(define (my-random-integer n)
  (let ((x (random-integer n)))
    (if (<= 0 x (- n 1))
        x
        (error "(random-integer n) returned illegal value" x))))

(define (my-random-real)
  (let ((x (random-real)))
    (if (< 0 x 1)
        x
        (error "(random-real) returned illegal value" x))))

;; generate increasingly large numbers
(test #t (do ((k 0 (+ k 1))
              (n 1 (* n 2)))
             ((> k 1024) #t)
           (my-random-integer n)))

;; generate some reals
(test #t (do ((k 0 (+ k 1))
              (x (my-random-real) (+ x (my-random-real))))
             ((= k 1000) (real? x))))

;; get/set the state
(test '(#t #t)
      (let* ((state1 (random-source-state-ref default-random-source))
             (x1 (my-random-integer (expt 2 32)))
             (state2 (random-source-state-ref default-random-source))
             (x2 (my-random-integer (expt 2 32))))
        (random-source-state-set! default-random-source state1)
        (let ((y1 (my-random-integer (expt 2 32))))
          (random-source-state-set! default-random-source state2)
          (let ((y2 (my-random-integer (expt 2 32))))
            (list (= x1 y1) (= x2 y2))))))

;; randomize!
(test #f (let* ((state1 (random-source-state-ref default-random-source))
                (x1 (my-random-integer (expt 2 32))))
           (random-source-state-set! default-random-source state1)
           (random-source-randomize! default-random-source)
           (= x1 (my-random-integer (expt 2 32)))))

;; pseudo-randomize!
(test '(#f #f)
      (let* ((state1 (random-source-state-ref default-random-source))
             (x1 (my-random-integer (expt 2 32))))
        (random-source-state-set! default-random-source state1)
        (random-source-pseudo-randomize! default-random-source 0 1)
        (let ((y1 (my-random-integer (expt 2 32))))
          (random-source-state-set! default-random-source state1)
          (random-source-pseudo-randomize! default-random-source 1 0)
          (let ((y2 (my-random-integer (expt 2 32))))
            (list (= x1 y1) (= x1 y2))))))

;; ---- conftest.scm: check-mrg32k3a ----

;; the initial state is A^16 * (1 0 0 1 0 0)
(test #t (let* ((s (make-random-source))
                (state1 (random-source-state-ref s))
                (rand (random-source-make-reals s)))
           (random-source-state-set! s '(lecuyer-mrg32k3a 1 0 0 1 0 0))
           (do ((k 0 (+ k 1)))
               ((= k 16)
                (equal? state1 (random-source-state-ref s)))
             (rand))))

;; pseudo-randomize! advances properly
(test '(lecuyer-mrg32k3a
        1250826159
        3004357423
        431373563
        3322526864
        623307378
        2983662421)
      (let ((s (make-random-source)))
        (random-source-pseudo-randomize! s 1 2)
        (random-source-state-ref s)))

;; ---- more of the interface ----

(test #t (random-source? default-random-source))
(test #t (random-source? (make-random-source)))
(test #f (random-source? '(lecuyer-mrg32k3a 1 0 0 1 0 0)))
(test 'lecuyer-mrg32k3a (car (random-source-state-ref (make-random-source))))
(test-error (random-source-state-set! (make-random-source) '(other 1 2 3)))
(test-error (random-source-state-set! (make-random-source)
                                      '(lecuyer-mrg32k3a 0 0 0 1 0 0)))
(test-error (random-source-make-reals (make-random-source) 2))
(test #t (let ((r ((random-source-make-reals (make-random-source) 1e-20))))
           (and (inexact? r) (< 0 r 1))))
(test #t (let ((s1 (make-random-source))
               (s2 (make-random-source)))
           (= ((random-source-make-integers s1) 100000)
              ((random-source-make-integers s2) 100000))))

;; ---- chibi's test.sld ----

(define (random-histogram bound n . o)
  (let* ((hist (make-vector (if (pair? o) (car o) (min 10 bound)) 0))
         (rs (make-random-source))
         (rand (random-source-make-integers rs)))
    (random-source-pseudo-randomize! rs 23 42)
    (do ((i 0 (+ i 1)))
        ((= i n) hist)
      (let* ((a (rand bound))
             (b (quotient (* a (vector-length hist)) bound)))
        (vector-set! hist b (+ 1 (vector-ref hist b)))))))
;; Turmeric: log-gamma by Lanczos (g = 7), for flloggamma.
(define lanczos-coefficients
  '(0.99999999999980993 676.5203681218851 -1259.1392167224028
    771.32342877765313 -176.61502916214059 12.507343278686905
    -0.13857109526572012 9.9843695780195716e-6 1.5056327351493116e-7))
(define (loggamma x)
  (let* ((x (- x 1))
         (t (+ x 7.5))
         (a (let lp ((cs (cdr lanczos-coefficients)) (i 1)
                     (sum (car lanczos-coefficients)))
              (if (null? cs)
                  sum
                  (lp (cdr cs) (+ i 1) (+ sum (/ (car cs) (+ x i))))))))
    (+ (* 0.5 (log (* 2 3.141592653589793)))
       (* (+ x 0.5) (log t))
       (- t)
       (log a))))
;; Turmeric: a one-vector vector-fold, for (scheme vector)'s.
(define (vector-fold kons knil vec)
  (let lp ((i 0) (acc knil))
    (if (= i (vector-length vec))
        acc
        (lp (+ i 1) (kons acc (vector-ref vec i))))))
;; continued fraction expansion, borrowed from (chibi math stats)
(define (lower-incomplete-gamma s z)
  (let lp ((k 1) (x 1.0) (sum 1.0))
    (if (or (= k 1000) (< (/ x sum) 1e-14))
        (exp (+ (* s (log z))
                (log sum)
                (- z)
                (- (loggamma (+ s 1.)))))
        (let* ((x2 (* x (/ z (+ s k))))
               (sum2 (+ sum x2)))
          (lp (+ k 1) x2 sum2)))))
(define (chi^2-cdf X^2 df)
  (min 1 (lower-incomplete-gamma (/ df 2) (/ X^2 2))))
(define (histogram-uniform? hist . o)
  ;; ultra-conservative alpha to avoid test failures on false positives
  (let* ((alpha (if (pair? o) (car o) 1e-5))
         (n (vector-fold + 0 hist))
         (len (vector-length hist))
         (expected (/ n (inexact len)))
         (X^2 (vector-fold
               (lambda (X^2 observed)
                 (+ X^2 (/ (square (- observed expected)) expected)))
               0
               hist))
         (p (- 1.0 (chi^2-cdf X^2 (- len 1)))))
    (> p alpha)))
(define (test-random rand n)
  (<= 0 (rand n) (- n 1)))

;; sanity checks
(test 0 (random-integer 1))
(test-assert (<= 0 (random-integer 2) 1))
(test-error (random-integer 0))
(test-error (random-integer -1))

;; chosen by fair dice roll.  guaranteed to be random
(define rs4 (make-random-source))
(random-source-pseudo-randomize! rs4 4 4)
(define rand4 (random-source-make-integers rs4))
(test #t (do ((k 0 (+ k 5))
              (n 1 (* n 2))
              (ok #t (and ok (test-random rand4 n))))
             ((> k 1024) ok)))
(test-not (let* ((state (random-source-state-ref rs4))
                 (x (rand4 100000)))
            (= x (rand4 100000))))

;; Distribution checks
(test-assert
    (histogram-uniform? (random-histogram 2 1000)))      ; coin
(test-assert
    (histogram-uniform? (random-histogram 6 1000)))     ; die
(test-assert
    (histogram-uniform? (random-histogram 27 1000 27))) ; small prime
;; boundaries
(test-assert
    (histogram-uniform? (random-histogram (expt 2 31) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (expt 2 32) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (- (expt 2 62) 1) 1000)))
;; bignums
(test-assert
    (histogram-uniform? (random-histogram (expt 2 62) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (expt 2 63) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (expt 2 63) 1000 100)))
(test-assert
    (histogram-uniform? (random-histogram (- (expt 2 64) 1) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (expt 2 64) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (+ (expt 2 64) 1) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (expt 2 65) 1000)))
(test-assert
    (histogram-uniform? (random-histogram (expt 2 164) 1000)))

(test-end)
