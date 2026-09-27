;;; tests/r7rs/srfi/69/tests.scm -- SRFI 69's tests: chibi-scheme's
;;; lib/srfi/69/test.sld, its run-tests body lifted out as top-level forms for
;;; tests/r7rs/run-conformance.py (r7rs-srfi-plan D7), less one chibi-only test
;;; (noted where it stood).  The suite also imports (srfi 1), for lset=
;;; (./imports).  Copyright (c) 2009-2021 Alex Shinn; BSD-3, see
;;; tests/r7rs/CHIBI-COPYING.

(define-syntax test-lset-eq?
  (syntax-rules ()
    ((test-lset= . args)
     (test-equal (lambda (a b) (lset= eq? a b)) . args))))

(define-syntax test-lset-equal?
  (syntax-rules ()
    ((test-lset-equal? . args)
     (test-equal (lambda (a b) (lset= equal? a b)) . args))))

(test-begin "srfi-69: hash-tables")

(let ((ht (make-hash-table eq?)))
  ;; 3 initial elements
  (test 0 (hash-table-size ht))
  (hash-table-set! ht 'cat 'black)
  (hash-table-set! ht 'dog 'white)
  (hash-table-set! ht 'elephant 'pink)
  (test 3 (hash-table-size ht))
  (test-assert (hash-table-exists? ht 'dog))
  (test-assert (hash-table-exists? ht 'cat))
  (test-assert (hash-table-exists? ht 'elephant))
  (test-not (hash-table-exists? ht 'goose))
  (test 'white (hash-table-ref ht 'dog))
  (test 'black (hash-table-ref ht 'cat))
  (test 'pink (hash-table-ref ht 'elephant))
  (test-error (hash-table-ref ht 'goose))
  (test 'grey (hash-table-ref ht 'goose (lambda () 'grey)))
  (test 'grey (hash-table-ref/default ht 'goose 'grey))
  (test-lset-eq? '(cat dog elephant) (hash-table-keys ht))
  (test-lset-eq? '(black white pink) (hash-table-values ht))
  (test-lset-equal? '((cat . black) (dog . white) (elephant . pink))
                    (hash-table->alist ht))

  ;; remove an element
  (hash-table-delete! ht 'dog)
  (test 2 (hash-table-size ht))
  (test-not (hash-table-exists? ht 'dog))
  (test-assert (hash-table-exists? ht 'cat))
  (test-assert (hash-table-exists? ht 'elephant))
  (test-error (hash-table-ref ht 'dog))
  (test 'black (hash-table-ref ht 'cat))
  (test 'pink (hash-table-ref ht 'elephant))
  (test-lset-eq? '(cat elephant) (hash-table-keys ht))
  (test-lset-eq? '(black pink) (hash-table-values ht))
  (test-lset-equal? '((cat . black) (elephant . pink)) (hash-table->alist ht))

  ;; remove a non-existing element
  (hash-table-delete! ht 'dog)
  (test 2 (hash-table-size ht))
  (test-not (hash-table-exists? ht 'dog))

  ;; overwrite an existing element
  (hash-table-set! ht 'cat 'calico)
  (test 2 (hash-table-size ht))
  (test-not (hash-table-exists? ht 'dog))
  (test-assert (hash-table-exists? ht 'cat))
  (test-assert (hash-table-exists? ht 'elephant))
  (test-error (hash-table-ref ht 'dog))
  (test 'calico (hash-table-ref ht 'cat))
  (test 'pink (hash-table-ref ht 'elephant))
  (test-lset-eq? '(cat elephant) (hash-table-keys ht))
  (test-lset-eq? '(calico pink) (hash-table-values ht))
  (test-lset-equal? '((cat . calico) (elephant . pink)) (hash-table->alist ht))

  ;; walk and fold
  (test-lset-equal?
   '((cat . calico) (elephant . pink))
   (let ((a '()))
     (hash-table-walk ht (lambda (k v) (set! a (cons (cons k v) a))))
     a))
  (test-lset-equal? '((cat . calico) (elephant . pink))
                    (hash-table-fold ht (lambda (k v a) (cons (cons k v) a)) '()))

  ;; copy
  (let ((ht2 (hash-table-copy ht)))
    (test 2 (hash-table-size ht2))
    (test-not (hash-table-exists? ht2 'dog))
    (test-assert (hash-table-exists? ht2 'cat))
    (test-assert (hash-table-exists? ht2 'elephant))
    (test-error (hash-table-ref ht2 'dog))
    (test 'calico (hash-table-ref ht2 'cat))
    (test 'pink (hash-table-ref ht2 'elephant))
    (test-lset-eq? '(cat elephant) (hash-table-keys ht2))
    (test-lset-eq? '(calico pink) (hash-table-values ht2))
    (test-lset-equal? '((cat . calico) (elephant . pink))
                      (hash-table->alist ht2)))

  ;; merge
  (let ((ht2 (make-hash-table eq?)))
    (hash-table-set! ht2 'bear 'brown)
    (test 1 (hash-table-size ht2))
    (test-not (hash-table-exists? ht2 'dog))
    (test-assert (hash-table-exists? ht2 'bear))
    (hash-table-merge! ht2 ht)
    (test 3 (hash-table-size ht2))
    (test-assert (hash-table-exists? ht2 'bear))
    (test-assert (hash-table-exists? ht2 'cat))
    (test-assert (hash-table-exists? ht2 'elephant))
    (test-not (hash-table-exists? ht2 'goose))
    (test 'brown (hash-table-ref ht2 'bear))
    (test 'calico (hash-table-ref ht2 'cat))
    (test 'pink (hash-table-ref ht2 'elephant))
    (test-error (hash-table-ref ht2 'goose))
    (test 'grey (hash-table-ref/default ht2 'goose 'grey))
    (test-lset-eq? '(bear cat elephant) (hash-table-keys ht2))
    (test-lset-eq? '(brown calico pink) (hash-table-values ht2))
    (test-lset-equal? '((cat . calico) (bear . brown) (elephant . pink))
                      (hash-table->alist ht2)))

  ;; alist->hash-table
  (test-lset-equal? (hash-table->alist ht)
                    (hash-table->alist
                     (alist->hash-table
                      '((cat . calico) (elephant . pink))))))

;; update
(let ((ht (make-hash-table eq?))
      (add1 (lambda (x) (+ x 1))))
  (hash-table-set! ht 'sheep 0)
  (hash-table-update! ht 'sheep add1)
  (hash-table-update! ht 'sheep add1)
  (test 2 (hash-table-ref ht 'sheep))
  (hash-table-update!/default ht 'crows add1 0)
  (hash-table-update!/default ht 'crows add1 0)
  (hash-table-update!/default ht 'crows add1 0)
  (test 3 (hash-table-ref ht 'crows)))

;; string keys
(let ((ht (make-hash-table equal?)))
  (hash-table-set! ht "cat" 'black)
  (hash-table-set! ht "dog" 'white)
  (hash-table-set! ht "elephant" 'pink)
  (hash-table-ref/default ht "dog" #f)
  (test 'white (hash-table-ref ht "dog"))
  (test 'black (hash-table-ref ht "cat"))
  (test 'pink (hash-table-ref ht "elephant"))
  (test-error (hash-table-ref ht "goose"))
  (test 'grey (hash-table-ref/default ht "goose" 'grey))
  (test-lset-equal? '("cat" "dog" "elephant") (hash-table-keys ht))
  (test-lset-equal? '(black white pink) (hash-table-values ht))
  (test-lset-equal?
   '(("cat" . black) ("dog" . white) ("elephant" . pink))
   (hash-table->alist ht)))

;; string-ci keys
(let ((ht (make-hash-table string-ci=? string-ci-hash)))
  (hash-table-set! ht "cat" 'black)
  (hash-table-set! ht "dog" 'white)
  (hash-table-set! ht "elephant" 'pink)
  (hash-table-ref/default ht "DOG" #f)
  (test 'white (hash-table-ref ht "DOG"))
  (test 'black (hash-table-ref ht "Cat"))
  (test 'pink (hash-table-ref ht "eLePhAnT"))
  (test-error (hash-table-ref ht "goose"))
  (test-lset-equal? '("cat" "dog" "elephant") (hash-table-keys ht))
  (test-lset-equal? '(black white pink) (hash-table-values ht))
  (test-lset-equal?
   '(("cat" . black) ("dog" . white) ("elephant" . pink))
   (hash-table->alist ht)))

;; (chibi's "Exception values" test is left out: make-exception and
;; exception-kind are chibi's own, not R7RS or SRFI 69.)
;; stress test
(test 625
    (let ((ht (make-hash-table)))
      (do ((i 0 (+ i 1))) ((= i 1000))
        (hash-table-set! ht i (* i i)))
      (hash-table-ref/default ht 25 #f)))

(test-end)
