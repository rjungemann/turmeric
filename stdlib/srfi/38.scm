;;; srfi/38 -- SRFI 38, External Representation for Data With Shared
;;; Structure: two new names for R7RS procedures.
;;;
;;; `write-with-shared-structure` is (scheme write)'s `write-shared`, and
;;; `read-with-shared-structure` is (scheme read)'s `read`, which reads datum
;;; labels.  SRFI 38's optional third argument to the writer is
;;; implementation-defined; it is accepted and ignored.
;;; docs/upcoming/r7rs-srfi-plan.md, D2.
(define-library (srfi 38)
  (export write-with-shared-structure read-with-shared-structure)
  (import (scheme base) (scheme write) (scheme read))
  (begin
    (define (write-with-shared-structure obj . args)
      (if (null? args) (write-shared obj) (write-shared obj (car args))))
    (define (read-with-shared-structure . args)
      (if (null? args) (read) (read (car args))))))
