;;; tests/r7rs/srfi/66/tests.scm -- SRFI 66's tests, as top-level forms for
;;; tests/r7rs/run-conformance.py (r7rs-srfi-plan D7).  SRFI 66 ships no
;;; suite, so this checks each procedure against its document (MIT, as the
;;; SRFI): the octet range, u8vector-compare's ordering (shorter is smaller,
;;; then lexicographic), and u8vector-copy!'s R6RS argument order and
;;; overlapping copies.  The octet vector is R7RS's bytevector.

(test-begin "srfi-66: octet vectors")

(test #t (u8vector? (u8vector)))
(test #t (u8vector? (bytevector 1)))
(test #f (u8vector? (vector 1)))
(test #f (u8vector? "abc"))
(test '(7 7 7) (u8vector->list (make-u8vector 3 7)))
(test 3 (u8vector-length (make-u8vector 3 0)))
(test '(0 1 255) (u8vector->list (u8vector 0 1 255)))
(test '() (u8vector->list (u8vector)))
(test (bytevector 1 2 3) (list->u8vector '(1 2 3)))
(test '(4 5) (u8vector->list (list->u8vector '(4 5))))
(test 3 (u8vector-length (u8vector 1 2 3)))
(test 0 (u8vector-length (u8vector)))
(test 2 (u8vector-ref (u8vector 1 2 3) 1))
(test '(1 9 3) (let ((v (u8vector 1 2 3)))
                 (u8vector-set! v 1 9)
                 (u8vector->list v)))

;; the octet range, and the type
(test-error (u8vector 256))
(test-error (u8vector -1))
(test-error (list->u8vector '(1 2.0)))
(test-error (make-u8vector 2 300))
(test-error (u8vector-set! (u8vector 1) 0 256))
(test-error (u8vector-ref (u8vector 1) 1))
(test-error (u8vector-ref (vector 1) 0))
(test-error (u8vector-length '(1 2)))

;; u8vector=? and u8vector-compare
(test #t (u8vector=? (u8vector 1 2) (u8vector 1 2)))
(test #f (u8vector=? (u8vector 1 2) (u8vector 1 3)))
(test #f (u8vector=? (u8vector 1 2) (u8vector 1 2 0)))
(test #t (u8vector=? (u8vector) (u8vector)))
(test 0 (u8vector-compare (u8vector 1 2) (u8vector 1 2)))
(test -1 (u8vector-compare (u8vector 1 2) (u8vector 1 3)))
(test 1 (u8vector-compare (u8vector 1 3) (u8vector 1 2)))
(test -1 (u8vector-compare (u8vector 9 9) (u8vector 0 0 0)))
(test 1 (u8vector-compare (u8vector 0 0 0) (u8vector 9 9)))
(test -1 (u8vector-compare (u8vector) (u8vector 0)))
(test 1 (u8vector-compare (u8vector 255) (u8vector 0)))

;; u8vector-copy! (source source-start target target-start n)
(test '(0 3 4 0 0)
      (let ((t (make-u8vector 5 0)))
        (u8vector-copy! (u8vector 1 2 3 4 5) 2 t 1 2)
        (u8vector->list t)))
(test '(1 1 2 3 4)
      (let ((v (u8vector 1 2 3 4 5)))
        (u8vector-copy! v 0 v 1 4)
        (u8vector->list v)))
(test '(2 3 4 5 5)
      (let ((v (u8vector 1 2 3 4 5)))
        (u8vector-copy! v 1 v 0 4)
        (u8vector->list v)))
(test '(1 2 3)
      (let ((v (u8vector 1 2 3)))
        (u8vector-copy! v 0 v 0 0)
        (u8vector->list v)))
(test-error (u8vector-copy! (u8vector 1 2) 1 (make-u8vector 2 0) 0 2))

;; u8vector-copy is a fresh copy
(test '((1 2 3) (9 2 3))
      (let* ((v (u8vector 1 2 3))
             (c (u8vector-copy v)))
        (u8vector-set! c 0 9)
        (list (u8vector->list v) (u8vector->list c))))
(test #t (let ((v (u8vector 1)))
           (not (eq? v (u8vector-copy v)))))

(test-end)
