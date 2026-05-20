#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define s (apply string-append (make-list n "hello")))
(displayln (string-length s))
