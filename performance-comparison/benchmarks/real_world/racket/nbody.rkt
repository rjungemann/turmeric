#lang racket
(define n-bodies (string->number (vector-ref (current-command-line-arguments) 0)))
(define steps    (string->number (vector-ref (current-command-line-arguments) 1)))

(define LCG-A 6364136223846793005)
(define LCG-C 1442695040888963407)
(define MASK #xFFFFFFFFFFFFFFFF)

(define (lcg s) (bitwise-and (+ (* s LCG-A) LCG-C) MASK))
(define (int32-val s) (/ (exact->inexact (bitwise-and (arithmetic-shift s -32) #xFFFFFFFF)) 1e8))

;; Initialize bodies
(define bodies
  (let loop ([i 0] [state 42] [acc '()])
    (if (>= i n-bodies) (list->vector (reverse acc))
        (let* ([s1 (lcg state)] [s2 (lcg s1)] [s3 (lcg s2)])
          (loop (+ i 1) s3
                (cons (vector (int32-val s1) (int32-val s2) (int32-val s3)
                               0.0 0.0 0.0
                               (+ 1.0 (* (modulo i 5) 0.5)))
                      acc))))))

(define (bx b) (vector-ref b 0)) (define (set-bx! b v) (vector-set! b 0 v))
(define (by b) (vector-ref b 1)) (define (set-by! b v) (vector-set! b 1 v))
(define (bz b) (vector-ref b 2)) (define (set-bz! b v) (vector-set! b 2 v))
(define (bvx b) (vector-ref b 3)) (define (set-bvx! b v) (vector-set! b 3 v))
(define (bvy b) (vector-ref b 4)) (define (set-bvy! b v) (vector-set! b 4 v))
(define (bvz b) (vector-ref b 5)) (define (set-bvz! b v) (vector-set! b 5 v))
(define (bmass b) (vector-ref b 6))

(for ([_ (in-range steps)])
  (for* ([i (in-range n-bodies)] [j (in-range (+ i 1) n-bodies)])
    (define bi (vector-ref bodies i)) (define bj (vector-ref bodies j))
    (define dx (- (bx bj) (bx bi))) (define dy (- (by bj) (by bi))) (define dz (- (bz bj) (bz bi)))
    (define dist (+ (sqrt (+ (* dx dx) (* dy dy) (* dz dz))) 1e-10))
    (define f (/ (* (bmass bi) (bmass bj)) (* dist dist dist)))
    (set-bvx! bi (+ (bvx bi) (* f dx))) (set-bvy! bi (+ (bvy bi) (* f dy))) (set-bvz! bi (+ (bvz bi) (* f dz)))
    (set-bvx! bj (- (bvx bj) (* f dx))) (set-bvy! bj (- (bvy bj) (* f dy))) (set-bvz! bj (- (bvz bj) (* f dz))))
  (for ([i (in-range n-bodies)])
    (define b (vector-ref bodies i))
    (set-bx! b (+ (bx b) (bvx b))) (set-by! b (+ (by b) (bvy b))) (set-bz! b (+ (bz b) (bvz b)))))

(define ke
  (for/fold ([s 0.0]) ([i (in-range n-bodies)])
    (define b (vector-ref bodies i))
    (+ s (* 0.5 (bmass b) (+ (* (bvx b) (bvx b)) (* (bvy b) (bvy b)) (* (bvz b) (bvz b)))))))
(printf "~a\n" (real->decimal-string ke 4))
