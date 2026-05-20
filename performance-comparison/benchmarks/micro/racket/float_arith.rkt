#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define result
  (let loop ([i 0] [a 1.0] [b 1.0])
    (if (>= i n) (+ a b)
        (loop (+ i 1)
              (+ (* a 1.0000001) (sqrt b))
              (+ (* b 0.9999999) (sqrt a))))))
(printf "~a\n" (real->decimal-string result 6))
