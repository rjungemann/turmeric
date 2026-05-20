#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define result
  (let loop ([i 0] [a 1] [b 1])
    (if (>= i n) (bitwise-xor a b)
        (loop (+ i 1)
              (+ (* a 1000003) b)
              (+ (* b 999983) a)))))
(displayln result)
