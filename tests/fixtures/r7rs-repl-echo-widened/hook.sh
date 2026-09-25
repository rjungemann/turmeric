#!/usr/bin/env bash
# r7rs-repl-toplevel-expression-value-not-widened: the R7RS prompt echoes a
# top-level expression's value as a Scheme value whatever its elaborated
# type -- a `let` yielding a vector printed as a pointer before.  A `define`
# and an unspecified value print nothing.
set -u
TUR="${TUR:-./build/tur}"
printf '(vector 1 2)\n(let ((v (vector 1 2))) v)\n(define (g) (let ((v (vector 1 2))) v))\n(g)\n(list 1 (quote a) "s")\n(let loop ((i 0) (acc (quote ()))) (if (= i 3) acc (loop (+ i 1) (cons i acc))))\n(+ 1 2)\n' \
  | ASAN_OPTIONS=detect_leaks=0 "$TUR" repl --lang r7rs 2>/dev/null | grep '^=>'
exit 0
