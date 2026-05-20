#lang racket
(define width  (string->number (vector-ref (current-command-line-arguments) 0)))
(define height (string->number (vector-ref (current-command-line-arguments) 1)))

(define spheres (list (list 0 0 -5 1.0) (list 2 0 -7 1.5) (list -3 0 -6 0.8)))

(define (dot ax ay az bx by bz) (+ (* ax bx) (* ay by) (* az bz)))
(define (norm x y z)
  (define l (+ (sqrt (dot x y z x y z)) 1e-15))
  (values (/ x l) (/ y l) (/ z l)))
(define-values (lx ly lz) (norm 1 1 -1))

(define (sphere-hit cx cy cz r ox oy oz dx dy dz)
  (define ocx (- ox cx)) (define ocy (- oy cy)) (define ocz (- oz cz))
  (define a  (dot dx dy dz dx dy dz))
  (define b2 (dot ocx ocy ocz dx dy dz))
  (define c  (- (dot ocx ocy ocz ocx ocy ocz) (* r r)))
  (define d  (- (* b2 b2) (* a c)))
  (and (>= d 0)
       (let ([t (/ (- (- b2) (sqrt d)) a)])
         (and (> t 0.001) t))))

(define checksum
  (for*/fold ([cs 0]) ([y (in-range height)] [x (in-range width)])
    (define-values (dx dy dz) (norm (- (* (/ (exact->inexact x) width) 2) 1)
                                    (- (* (/ (exact->inexact y) height) 2) 1)
                                    -1))
    (define best
      (for/fold ([best #f]) ([s spheres])
        (define t (sphere-hit (first s) (second s) (third s) (fourth s) 0 0 0 dx dy dz))
        (if (and t (or (not best) (< t (car best))))
            (cons t s) best)))
    (if best
        (let* ([t  (car best)] [s (cdr best)]
               [hx (* dx t)] [hy (* dy t)] [hz (* dz t)]
               [nx (- hx (first s))] [ny (- hy (second s))] [nz (- hz (third s))])
          (define-values (nnx nny nnz) (norm nx ny nz))
          (+ cs (inexact->exact (floor (* (max 0.0 (dot nnx nny nnz lx ly lz)) 255)))))
        cs)))
(displayln checksum)
