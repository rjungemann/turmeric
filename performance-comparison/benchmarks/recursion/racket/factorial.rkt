#lang racket
(define MOD 1000000007)
(define (factorial n acc)
  (if (<= n 1) (modulo acc MOD)
      (factorial (- n 1) (modulo (* acc n) MOD))))

(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(displayln (factorial n 1))
