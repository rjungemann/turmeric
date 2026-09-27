;;; tests/r7rs/srfi/48/tests.scm -- SRFI 48's tests: test/test-0001.scm of
;;; the SRFI's repository (Hamayama, 2017; MIT), in (chibi test)'s vocabulary
;;; for tests/r7rs/run-conformance.py (r7rs-srfi-plan D7): each (expect
;;; expected form) is (test expected form), and (expect expected form check)
;;; is (test-equal check expected form).  Its cond-expands keep the branch an
;;; R7RS implementation takes; a section heading is a comment.
;;;
;;; SPDX-FileCopyrightText: 2017 Hamayama <hamay1010@gmail.com>
;;; SPDX-License-Identifier: MIT

(define (x->number x)
  (cond
   ((number? x) x)
   ((string? x) (string->number x))
   (else (error "x->number error"))))

(define (nearly=? a b)
  (let* ((a1 (x->number a))
         (b1 (x->number b))
         (e1 (abs (- a1 b1))))
    ;(format #t "(a1 = ~s, b1 = ~s, e1 = ~s)~%" a1 b1 e1)
    (< e1 1.0e-10)))

(define pi 3.141592653589793)

(test-begin "srfi-48 format test")

;; original
(test (format "test ~s" 'me) (format #f "test ~a" "me"))
(test  " 0.333" (format "~6,3F" 1/3)) ;;; "  .333" OK
(test "  12" (format "~4F" 12))
(test "  12.346" (format "~8,3F" 12.3456))
(test "123.346" (format "~6,3F" 123.3456))
(test "123.346" (format "~4,3F" 123.3456))
(test "0.000+1.949i" (format "~8,3F" (sqrt -3.8)))
(test " 32.00" (format "~6,2F" 32))
(test "    32" (format "~6F" 32))
;(test "   32." (format "~6F" 32.)) ;; "  32.0" OK
(test "  32.0" (format "~6F" 32.))
;; NB: (not (and (exact? 32.) (integer? 32.)))
(test "  3.2e46" (format "~8F" 32e45))
(test " 3.2e-44" (format "~8F" 32e-45))
(test "  3.2e21" (format "~8F" 32e20))
;;(test "   3.2e6" (format "~8F" 32e5)) ;; ok.  converted in input to 3200000.0
;(test "   3200." (format "~8F" 32e2)) ;; "  3200.0" OK
(test "  3200.0" (format "~8F" 32e2))
(test " 3.20e11" (format "~8,2F" 32e10))
(test "      1.2345" (format "~12F" 1.2345))
(test "        1.23" (format "~12,2F" 1.2345))
(test "       1.234" (format "~12,3F" 1.2345))
(test "        0.000+1.949i" (format "~20,3F" (sqrt -3.8)))
(test "0.000+1.949i" (format "~8,3F" (sqrt -3.8)))
(test " 3.46e11" (format "~8,2F" 3.4567e11))
; (test "#1=(a b c . #1#)"
;         (format "~w" (let ( (c '(a b c)) ) (set-cdr! (cddr c) c) c)))
(test "
"
        (format "~A~A~&" (list->string (list #\newline)) ""))
(test "a new test"
        (format "~a ~? ~a" 'a "~s" '(new) 'test))
(test "a new test, yes!"
        (format "~a ~?, ~a!" 'a "~s ~a" '(new test) 'yes))
(test " 3.46e20" (format "~8,2F" 3.4567e20))
(test " 3.46e21" (format "~8,2F" 3.4567e21))
(test " 3.46e22" (format "~8,2F" 3.4567e22))
(test " 3.46e23" (format "~8,2F" 3.4567e23))
(test "   3.e24" (format "~8,0F" 3.4567e24))
(test "  3.5e24" (format "~8,1F" 3.4567e24))
(test " 3.46e24" (format "~8,2F" 3.4567e24))
(test "3.457e24" (format "~8,3F" 3.4567e24))
(test "   4.e24" (format "~8,0F" 3.5567e24))
(test "  3.6e24" (format "~8,1F" 3.5567e24))
(test " 3.56e24" (format "~8,2F" 3.5567e24))
(test "    -3.e-4" (format "~10,0F" -3e-4))
(test "   -3.0e-4" (format "~10,1F" -3e-4))
(test "  -3.00e-4" (format "~10,2F" -3e-4))
(test " -3.000e-4" (format "~10,3F" -3e-4))
(test "-3.0000e-4" (format "~10,4F" -3e-4))
(test "-3.00000e-4" (format "~10,5F" -3e-4))
(test "     1.020" (format "~10,3F" 1.02))
(test "     1.025" (format "~10,3F" 1.025))
(test "     1.026" (format "~10,3F" 1.0256))
(test "     1.002" (format "~10,3F" 1.002))
(test "     1.002" (format "~10,3F" 1.0025))
(test "     1.003" (format "~10,3F" 1.00256))


;; examples
(test "    0.33"   (format "~8,2F" 1/3))
(test "    32"     (format "~6F" 32))
(test "   32.00"   (format "~8,2F" 32))
(test "4321.00"    (format "~1,2F" 4321))
(test "0.00+1.97i" (format "~1,2F" (sqrt -3.9)))
(test "3200000.0"  (format "~8F" 32e5))
;(test "   3.2e6"   (format "~8F" 32e5))
(test-equal (lambda (e r) (string? r)) "<string>" (format "~h"))
(test "Hello, World!" (format "Hello, ~a" "World!"))
(test "Error, list is too short: (one \"two\" 3)" (format "Error, list is too short: ~s" '(one "two" 3)))
(test "test me"    (format "test me"))
(test "this is a \"test\"" (format "~a ~s ~a ~s" 'this 'is "a" "test"))
(test (if #f #f)   (format #t "#d~d #x~x #o~o #b~b~%" 32 32 32 32))
(test "a new test" (format "~a ~? ~a" 'a "~s" '(new) 'test))
(test "\n1\n2\n3\n" (format #f "~&1~&~&2~&~&~&3~%"))
(test "3  2 2  3 \n" (format #f "~a ~? ~a ~%" 3 " ~s ~s " '(2 2) 3))
;; incorrect mutation of literal list in example
;(test "#1=(a b c . #1#)" (format "~w" (let ( (c '(a b c)) ) (set-cdr! (cddr c) c) c)))
(test "#0=(a b c . #0#)" (format "~w" (let ( (c (list 'a 'b 'c)) ) (set-cdr! (cddr c) c) c)))
(test "   32.00"   (format "~8,2F" 32))
(test "0.000+1.949i" (format "~8,3F" (sqrt -3.8)))
;(test " 3.45e11"   (format "~8,2F" 3.4567e11))
(test " 3.46e11"   (format "~8,2F" 3.4567e11))
(test " 0.333"     (format "~6,3F" 1/3))
(test "  12"       (format "~4F" 12))
(test " 123.346"   (format "~8,3F" 123.3456))
(test "123.346"    (format "~6,3F" 123.3456))
(test "123.346"    (format "~2,3F" 123.3456))
(test "     foo"   (format "~8,3F" "foo"))
(test "\n"         (format "~a~a~&" (list->string (list #\newline)) ""))


;; ~F normal
(test "0"          (format "~F"    0))
(test "1"          (format "~F"    1))
(test "123"        (format "~F"  123))
(test "0.456"      (format "~F"    0.456))
(test "123.456"    (format "~F"  123.456))
(test "-1"         (format "~F"   -1))
(test "-123"       (format "~F" -123))
(test "-0.456"     (format "~F"   -0.456))
(test "-123.456"   (format "~F" -123.456))


;; ~F width
(test "123"        (format "~0F"  123))
(test "123"        (format "~1F"  123))
(test "123"        (format "~2F"  123))
(test "123"        (format "~3F"  123))
(test " 123"       (format "~4F"  123))
(test "  123"      (format "~5F"  123))
(test "-123"       (format "~3F" -123))
(test "-123"       (format "~4F" -123))
(test " -123"      (format "~5F" -123))
(test "  -123"     (format "~6F" -123))


;; ~F digits
(test "123."       (format "~1,0F"   123))
(test "123.0"      (format "~1,1F"   123))
(test "123.00"     (format "~1,2F"   123))
(test "0.12"       (format "~1,2F"   0.123))
(test "0.123"      (format "~1,3F"   0.123))
(test "0.1230"     (format "~1,4F"   0.123))
(test "-123."      (format "~1,0F"  -123))
(test "-123.0"     (format "~1,1F"  -123))
(test "-123.00"    (format "~1,2F"  -123))
(test "-0.12"      (format "~1,2F"  -0.123))
(test "-0.123"     (format "~1,3F"  -0.123))
(test "-0.1230"    (format "~1,4F"  -0.123))


;; ~F rounding (banker's rounding)
(test "123."       (format "~1,0F"   123.456))
(test "123.5"      (format "~1,1F"   123.456))
(test "123.46"     (format "~1,2F"   123.456))
(test "-123."      (format "~1,0F"  -123.456))
(test "-123.5"     (format "~1,1F"  -123.456))
(test "-123.46"    (format "~1,2F"  -123.456))
(test "123.0"      (format "~1,1F"   123.05))
(test "123.2"      (format "~1,1F"   123.15))
(test "124.0"      (format "~1,1F"   123.95))
(test "-123.0"     (format "~1,1F"  -123.05))
(test "-123.2"     (format "~1,1F"  -123.15))
(test "-124.0"     (format "~1,1F"  -123.95))
(test "1000.00"    (format "~1,2F"   999.995))
(test "-1000.00"   (format "~1,2F"  -999.995))
(test "1."         (format "~1,0F"   1.49))
(test "2."         (format "~1,0F"   1.5))
(test "2."         (format "~1,0F"   1.51))
(test "2."         (format "~1,0F"   2.49))
(test "2."         (format "~1,0F"   2.5))
(test "3."         (format "~1,0F"   2.51))


;; ~F misc
(test "+inf.0"     (format "~F" +inf.0))
(test "-inf.0"     (format "~F" -inf.0))
(test "+nan.0"     (format "~F" +nan.0))
(test "0.0"        (format "~F" 0.0))
(test "-0.0"       (format "~F" -0.0))
(test "+inf.0"     (format "~1F" +inf.0))
(test "-inf.0"     (format "~1F" -inf.0))
(test "+nan.0"     (format "~1F" +nan.0))
(test "0.0"        (format "~1F" 0.0))
(test "-0.0"       (format "~1F" -0.0))
(test "+inf.0"     (format "~1,0F" +inf.0))
(test "-inf.0"     (format "~1,0F" -inf.0))
(test "+nan.0"     (format "~1,0F" +nan.0))
(test "0."         (format "~1,0F" 0.0))
(test "-0."        (format "~1,0F" -0.0))
(test "+inf.0"     (format "~1,1F" +inf.0))
(test "-inf.0"     (format "~1,1F" -inf.0))
(test "+nan.0"     (format "~1,1F" +nan.0))
(test "0.0"        (format "~1,1F" 0.0))
(test "-0.0"       (format "~1,1F" -0.0))
(test "31.41592653589793" (format "~F" (* pi 10)))
(test "0.33333"    (format "~1,5F"  1/3))
(test "-0.33333"   (format "~1,5F" -1/3))
(test "0.142857142857" (format "~1,12F"  1/7))
(test-equal nearly=? "299999999.999999999" (format "~F" 299999999999999999/1000000000))
(test "1.797693e308"   (format "~F"     1.797693e308))
(test "1.797693e308"   (format "~1F"    1.797693e308))
(test "2.e308"         (format "~1,0F"  1.797693e308))
(test "1.8e308"        (format "~1,1F"  1.797693e308))
(test "-1.797693e308"  (format "~F"    -1.797693e308))
(test "-1.797693e308"  (format "~1F"   -1.797693e308))
(test "-2.e308"        (format "~1,0F" -1.797693e308))
(test "-1.8e308"       (format "~1,1F" -1.797693e308))
(test "2.225074e-308"  (format "~F"  2.225074e-308))
(test "5.02"       (format "~1,2F" 5.015))
(test "6.00"       (format "~1,2F" 5.999))
(test "123."       (format "~1,0F" 123.00))
(test "0.1"        (format "~F" .1))
(test "1"          (format "~1f" 1)) ; lower case f
(test "1.e100"     (format "~1,0F" 1e100))
(test "1."         (format "~1,0F" 1))
(test "0."         (format "~1,0F" .1))
(test "0.0"        (format "~1,1F" .01))


;; ~F error
(test "<error>" (guard (e (else "<error>")) (format "~-1F" 1)))
(test "<error>" (guard (e (else "<error>")) (format "~1,-1F" 1)))


;; from mailing list 2004-05-27
(test "1.230e20"   (format "~0,3F" 1.23e20))
(test "1.230e-20"  (format "~0,3F" 1.23e-20))


;; from mailing list 2004-06-11
(test "3.457e15"   (format "~8,3F" 3.4569e15))
(test "   3.457"   (format "~8,3F" 3.4569))
(test " 3.46e15"   (format "~8,2F" 3.456e15))
(test "    3.46"   (format "~8,2F" 3.456))


;; from mailing list 2005-06-03
(test "    -3.e-4" (format "~10,0F" -3e-4))
(test "   -3.0e-4" (format "~10,1F" -3e-4))
(test "  -3.00e-4" (format "~10,2F" -3e-4))
(test " -3.000e-4" (format "~10,3F" -3e-4))
(test "-3.0000e-4" (format "~10,4F" -3e-4))
(test " 3.0000e-5" (format "~10,4F"  3e-5))


;; from mailing list 2005-06-07
(test "     1.020" (format "~10,3F" 1.02))
(test "     1.025" (format "~10,3F" 1.025))
(test "     1.026" (format "~10,3F" 1.0256))
(test "     1.002" (format "~10,3F" 1.002))
(test "     1.002" (format "~10,3F" 1.0025))
(test "     1.003" (format "~10,3F" 1.00256))


;; from mailing list 2005-06-07
(test "1.000012"   (format "~8,6F" 1.00001234))


;; from mailing list 2005-07-02
(test "abc\ndef\nghi\n" (format "abc~%~&def~&ghi~%"))
(test "\ndef\nghi\n" (format "~&def~&ghi~%"))


;; from mailing list 2017-10-11
(test "   1.00"    (format "~7,2F" .997554209949891))
(test "   1.00"    (format "~7,2F" .99755))
(test "   1.00"    (format "~7,2F" .9975))
(test "   1.00"    (format "~7,2F" .997))
(test "   0.99"    (format "~7,2F" .99))


;; from mailing list 2017-10-13
(test "  18.00"    (format "~7,2F" 18.0000000000008))
(test "    -15."   (format "~8,0F" -14.99995999999362))

(test-end)

