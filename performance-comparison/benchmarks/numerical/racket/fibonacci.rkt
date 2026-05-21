#lang racket
(define (fib n)
  (let loop ([i 2] [a 0] [b 1])
    (if (> i n) b
        (loop (+ i 1) b (+ a b)))))

(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(displayln (fib n))
