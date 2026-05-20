#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define lst
  (let loop ([i 0] [acc '()])
    (if (>= i n) acc
        (loop (+ i 1) (cons i acc)))))
(displayln (apply + lst))
