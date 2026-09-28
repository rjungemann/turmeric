#!/usr/bin/env bash
# r7rs-repl-forgets-macros-and-set: each R7RS prompt turn is lowered on its
# own, so what one turn set up was gone in the next:
#   - a macro it defined ("unknown function or operator 'swap-args'");
#   - a variable it defined, to a later `set!` ("'n' is immutable");
#   - what an import set up in the lowering: an SRFI's names and macros
#     (`fold`, `cut`), a `rename`, a library's exported macros.
# And a turn that imported a library ran none of its own expressions (they
# became a `main` nothing called) and kept its definitions private.  A later
# definition of a macro's name replaces the macro, and a later define-syntax
# replaces that.
set -u
TUR="${TUR:-./build/tur}"
case "$TUR" in /*) ;; *) TUR="$PWD/$TUR" ;; esac
work="${1:-$(mktemp -d)}"
mkdir -p "$work/lb"
cat > "$work/lb/mac.tur" <<'LIB'
#lang r7rs
(define-library (lb mac)
  (export swap2 helper)
  (import (scheme base))
  (begin
    (define (helper x) (* x 100))
    (define-syntax swap2 (syntax-rules () ((_ a b) (list (helper b) a))))))
LIB
cd "$work" || exit 1
printf '%s\n' \
  '(define-syntax swap-args (syntax-rules () ((_ f a b) (f b a))))' \
  '(swap-args list 1 2)' \
  '(define n 0)' \
  '(set! n (+ n 1))' \
  'n' \
  '(define (bump!) (set! n (+ n 10)) n)' \
  '(bump!)' \
  '(begin (define-syntax twice (syntax-rules () ((_ e) (begin e e)))) (define k 0))' \
  '(twice (set! k (+ k 1)))' \
  'k' \
  '(swap-args - 1 10)' \
  '(define-syntax swap-args (syntax-rules () ((_ f a b) (list (quote swapped) (f b a)))))' \
  '(swap-args - 1 10)' \
  '(define (swap-args f a b) (f a b))' \
  '(swap-args - 1 10)' \
  '(twice n)' \
  '(import (srfi 1) (srfi 26) (rename (only (srfi 2) and-let*) (and-let* al*)))' \
  '(fold + 0 (map (cut * 2 <>) (list 1 2 3)))' \
  '(al* ((x 5)) (+ x 1))' \
  '(import (lb mac)) (display "same turn ") (define (mine) (swap2 1 2)) (mine)' \
  '(list (mine) (helper 3) (swap2 3 4))' \
  | ASAN_OPTIONS=detect_leaks=0 "$TUR" repl --lang r7rs 2>&1 | grep -E '^=>|error|same turn'
exit 0
