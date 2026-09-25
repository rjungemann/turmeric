;; Included at top level: a definition, and a set! of the includer's global.
(define (twice x) (* x 2))
(define (bump!) (set! counter (+ counter 1)) counter)
