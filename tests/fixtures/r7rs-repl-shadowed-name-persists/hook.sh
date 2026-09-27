#!/usr/bin/env bash
# r7rs-srfi-plan S0: a name the program defines for itself -- a standard one
# (`square`), a stdlib one (`list-length`), a Turmeric type name (`any`) --
# is respelled `<name>--user` so it cannot collide with the stdlib's.  At the
# R7RS prompt each line is a turn of its own, so a later turn has to find the
# earlier turn's respelling: `(square 2)` is the prompt's `square`, not
# (scheme base)'s, and so is the `square` handed to `map`.  Redefining it in
# a later turn replaces it again.
set -u
TUR="${TUR:-./build/tur}"
printf '(define (square x) (+ x 1000))\n(square 2)\n(map square (list 1 2))\n(define (list-length x) 77)\n(list-length (list 1))\n(define (any p) (p 1))\n(any (lambda (x) (+ x 1)))\n(define (square x) (+ x 5))\n(square 1)\n(call-with-values (lambda () (exact-integer-sqrt 17)) list)\n' \
  | ASAN_OPTIONS=detect_leaks=0 "$TUR" repl --lang r7rs 2>/dev/null | grep '^=>'
exit 0
