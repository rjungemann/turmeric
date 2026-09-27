;;; tests/r7rs/srfi/26/tests.scm -- SRFI 26's tests, for
;;; tests/r7rs/run-conformance.py (r7rs-srfi-plan D7):
;;;
;;; - chibi-scheme's lib/srfi/26/test.sld, its run-tests body lifted out.
;;;   Copyright (c) 2009-2021 Alex Shinn; BSD-3, see tests/r7rs/CHIBI-COPYING.
;;; - the SRFI's reference check.scm (Sebastian Egner, public domain), each
;;;   `(check '<expr>)` written as `(test #t <expr>)`: check.scm evaluates
;;;   its list through `eval`, which would test `eval`'s environment rather
;;;   than the import.
(test-begin "srfi-26: cut")
(let ((x 'orig))
  (let ((f (cute list x)))
    (set! x 'wrong)
    (test '(orig) (f))))
(let ((x 'wrong))
  (let ((f (cut list x)))
    (set! x 'right)
    (test '(right) (f))))
(test #t (equal? ((cut list)) '()))
(test #t (equal? ((cut list <...>)) '()))
(test #t (equal? ((cut list 1)) '(1)))
(test #t (equal? ((cut list <>) 1) '(1)))
(test #t (equal? ((cut list <...>) 1) '(1)))
(test #t (equal? ((cut list 1 2)) '(1 2)))
(test #t (equal? ((cut list 1 <>) 2) '(1 2)))
(test #t (equal? ((cut list 1 <...>) 2) '(1 2)))
(test #t (equal? ((cut list 1 <...>) 2 3 4) '(1 2 3 4)))
(test #t (equal? ((cut list 1 <> 3 <>) 2 4) '(1 2 3 4)))
(test #t (equal? ((cut list 1 <> 3 <...>) 2 4 5 6) '(1 2 3 4 5 6)))
(test #t (equal? (let* ((x 'wrong) (y (cut list x))) (set! x 'ok) (y)) '(ok)))
(test #t (equal? (let ((a 0)) (map (cut + (begin (set! a (+ a 1)) a) <>) '(1 2)) a) 2))
(test #t (equal? ((cute list)) '()))
(test #t (equal? ((cute list <...>)) '()))
(test #t (equal? ((cute list 1)) '(1)))
(test #t (equal? ((cute list <>) 1) '(1)))
(test #t (equal? ((cute list <...>) 1) '(1)))
(test #t (equal? ((cute list 1 2)) '(1 2)))
(test #t (equal? ((cute list 1 <>) 2) '(1 2)))
(test #t (equal? ((cute list 1 <...>) 2) '(1 2)))
(test #t (equal? ((cute list 1 <...>) 2 3 4) '(1 2 3 4)))
(test #t (equal? ((cute list 1 <> 3 <>) 2 4) '(1 2 3 4)))
(test #t (equal? ((cute list 1 <> 3 <...>) 2 4 5 6) '(1 2 3 4 5 6)))
(test-end)
