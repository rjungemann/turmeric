#lang racket
(define (make-matrix n val)
  (build-vector n (lambda (_) (make-vector n val))))

(define (matmul A B n)
  (define C (make-matrix n 0.0))
  (for ([i (in-range n)])
    (for ([k (in-range n)])
      (for ([j (in-range n)])
        (vector-set! (vector-ref C i) j
          (+ (vector-ref (vector-ref C i) j)
             (* (vector-ref (vector-ref A i) k)
                (vector-ref (vector-ref B k) j)))))))
  C)

(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define A (make-matrix n 1.0))
(define B (make-matrix n 1.0))
(define C (matmul A B n))
(define s (for/sum ([row C]) (for/sum ([v row]) v)))
(displayln (exact->inexact s))
