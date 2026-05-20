#lang racket

(define (recursive-fib n)
  (if (<= n 1)
      n
      (+ (recursive-fib (- n 1))
         (recursive-fib (- n 2)))))

(define (iterative-fib n)
  (if (<= n 1)
      n
      (let loop ([a 0] [b 1] [i 2])
        (if (> i n)
            b
            (loop b (+ a b) (+ i 1))))))

(define (current-time-ms)
  (inexact->exact (floor (* 1000 (current-inexact-milliseconds)))))

(define (main args)
  (let ([n (string->number (list-ref args 0))])
    (let ([start (current-time-ms)]
          [result (recursive-fib n)]
          [time (- (current-time-ms) start)])
      (printf "{"language": "Racket", "version": "~a", "implementation": "recursive", "n": ~a, "result": ~a, "time_ms": ~a}~n"
              (version) n result time))

    (let ([start (current-time-ms)]
          [result (iterative-fib n)]
          [time (- (current-time-ms) start)])
      (printf "{"language": "Racket", "version": "~a", "implementation": "iterative", "n": ~a, "result": ~a, "time_ms": ~a}~n"
              (version) n result time))))

(main (command-line-arguments))