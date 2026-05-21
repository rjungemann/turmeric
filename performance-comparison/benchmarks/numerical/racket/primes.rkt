#lang racket
(define (count-primes limit)
  (define sieve (make-vector (+ limit 1) #f))
  (let loop ([i 2] [count 0])
    (cond
      [(> i limit) count]
      [(vector-ref sieve i) (loop (+ i 1) count)]
      [else
       (let inner ([j (* 2 i)])
         (when (<= j limit)
           (vector-set! sieve j #t)
           (inner (+ j i))))
       (loop (+ i 1) (+ count 1))])))

(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(displayln (count-primes n))
