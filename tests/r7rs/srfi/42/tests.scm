;;; tests/r7rs/srfi/42/tests.scm -- SRFI 42's tests: Sebastian Egner's
;;; examples.scm from the SRFI's repository (MIT, the SRFI's licence), in
;;; (chibi test)'s vocabulary for tests/r7rs/run-conformance.py
;;; (r7rs-srfi-plan D7): each (my-check expr => desired) is
;;; (test-equal my-equal? desired expr).  The examples that write a file
;;; do it through the my-open-output-file / my-call-with-input-file hooks
;;; the SRFI leaves to each implementation; here those are string ports.
;;;
;;; Copyright (C) Sebastian Egner (2003). All Rights Reserved.

(test-begin "srfi-42: eager comprehensions")

;; The examples' own hooks, which the SRFI asks each implementation to define
;; (see its header).  Turmeric: over string ports, so a run leaves no file
;; "tmp1" behind in the working directory.
(define my-files '())
(define (my-open-output-file filename)
  (let ((port (open-output-string)))
    (set! my-files (cons (cons filename port) my-files))
    port))
(define (my-call-with-input-file filename proc)
  (proc (open-input-string (get-output-string (cdr (assoc filename my-files))))))

; Tools for checking results
; ==========================

(define (my-equal? x y)
  (cond
   ((or (boolean? x) 
        (null? x)
        (symbol? x) 
        (char? x) 
        (input-port? x)
        (output-port? x) )
    (eqv? x y) )
   ((string? x)
    (and (string? y) (string=? x y)) )
   ((vector? x)
    (and (vector? y)
         (my-equal? (vector->list x) (vector->list y)) ))
   ((pair? x)
    (and (pair? y)
         (my-equal? (car x) (car y))
         (my-equal? (cdr x) (cdr y)) ))
   ((real? x)
    (and (real? y)
         (eqv? (exact? x) (exact? y))
         (if (exact? x)
             (= x y)
             (< (abs (- x y)) (/ 1 (expt 10 6))) ))) ; will do here
   (else
    (error "unrecognized type" x) )))

; ==========================================================================
; do-ec 
; ==========================================================================

(test-equal my-equal? 1
  (let ((x 0)) (do-ec (set! x (+ x 1))) x))

(test-equal my-equal? 10
  (let ((x 0)) (do-ec (:range i 10) (set! x (+ x 1))) x))

(test-equal my-equal? 45
  (let ((x 0)) (do-ec (:range n 10) (:range k n) (set! x (+ x 1))) x))


; ==========================================================================
; list-ec and basic qualifiers 
; ==========================================================================

(test-equal my-equal? '(1)
  (list-ec 1))

(test-equal my-equal? '(0 1 2 3)
  (list-ec (:range i 4) i))

(test-equal my-equal? '((0 0) (1 0) (1 1) (2 0) (2 1) (2 2))
  (list-ec (:range n 3) (:range k (+ n 1)) (list n k)))

(test-equal my-equal? '((0 0) (2 0) (2 1) (2 2) (4 0) (4 1) (4 2) (4 3) (4 4))
  (list-ec (:range n 5) (if (even? n)) (:range k (+ n 1)) (list n k)))

(test-equal my-equal? '((1 0) (1 1) (3 0) (3 1) (3 2) (3 3))
  (list-ec (:range n 5) (not (even? n)) (:range k (+ n 1)) (list n k)))

(test-equal my-equal? '((4 0) (4 1) (4 2) (4 3) (4 4))
  (list-ec (:range n 5) 
           (and (even? n) (> n 2)) 
           (:range k (+ n 1)) 
           (list n k) ))

(test-equal my-equal? '((0 0) (2 0) (2 1) (2 2) (4 0) (4 1) (4 2) (4 3) (4 4))
  (list-ec (:range n 5) 
           (or (even? n) (> n 3)) 
           (:range k (+ n 1)) 
           (list n k) ))

(test-equal my-equal? 10
  (let ((x 0)) (list-ec (:range n 10) (begin (set! x (+ x 1))) n) x))

(test-equal my-equal? '(0 0 1)
  (list-ec (nested (:range n 3) (:range k n)) k))


; ==========================================================================
; Other comprehensions
; ==========================================================================

(test-equal my-equal? '(a b)
  (append-ec '(a b)))
(test-equal my-equal? '()
  (append-ec (:range i 0) '(a b)))
(test-equal my-equal? '(a b)
  (append-ec (:range i 1) '(a b)))
(test-equal my-equal? '(a b a b)
  (append-ec (:range i 2) '(a b)))

(test-equal my-equal? (string #\a)
  (string-ec #\a))
(test-equal my-equal? ""
  (string-ec (:range i 0) #\a))
(test-equal my-equal? "a"
  (string-ec (:range i 1) #\a))
(test-equal my-equal? "aa"
  (string-ec (:range i 2) #\a))

(test-equal my-equal? "ab"
  (string-append-ec "ab"))
(test-equal my-equal? ""
  (string-append-ec (:range i 0) "ab"))
(test-equal my-equal? "ab"
  (string-append-ec (:range i 1) "ab"))
(test-equal my-equal? "abab"
  (string-append-ec (:range i 2) "ab"))

(test-equal my-equal? (vector 1)
  (vector-ec 1))
(test-equal my-equal? (vector)
  (vector-ec (:range i 0) i))
(test-equal my-equal? (vector 0)
  (vector-ec (:range i 1) i))
(test-equal my-equal? (vector 0 1)
  (vector-ec (:range i 2) i))

(test-equal my-equal? (vector 1)
  (vector-of-length-ec 1 1))
(test-equal my-equal? (vector)
  (vector-of-length-ec 0 (:range i 0) i))
(test-equal my-equal? (vector 0)
  (vector-of-length-ec 1 (:range i 1) i))
(test-equal my-equal? (vector 0 1)
  (vector-of-length-ec 2 (:range i 2) i))

(test-equal my-equal? 1
  (sum-ec 1))
(test-equal my-equal? 0
  (sum-ec (:range i 0) i))
(test-equal my-equal? 0
  (sum-ec (:range i 1) i))
(test-equal my-equal? 1
  (sum-ec (:range i 2) i))
(test-equal my-equal? 3
  (sum-ec (:range i 3) i))

(test-equal my-equal? 1
  (product-ec 1))
(test-equal my-equal? 1
  (product-ec (:range i 1 0) i))
(test-equal my-equal? 1
  (product-ec (:range i 1 1) i))
(test-equal my-equal? 1
  (product-ec (:range i 1 2) i))
(test-equal my-equal? 2
  (product-ec (:range i 1 3) i))
(test-equal my-equal? 6
  (product-ec (:range i 1 4) i))

(test-equal my-equal? 1
  (min-ec 1))
(test-equal my-equal? 0
  (min-ec (:range i 1) i))
(test-equal my-equal? 0
  (min-ec (:range i 2) i))

(test-equal my-equal? 1
  (max-ec 1))
(test-equal my-equal? 0
  (max-ec (:range i 1) i))
(test-equal my-equal? 1
  (max-ec (:range i 2) i))

(test-equal my-equal? 1
  (first-ec #f 1))
(test-equal my-equal? #f
  (first-ec #f (:range i 0) i))
(test-equal my-equal? 0
  (first-ec #f (:range i 1) i))
(test-equal my-equal? 0
  (first-ec #f (:range i 2) i))

(test-equal my-equal? 0
  (let ((last-i -1))
    (first-ec #f (:range i 10) (begin (set! last-i i)) i)
    last-i ))

(test-equal my-equal? 1
  (last-ec #f 1))
(test-equal my-equal? #f
  (last-ec #f (:range i 0) i))
(test-equal my-equal? 0
  (last-ec #f (:range i 1) i))
(test-equal my-equal? 1
  (last-ec #f (:range i 2) i))

(test-equal my-equal? #f
  (any?-ec #f))
(test-equal my-equal? #t
  (any?-ec #t))
(test-equal my-equal? #f
  (any?-ec (:range i 2 2) (even? i)))
(test-equal my-equal? #t
  (any?-ec (:range i 2 3) (even? i)))

(test-equal my-equal? #f
  (every?-ec #f))
(test-equal my-equal? #t
  (every?-ec #t))
(test-equal my-equal? #t
  (every?-ec (:range i 2 2) (even? i)))
(test-equal my-equal? #t
  (every?-ec (:range i 2 3) (even? i)))
(test-equal my-equal? #f
  (every?-ec (:range i 2 4) (even? i)))

(test-equal my-equal? 285
  (let ((sum-sqr (lambda (x result) (+ result (* x x)))))
   (fold-ec 0 (:range i 10) i sum-sqr) ))

(test-equal my-equal? 284
  (let ((minus-1 (lambda (x) (- x 1)))
       (sum-sqr (lambda (x result) (+ result (* x x)))))
   (fold3-ec (error "wrong") (:range i 10) i minus-1 sum-sqr) ))

(test-equal my-equal? 'infinity
  (fold3-ec 'infinity (:range i 0) i min min))


; ==========================================================================
; Typed generators
; ==========================================================================

(test-equal my-equal? '()
  (list-ec (:list x '()) x))
(test-equal my-equal? '(1)
  (list-ec (:list x '(1)) x))
(test-equal my-equal? '(1 2 3)
  (list-ec (:list x '(1 2 3)) x))
(test-equal my-equal? '(1 2)
  (list-ec (:list x '(1) '(2)) x))
(test-equal my-equal? '(1 2 3)
  (list-ec (:list x '(1) '(2) '(3)) x))

(test-equal my-equal? '()
  (list-ec (:string c "") c))
(test-equal my-equal? '(#\1)
  (list-ec (:string c "1") c))
(test-equal my-equal? '(#\1 #\2 #\3)
  (list-ec (:string c "123") c))
(test-equal my-equal? '(#\1 #\2)
  (list-ec (:string c "1" "2") c))
(test-equal my-equal? '(#\1 #\2 #\3)
  (list-ec (:string c "1" "2" "3") c))

(test-equal my-equal? '()
  (list-ec (:vector x (vector)) x))
(test-equal my-equal? '(1)
  (list-ec (:vector x (vector 1)) x))
(test-equal my-equal? '(1 2 3)
  (list-ec (:vector x (vector 1 2 3)) x))
(test-equal my-equal? '(1 2)
  (list-ec (:vector x (vector 1) (vector 2)) x))
(test-equal my-equal? '(1 2 3)
  (list-ec (:vector x (vector 1) (vector 2) (vector 3)) x))

(test-equal my-equal? '()
  (list-ec (:range x -2) x))
(test-equal my-equal? '()
  (list-ec (:range x -1) x))
(test-equal my-equal? '()
  (list-ec (:range x  0) x))
(test-equal my-equal? '(0)
  (list-ec (:range x  1) x))
(test-equal my-equal? '(0 1)
  (list-ec (:range x  2) x))

(test-equal my-equal? '(0 1 2)
  (list-ec (:range x  0  3) x))
(test-equal my-equal? '(1 2)
  (list-ec (:range x  1  3) x))
(test-equal my-equal? '(-2)
  (list-ec (:range x -2 -1) x))
(test-equal my-equal? '()
  (list-ec (:range x -2 -2) x))

(test-equal my-equal? '(1 3)
  (list-ec (:range x 1 5  2) x))
(test-equal my-equal? '(1 3 5)
  (list-ec (:range x 1 6  2) x))
(test-equal my-equal? '(5 3)
  (list-ec (:range x 5 1 -2) x))
(test-equal my-equal? '(6 4 2)
  (list-ec (:range x 6 1 -2) x))

(test-equal my-equal? '(0. 1. 2.)
  (list-ec (:real-range x 0.0 3.0)     x))
(test-equal my-equal? '(0. 1. 2.)
  (list-ec (:real-range x 0   3.0)     x))
(test-equal my-equal? '(0. 1. 2.)
  (list-ec (:real-range x 0   3   1.0) x))

(test-equal my-equal? "abcdefghijklmnopqrstuvwxyz"
  (string-ec (:char-range c #\a #\z) c))

(test-equal my-equal? (list-ec (:range n 10) n)
  (begin
   (let ((f (my-open-output-file "tmp1")))
     (do-ec (:range n 10) (begin (write n f) (newline f)))
     (close-output-port f))
   (my-call-with-input-file "tmp1"
    (lambda (port) (list-ec (:port x port read) x)) )))

(test-equal my-equal? (list-ec (:range n 10) n)
  (begin
   (let ((f (my-open-output-file "tmp1")))
     (do-ec (:range n 10) (begin (write n f) (newline f)))
     (close-output-port f))
   (my-call-with-input-file "tmp1"                 
     (lambda (port) (list-ec (:port x port) x)) )))


; ==========================================================================
; The special generators :do :let :parallel :while :until
; ==========================================================================

(test-equal my-equal? '(0 1 2 3)
  (list-ec (:do ((i 0)) (< i 4) ((+ i 1))) i))

(test-equal my-equal? '(10 9 8 7)
  (list-ec 
  (:do (let ((x 'x)))
       ((i 0)) 
       (< i 4) 
       (let ((j (- 10 i))))
       #t
       ((+ i 1)) )
  j ))

(test-equal my-equal? '(1)
  (list-ec (:let x 1) x))
(test-equal my-equal? '(2)
  (list-ec (:let x 1) (:let y (+ x 1)) y))
(test-equal my-equal? '(2)
  (list-ec (:let x 1) (:let x (+ x 1)) x))

(test-equal my-equal? '((1 a) (2 b) (3 c))
  (list-ec (:parallel (:range i 1 10) (:list x '(a b c))) (list i x)))

(test-equal my-equal? '(1 2 3 4)
  (list-ec (:while (:range i 1 10) (< i 5)) i))

(test-equal my-equal? '(1 2 3 4 5)
  (list-ec (:until (:range i 1 10) (>= i 5)) i))

; with generator that might use inner bindings

(test-equal my-equal? '(1 2 3 4)
  (list-ec (:while (:list i '(1 2 3 4 5 6 7 8 9)) (< i 5)) i))
; Was broken in original reference implementation as pointed
; out by sunnan@handgranat.org on 24-Apr-2005 comp.lang.scheme.
; Refer to http://groups-beta.google.com/group/comp.lang.scheme/
; browse_thread/thread/f5333220eaeeed66/75926634cf31c038#75926634cf31c038

(test-equal my-equal? '(1 2 3 4 5)
  (list-ec (:until (:list i '(1 2 3 4 5 6 7 8 9)) (>= i 5)) i))

(test-equal my-equal? '(1 2 3 4 5)
  (list-ec (:while (:vector x (index i) '#(1 2 3 4 5))
		  (< x 10))
	  x))
; Was broken in reference implementation, even after fix for the
; bug reported by Sunnan, as reported by Jens-Axel Soegaard on
; 4-Jun-2007.

; combine :while/:until and :parallel

(test-equal my-equal? '((1 1) (2 2) (3 3) (4 4))
  (list-ec (:while (:parallel (:range i 1 10)
                             (:list j '(1 2 3 4 5 6 7 8 9)))
                  (< i 5))
          (list i j)))

(test-equal my-equal? '((1 1) (2 2) (3 3) (4 4) (5 5))
  (list-ec (:until (:parallel (:range i 1 10)
                             (:list j '(1 2 3 4 5 6 7 8 9)))
                  (>= i 5))
          (list i j)))

; check that :while/:until really stop the generator

(test-equal my-equal? 5
  (let ((n 0))
   (do-ec (:while (:range i 1 10) (begin (set! n (+ n 1)) (< i 5)))
          (if #f #f))
   n))

(test-equal my-equal? 5
  (let ((n 0))
   (do-ec (:until (:range i 1 10) (begin (set! n (+ n 1)) (>= i 5)))
          (if #f #f))
   n))

(test-equal my-equal? 5
  (let ((n 0))
   (do-ec (:while (:parallel (:range i 1 10)
                             (:do () (begin (set! n (+ n 1)) #t) ()))
                  (< i 5))
          (if #f #f))
   n))

(test-equal my-equal? 5
  (let ((n 0))
   (do-ec (:until (:parallel (:range i 1 10)
                             (:do () (begin (set! n (+ n 1)) #t) ()))
                  (>= i 5))
          (if #f #f))
   n))

; ==========================================================================
; The dispatching generator
; ==========================================================================

(test-equal my-equal? '(a b)
  (list-ec (: c '(a b)) c))
(test-equal my-equal? '(a b c d)
  (list-ec (: c '(a b) '(c d)) c))

(test-equal my-equal? '(#\a #\b)
  (list-ec (: c "ab") c))
(test-equal my-equal? '(#\a #\b #\c #\d)
  (list-ec (: c "ab" "cd") c))

(test-equal my-equal? '(a b)
  (list-ec (: c (vector 'a 'b)) c))
(test-equal my-equal? '(a b c)
  (list-ec (: c (vector 'a 'b) (vector 'c)) c))

(test-equal my-equal? '()
  (list-ec (: i 0) i))
(test-equal my-equal? '(0)
  (list-ec (: i 1) i))
(test-equal my-equal? '(0 1 2 3 4 5 6 7 8 9)
  (list-ec (: i 10) i))
(test-equal my-equal? '(1)
  (list-ec (: i 1 2) i))
(test-equal my-equal? '(1)
  (list-ec (: i 1 2 3) i))
(test-equal my-equal? '(1 4 7)
  (list-ec (: i 1 9 3) i))

(test-equal my-equal? '(0. 0.2 0.4 0.6 0.8)
  (list-ec (: i 0.0 1.0 0.2) i))

(test-equal my-equal? '(#\a #\b #\c)
  (list-ec (: c #\a #\c) c))

(test-equal my-equal? (list-ec (:range n 10) n)
  (begin
   (let ((f (my-open-output-file "tmp1")))
     (do-ec (:range n 10) (begin (write n f) (newline f)))
     (close-output-port f))
   (my-call-with-input-file "tmp1"                 
     (lambda (port) (list-ec (: x port read) x)) )))
    
(test-equal my-equal? (list-ec (:range n 10) n)
  (begin
   (let ((f (my-open-output-file "tmp1")))
     (do-ec (:range n 10) (begin (write n f) (newline f)))
     (close-output-port f))
   (my-call-with-input-file "tmp1"                 
     (lambda (port) (list-ec (: x port) x)) )))


; ==========================================================================
; With index variable
; ==========================================================================

(test-equal my-equal? '((a 0) (b 1))
  (list-ec (:list c (index i) '(a b)) (list c i)))
(test-equal my-equal? '((#\a 0))
  (list-ec (:string c (index i) "a") (list c i)))
(test-equal my-equal? '((a 0))
  (list-ec (:vector c (index i) (vector 'a)) (list c i)))

(test-equal my-equal? '((0 0) (-1 1) (-2 2))
  (list-ec (:range i (index j) 0 -3 -1) (list i j)))

(test-equal my-equal? '((0. 0) (0.2 1) (0.4 2) (0.6 3) (0.8 4))
  (list-ec (:real-range i (index j) 0 1 0.2) (list i j)))

(test-equal my-equal? '((#\a 0) (#\b 1) (#\c 2))
  (list-ec (:char-range c (index i) #\a #\c) (list c i)))

(test-equal my-equal? '((a 0) (b 1) (c 2) (d 3))
  (list-ec (: x (index i) '(a b c d)) (list x i)))

(test-equal my-equal? '((0 0) (1 1) (2 2) (3 3) (4 4) (5 5) (6 6) (7 7) (8 8) (9 9))
  (begin
   (let ((f (my-open-output-file "tmp1")))
     (do-ec (:range n 10) (begin (write n f) (newline f)))
     (close-output-port f))
   (my-call-with-input-file "tmp1"
     (lambda (port) (list-ec (: x (index i) port) (list x i))) )))


; ==========================================================================
; The examples from the SRFI document
; ==========================================================================

; from Abstract

(test-equal my-equal? '(0 1 4 9 16)
  (list-ec (: i 5) (* i i)))

(test-equal my-equal? '((1 0) (2 0) (2 1) (3 0) (3 1) (3 2))
  (list-ec (: n 1 4) (: i n) (list n i)))

; from Generators

(test-equal my-equal? '((#\a 0) (#\b 1) (#\c 2))
  (list-ec (: x (index i) "abc") (list x i)))

(test-equal my-equal? '((#\a . 0) (#\b . 1))
  (list-ec (:string c (index i) "a" "b") (cons c i)))


; ==========================================================================
; Little Shop of Horrors
; ==========================================================================

(test-equal my-equal? '(0 0 1 0 1 2 0 1 2 3)
  (list-ec (:range x 5) (:range x x) x))

(test-equal my-equal? '(0 1 #\2 #\3 4)
  (list-ec (:list x '(2 "23" (4))) (: y x) y))

(test-equal my-equal? '((0 10) (1 9) (2 8) (3 7) (4 6))
  (list-ec (:parallel (:integers x) 
                     (:do ((i 10)) (< x i) ((- i 1))))
          (list x i)))


; ==========================================================================
; Less artificial examples
; ==========================================================================

(define (factorial n) ; n * (n-1) * .. * 1 for n >= 0
  (product-ec (:range k 2 (+ n 1)) k) )

(test-equal my-equal? 1
  (factorial  0))
(test-equal my-equal? 1
  (factorial  1))
(test-equal my-equal? 6
  (factorial  3))
(test-equal my-equal? 120
  (factorial  5))


(define (eratosthenes n) ; primes in {2..n-1} for n >= 1
  (let ((p? (make-string n #\1)))
    (do-ec (:range k 2 n)
           (if (char=? (string-ref p? k) #\1))
           (:range i (* 2 k) n k)
           (string-set! p? i #\0) )
    (list-ec (:range k 2 n) (if (char=? (string-ref p? k) #\1)) k) ))

(test-equal my-equal? '(2 3 5 7 11 13 17 19 23 29 31 37 41 43 47)
  (eratosthenes 50))

(test-equal my-equal? 9592
  (length (eratosthenes 100000))) ; we expect 10^5/ln(10^5)


(define (pythagoras n) ; a, b, c s.t. 1 <= a <= b <= c <= n, a^2 + b^2 = c^2
  (list-ec 
   (:let sqr-n (* n n))
   (:range a 1 (+ n 1))
; (begin (display a) (display " "))
   (:let sqr-a (* a a))
   (:range b a (+ n 1)) 
   (:let sqr-c (+ sqr-a (* b b)))
   (if (<= sqr-c sqr-n))
   (:range c b (+ n 1))
   (if (= (* c c) sqr-c))
   (list a b c) ))
           
(test-equal my-equal? '((3 4 5) (5 12 13) (6 8 10) (9 12 15))
  (pythagoras 15))

(test-equal my-equal? 127
  (length (pythagoras 200)))


(define (qsort xs) ; stable
  (if (null? xs)
      '()
      (let ((pivot (car xs)) (xrest (cdr xs)))
        (append
         (qsort (list-ec (:list x xrest) (if (<  x pivot)) x))
         (list pivot)
         (qsort (list-ec (:list x xrest) (if (>= x pivot)) x)) ))))

(test-equal my-equal? '(1 1 2 2 3 3 4 4 5 5)
  (qsort '(1 5 4 2 4 5 3 2 1 3)))


(define (pi-BBP m) ; approx. of pi within 16^-m (Bailey-Borwein-Plouffe)
  (sum-ec 
    (:range n 0 (+ m 1))
    (:let n8 (* 8 n))
    (* (- (/ 4 (+ n8 1))
          (+ (/ 2 (+ n8 4))
             (/ 1 (+ n8 5))
             (/ 1 (+ n8 6))))
       (/ 1 (expt 16 n)) )))

(test-equal my-equal? (/ 40413742330349316707 12864093722915635200)
  (pi-BBP 5))


(define (read-line port) ; next line (incl. #\newline) of port
  (let ((line
         (string-ec 
          (:until (:port c port read-char)
                  (char=? c #\newline) )
          c )))
    (if (string=? line "")
        (read-char port) ; eof-object
        line )))

(define (read-lines filename) ; list of all lines
  (my-call-with-input-file
   filename
   (lambda (port)
     (list-ec (:port line port read-line) line) )))

(test-equal my-equal? (list-ec (:char-range c #\0 #\9) (string c #\newline))
  (begin
   (let ((f (my-open-output-file "tmp1")))
     (do-ec (:range n 10) (begin (write n f) (newline f)))
     (close-output-port f))
   (read-lines "tmp1") ))

(test-end)
