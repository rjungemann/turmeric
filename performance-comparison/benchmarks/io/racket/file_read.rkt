#lang racket
(define n    (string->number (vector-ref (current-command-line-arguments) 0)))
(define path "/tmp/bench_io_read_rkt.bin")
;; write
(define wbuf (make-bytes 4096 #xCD))
(define out  (open-output-file path #:exists 'replace))
(let loop ([rem n])
  (when (> rem 0)
    (define chunk (min 4096 rem))
    (write-bytes (subbytes wbuf 0 chunk) out)
    (loop (- rem chunk))))
(close-output-port out)
;; read
(define in (open-input-file path))
(define rbuf (make-bytes 4096))
(define total
  (let loop ([t 0])
    (define nr (read-bytes! rbuf in))
    (if (eof-object? nr) t (loop (+ t nr)))))
(close-input-port in)
(delete-file path)
(displayln total)
