#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define sum
  (for/fold ([s 0]) ([i (in-range n)])
    (define b (box i))
    (+ s (unbox b))))
(displayln sum)
