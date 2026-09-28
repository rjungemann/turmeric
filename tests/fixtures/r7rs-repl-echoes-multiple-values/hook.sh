#!/usr/bin/env bash
# r7rs-repl-echoes-multiple-values-opaquely: the R7RS prompt echoes each of a
# turn's values on its own `=>` line and nothing for `(values)` -- it printed
# the prelude's carrier, `#<R7rsValues>`.  A definition echoes nothing either
# (its value is unspecified): `(define x 3)` printed `=> 3`, a procedure or
# record definition `=> #<procedure>`.
set -u
TUR="${TUR:-./build/tur}"
printf '%s\n' \
  '(values 1 2)' \
  '(values)' \
  '(exact-integer-sqrt 17)' \
  '(values 7)' \
  '(values (quote a) "b" #\c (list 1 2))' \
  '(call-with-values (lambda () (values 1 2)) list)' \
  '(define x 3)' \
  '(define (g) (values x 4))' \
  '(define-values (a b) (g))' \
  '(define-record-type point (make-point px py) point? (px point-x))' \
  '(begin (define z 9))' \
  '(g)' \
  '(list x a b z (point-x (make-point 5 6)))' \
  '(begin (define w 1) w)' \
  | ASAN_OPTIONS=detect_leaks=0 "$TUR" repl --lang r7rs 2>/dev/null | grep '^=>'
exit 0
