#lang racket
(define file-size (string->number (vector-ref (current-command-line-arguments) 0)))
(define n-reads   (string->number (vector-ref (current-command-line-arguments) 1)))
(define path "/tmp/bench_io_random_rkt.bin")

;; write deterministic data
(define out (open-output-file path #:exists 'replace))
(let loop ([pos 0])
  (when (< pos file-size)
    (define chunk (min 4096 (- file-size pos)))
    (write-bytes (build-string chunk (lambda (i) (integer->char (bitwise-and (+ pos i) 255)))) out)
    (loop (+ pos chunk))))
(close-output-port out)

;; random reads
(define in (open-input-file path))
(define checksum
  (let loop ([i 0] [state 12345678] [cs 0])
    (if (>= i n-reads) cs
        (let* ([state2 (bitwise-and (+ (* state 6364136223846793005) 1442695040888963407)
                                    #xFFFFFFFFFFFFFFFF)]
               [offset (modulo (arithmetic-shift state2 -1) file-size)])
          (file-position in offset)
          (loop (+ i 1) state2 (+ cs (read-byte in)))))))
(close-input-port in)
(delete-file path)
(displayln checksum)
