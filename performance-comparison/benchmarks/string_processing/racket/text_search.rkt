#lang racket
(define hs-size (string->number (vector-ref (current-command-line-arguments) 0)))
(define needle "hello")
(define hay
  (list->string
   (for/list ([i (in-range hs-size)])
     (if (< (modulo i 10) 5) #\x (string-ref needle (- (modulo i 10) 5))))))
(define count
  (let loop ([pos 0] [c 0])
    (define i (let search ([p pos])
                (if (> (+ p 5) (string-length hay)) -1
                    (if (equal? (substring hay p (+ p 5)) needle) p
                        (search (+ p 1))))))
    (if (< i 0) c (loop (+ i 5) (+ c 1)))))
(displayln count)
