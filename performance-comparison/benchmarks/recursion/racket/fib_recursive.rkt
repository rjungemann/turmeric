#lang racket
(define (fib n)
  (if (<= n 1) n
      (+ (fib (- n 1)) (fib (- n 2)))))

(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(displayln (fib n))
