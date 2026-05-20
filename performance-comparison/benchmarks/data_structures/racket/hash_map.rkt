#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define ht (make-hash))
(for ([i (in-range n)]) (hash-set! ht i (* i 2)))
(displayln (for/sum ([i (in-range n)]) (hash-ref ht i)))
