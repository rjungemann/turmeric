#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define (inc1 x) (+ x 1))
(define result
  (let loop ([i 0] [v 0])
    (if (>= i n) v
        (loop (+ i 1) (inc1 v)))))
(displayln result)
