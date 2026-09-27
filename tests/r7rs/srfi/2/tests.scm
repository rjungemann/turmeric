;;; tests/r7rs/srfi/2/tests.scm -- SRFI 2's tests: chibi-scheme's
;;; lib/srfi/2/test.sld, its run-tests body lifted out as top-level forms for
;;; tests/r7rs/run-conformance.py (r7rs-srfi-plan D7).  Copyright (c)
;;; 2009-2021 Alex Shinn; BSD-3, see tests/r7rs/CHIBI-COPYING.
(test-begin "srfi-2: and-let*")
(test 1 (and-let* () 1))
(test 2 (and-let* () 1 2))
(test #t (and-let* () ))
(test #f (let ((x #f)) (and-let* (x))))
(test 1 (let ((x 1)) (and-let* (x))))
(test #f (and-let* ((x #f)) ))
(test 1 (and-let* ((x 1)) ))
;; (test-syntax-error (and-let* ( #f (x 1))))
(test #f (and-let* ( (#f) (x 1)) ))
;; (test-syntax-error (and-let* (2 (x 1))))
(test 1 (and-let* ( (2) (x 1)) ))
(test 2 (and-let* ( (x 1) (2)) ))
(test #f (let ((x #f)) (and-let* (x) x)))
(test "" (let ((x "")) (and-let* (x) x)))
(test "" (let ((x "")) (and-let* (x)  )))
(test 2 (let ((x 1)) (and-let* (x) (+ x 1))))
(test #f (let ((x #f)) (and-let* (x) (+ x 1))))
(test 2 (let ((x 1)) (and-let* (((positive? x))) (+ x 1))))
(test #t (let ((x 1)) (and-let* (((positive? x))) )))
(test #f (let ((x 0)) (and-let* (((positive? x))) (+ x 1))))
(test 3  (let ((x 1)) (and-let* (((positive? x)) (x (+ x 1))) (+ x 1))))
(test 4
    (let ((x 1))
      (and-let* (((positive? x)) (x (+ x 1)) (x (+ x 1))) (+ x 1))))
(test 2 (let ((x 1)) (and-let* (x ((positive? x))) (+ x 1))))
(test 2 (let ((x 1)) (and-let* ( ((begin x)) ((positive? x))) (+ x 1))))
(test #f (let ((x 0)) (and-let* (x ((positive? x))) (+ x 1))))
(test #f (let ((x #f)) (and-let* (x ((positive? x))) (+ x 1))))
(test #f (let ((x #f)) (and-let* ( ((begin x)) ((positive? x))) (+ x 1))))

(test #f
    (let ((x 1)) (and-let* (x (y (- x 1)) ((positive? y))) (/ x y))))
(test #f
    (let ((x 0)) (and-let* (x (y (- x 1)) ((positive? y))) (/ x y))))
(test #f
    (let ((x #f)) (and-let* (x (y (- x 1)) ((positive? y))) (/ x y))))
(test 3/2
    (let ((x 3)) (and-let* (x (y (- x 1)) ((positive? y))) (/ x y))))
(test 5 (and-let* () (define x 5) x))
(test 6 (and-let* ((x 6)) (define y x) y))
(test-end)
