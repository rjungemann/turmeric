#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define MASK #xFFFFFFFFFFFFFFFF)
(define result
  (let loop ([i 0] [a 1] [b 1])
    (if (>= i n) (bitwise-xor a b)
        (let ([new-a (bitwise-and (+ (* a 1000003) b) MASK)])
          (loop (+ i 1)
                new-a
                (bitwise-and (+ (* b 999983) new-a) MASK))))))
; Match C's printf("%lld"): convert unsigned 64-bit to signed
(define signed-result
  (if (>= result (expt 2 63)) (- result (expt 2 64)) result))
(displayln signed-result)
