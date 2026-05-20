#lang racket
(define n-threads (string->number (vector-ref (current-command-line-arguments) 0)))
(define messages  (string->number (vector-ref (current-command-line-arguments) 1)))

;; Adjust so token reaches 0 at thread 0
(define adj   (modulo (- n-threads (modulo messages n-threads)) n-threads))
(define total (+ messages adj))

(define chans (for/vector ([_ (in-range n-threads)]) (make-channel)))
(define done  (make-semaphore 0))

(for ([i (in-range n-threads)])
  (define me   (vector-ref chans i))
  (define next (vector-ref chans (modulo (+ i 1) n-threads)))
  (thread (lambda ()
    (let loop ()
      (define tok (channel-get me))
      (cond
        [(<= tok 0)
         (when (= i 0) (semaphore-post done))
         (void)]
        [else
         (channel-put next (- tok 1))
         (loop)])))))

(channel-put (vector-ref chans 0) total)
(semaphore-wait done)
(displayln "done")
