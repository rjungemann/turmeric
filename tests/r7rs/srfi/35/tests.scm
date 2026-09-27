;;; tests/r7rs/srfi/35/tests.scm -- SRFI 35's tests: chibi-scheme's
;;; lib/srfi/35/test.sld, its run-tests body lifted out as top-level forms
;;; for tests/r7rs/run-conformance.py (r7rs-srfi-plan D7).  Its first group
;;; is the SRFI's own examples; its second used chibi's R6RS-style
;;; constructors (make-message-condition, make-error), which are not SRFI 35,
;;; so here it builds the same conditions with make-condition; its third
;;; (R6RS field shadowing) is not SRFI 35 and is left out.  The last group is
;;; Turmeric's: R7RS error objects as &error/&message conditions
;;; (r7rs-srfi-plan section 7, question 4).  The adaptation (c) 2009-2021
;;; Alex Shinn, BSD-3, see tests/r7rs/CHIBI-COPYING.

(test-begin "srfi-35: condition types")

;; Adapted from the SRFI 35 examples
(define-condition-type &c &condition
  c?
  (x c-x))

(define-condition-type &c1 &c
  c1?
  (a c1-a))

(define-condition-type &c2 &c
  c2?
  (b c2-b))
(define v1 (make-condition &c1 'x "V1" 'a "a1"))
(define v2 (condition (&c2
                       (x "V2")
                       (b "b2"))))
(define v3 (condition (&c1
                       (x "V3/1")
                       (a "a3"))
                      (&c2
                       (b "b3"))))
(define v4 (make-compound-condition v1 v2))
(define v5 (make-compound-condition v2 v3))

(test #t (c? v1))
(test #t (c1? v1))
(test #f (c2? v1))
(test "V1" (c-x v1))
(test "a1" (c1-a v1))

(test #t (c? v2))
(test #f (c1? v2))
(test #t (c2? v2))
(test "V2" (c-x v2))
(test "b2" (c2-b v2))

(test #t (c? v3))
(test #t (c1? v3))
(test #t (c2? v3))
(test "V3/1" (c-x v3))
(test "a3" (c1-a v3))
(test "b3" (c2-b v3))

(test #t (c? v4))
(test #t (c1? v4))
(test #t (c2? v4))
(test "V1" (c-x v4))
(test "a1" (c1-a v4))
(test "b2" (c2-b v4))

(test #t (c? v5))
(test #t (c1? v5))
(test #t (c2? v5))
(test "V2" (c-x v5))
(test "a3" (c1-a v5))
(test "b2" (c2-b v5))

;; Standard condition hierarchy
(define mc (make-condition &message 'message "foo!"))
(test #t (message-condition? mc))
(test "foo!" (condition-message mc))
(define ec (make-condition &error))
(test #t (error? ec))
(test #t (serious-condition? ec))
(define cc (make-compound-condition ec mc))
(test #t (error? cc))
(test #t (serious-condition? cc))
(test #t (message-condition? cc))
(test "foo!" (condition-message cc))

;; The procedural interface
(test #t (condition-type? &error))
(test #f (condition-type? ec))
(test #t (condition? ec))
(test #f (condition? &error))
(test #f (condition? 'error))
(test #t (condition-has-type? ec &serious))
(test #f (condition-has-type? mc &serious))
(test "foo!" (condition-ref mc 'message))
(test "V2" (condition-ref v5 'x))
(test "b2" (condition-message
            (make-compound-condition
             (make-condition &message 'message "b2") v1)))
(define ct (make-condition-type 'ct &error '(p q)))
(define cx (make-condition ct 'q 2 'p 1))
(test #t (error? cx))
(test '(1 2) (list (condition-ref cx 'p) (condition-ref cx 'q)))
(test #f (c? (extract-condition cx &error)))
(test #t (error? (extract-condition cx &error)))
(test-error (make-condition-type "ct" &error '()))
(test-error (make-condition-type 'ct 'no-type '()))
(test-error (make-condition-type 'ct ct '(p)))
(test-error (make-condition ct 'p 1))
(test-error (extract-condition mc &error))

;; Raised and caught
(test "boom"
      (guard (e ((message-condition? e) (condition-message e)))
        (raise (condition (&error) (&message (message "boom"))))))
(test 'serious
      (guard (e ((error? e) 'error) ((serious-condition? e) 'serious))
        (raise (make-condition &serious))))

;; An R7RS error object is an &error with a &message
(define eo (guard (e (#t e)) (error "went wrong" 1 2)))
(test #t (condition? eo))
(test #t (error? eo))
(test #t (serious-condition? eo))
(test #t (message-condition? eo))
(test "went wrong" (condition-message eo))
(test #f (c? eo))
(test "went wrong" (condition-message (extract-condition eo &message)))
(test #t (error? (make-compound-condition eo v1)))
(test "V1" (c-x (make-compound-condition eo v1)))
(test #f (error-object? ec))
(test #t (error? (guard (e (#t e)) (vector-ref (vector 1 2) 5))))
(test #t (error? (guard (e (#t e)) (open-input-file "/no/such/srfi-35/file"))))

(test-end)
