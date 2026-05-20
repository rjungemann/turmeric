#lang racket
(define n (string->number (vector-ref (current-command-line-arguments) 0)))
(define arr
  (let loop ([i 0] [s 12345] [acc '()])
    (if (>= i n) acc
        (let ([ns (bitwise-and (+ (* s 6364136223846793005)
                                  1442695040888963407) #xFFFFFFFFFFFFFFFF)])
          (loop (+ i 1) ns (cons (arithmetic-shift ns -1) acc))))))
(define sorted (sort arr <))
(printf "~a ~a\n" (first sorted) (last sorted))
