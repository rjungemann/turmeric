;;; tests/r7rs/srfi/13/tests.scm -- SRFI 13's tests: the Gauche suite
;;; (Shiro Kawai), as Larceny carries it in lib/SRFI/test/srfi-13-test.sps,
;;; in (chibi test)'s vocabulary for tests/r7rs/run-conformance.py
;;; (r7rs-srfi-plan D7): each (test* key expected form) is (test expected
;;; form).  SRFI 13 has no suite of its own and chibi no SRFI 13.  Only the
;;; Gauche part is here: the other suite in Larceny's file is Guile's, under
;;; the GPL.  The suite leaves string-map and string-for-each out of (scheme
;;; base) (./base-except, D5).  Gauche's notice, as it came:

;; Tests for SRFI-13 as implemented by the Gauche scheme system.
;;
;;   Copyright (c) 2000-2003 Shiro Kawai, All rights reserved.
;;
;;   Redistribution and use in source and binary forms, with or without
;;   modification, are permitted provided that the following conditions
;;   are met:
;;
;;    1. Redistributions of source code must retain the above copyright
;;       notice, this list of conditions and the following disclaimer.
;;
;;    2. Redistributions in binary form must reproduce the above copyright
;;       notice, this list of conditions and the following disclaimer in the
;;       documentation and/or other materials provided with the distribution.
;;
;;    3. Neither the name of the authors nor the names of its contributors
;;       may be used to endorse or promote products derived from this
;;       software without specific prior written permission.
;;
;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;;   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;;   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;;   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;;   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
;;   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
;;   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
;;   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
;;   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
;;   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;

(test-begin "srfi-13: strings")

;; See http://sourceforge.net/projects/gauche/
(test #f (string-null? "abc"))
(test #t (string-null? ""))
(test #t (string-every #\a ""))
(test #t (string-every #\a "aaaa"))
(test #f (string-every #\a "aaba"))
(test #t (string-every char-set:lower-case "aaba"))
(test #f (string-every char-set:lower-case "aAba"))
(test #t (string-every char-set:lower-case ""))
(test #t (string-every (lambda (x) (char-ci=? x #\a)) "aAaA"))
(test #f (string-every (lambda (x) (char-ci=? x #\a)) "aAbA"))
(test (char->integer #\A)
       (string-every (lambda (x) (char->integer x)) "aAbA"))
(test #t
       (string-every (lambda (x) (error "hoge")) ""))
(test #t (string-any #\a "aaaa"))
(test #f (string-any #\a "Abcd"))
(test #f (string-any #\a ""))
(test #t (string-any char-set:lower-case "ABcD"))
(test #f (string-any char-set:lower-case "ABCD"))
(test #f (string-any char-set:lower-case ""))
(test #t (string-any (lambda (x) (char-ci=? x #\a)) "CAaA"))
(test #f (string-any (lambda (x) (char-ci=? x #\a)) "ZBRC"))
(test #f (string-any (lambda (x) (char-ci=? x #\a)) ""))
(test (char->integer #\a)
       (string-any (lambda (x) (char->integer x)) "aAbA"))
(test "0123456789"
       (string-tabulate (lambda (code)
                          (integer->char (+ code (char->integer #\0))))
                        10))
(test ""
       (string-tabulate (lambda (code)
                          (integer->char (+ code (char->integer #\0))))
                        0))
(test "cBa"
       (reverse-list->string '(#\a #\B #\c)))
(test ""
       (reverse-list->string '()))
; string-join : Gauche builtin.
(test "cde" (substring/shared "abcde" 2))
(test "cd"  (substring/shared "abcde" 2 4))
(test "abCDEfg"
       (let ((x (string-copy "abcdefg")))
         (string-copy! x 2 "CDE")
         x))
(test "abCDEfg"
       (let ((x (string-copy "abcdefg")))
         (string-copy! x 2 "ZABCDE" 3)
         x))
(test "abCDEfg"
       (let ((x (string-copy "abcdefg")))
         (string-copy! x 2 "ZABCDEFG" 3 6)
         x))
(test "Pete S"  (string-take "Pete Szilagyi" 6))
(test ""        (string-take "Pete Szilagyi" 0))
(test "Pete Szilagyi" (string-take "Pete Szilagyi" 13))
(test "zilagyi" (string-drop "Pete Szilagyi" 6))
(test "Pete Szilagyi" (string-drop "Pete Szilagyi" 0))
(test ""        (string-drop "Pete Szilagyi" 13))

(test "rules" (string-take-right "Beta rules" 5))
(test ""      (string-take-right "Beta rules" 0))
(test "Beta rules" (string-take-right "Beta rules" 10))
(test "Beta " (string-drop-right "Beta rules" 5))
(test "Beta rules" (string-drop-right "Beta rules" 0))
(test ""      (string-drop-right "Beta rules" 10))

(test "  325" (string-pad "325" 5))
(test "71325" (string-pad "71325" 5))
(test "71325" (string-pad "8871325" 5))
(test "~~325" (string-pad "325" 5 #\~))
(test "~~~25" (string-pad "325" 5 #\~ 1))
(test "~~~~2" (string-pad "325" 5 #\~ 1 2))
(test "325  " (string-pad-right "325" 5))
(test "71325" (string-pad-right "71325" 5))
(test "88713" (string-pad-right "8871325" 5))
(test "325~~" (string-pad-right "325" 5 #\~))
(test "25~~~" (string-pad-right "325" 5 #\~ 1))
(test "2~~~~" (string-pad-right "325" 5 #\~ 1 2))

(test "a b c d  \n"
       (string-trim "  \t  a b c d  \n"))
(test "\t  a b c d  \n"
       (string-trim "  \t  a b c d  \n" #\space))
(test "a b c d  \n"
       (string-trim "4358948a b c d  \n" char-set:digit))

(test "  \t  a b c d"
       (string-trim-right "  \t  a b c d  \n"))
(test "  \t  a b c d  "
       (string-trim-right "  \t  a b c d  \n" (char-set #\newline)))
(test "349853a b c d"
       (string-trim-right "349853a b c d03490" char-set:digit))

(test "a b c d"
       (string-trim-both "  \t  a b c d  \n"))
(test "  \t  a b c d  "
       (string-trim-both "  \t  a b c d  \n" (char-set #\newline)))
(test "a b c d"
       (string-trim-both "349853a b c d03490" char-set:digit))

;; string-fill - in string.scm

(test 5
       (string-compare "The cat in the hat" "abcdefgh"
                       values values values
                       4 6 2 4))
(test 5
       (string-compare-ci "The cat in the hat" "ABCDEFGH"
                          values values values
                          4 6 2 4))

;; TODO: bunch of string= families

(test 5
       (string-prefix-length "cancaNCAM" "cancancan"))
(test 8
       (string-prefix-length-ci "cancaNCAM" "cancancan"))
(test 2
       (string-suffix-length "CanCan" "cankancan"))
(test 5
       (string-suffix-length-ci "CanCan" "cankancan"))

(test #t    (string-prefix? "abcd" "abcdefg"))
(test #f    (string-prefix? "abcf" "abcdefg"))
(test #t (string-prefix-ci? "abcd" "aBCDEfg"))
(test #f (string-prefix-ci? "abcf" "aBCDEfg"))
(test #t    (string-suffix? "defg" "abcdefg"))
(test #f    (string-suffix? "aefg" "abcdefg"))
(test #t (string-suffix-ci? "defg" "aBCDEfg"))
(test #f (string-suffix-ci? "aefg" "aBCDEfg"))

(test 4
       (string-index "abcd:efgh:ijkl" #\:))
(test 4
       (string-index "abcd:efgh;ijkl" (char-set-complement char-set:letter)))
(test #f
       (string-index "abcd:efgh;ijkl" char-set:digit))
(test 9
       (string-index "abcd:efgh:ijkl" #\: 5))
(test 4
       (string-index-right "abcd:efgh;ijkl" #\:))
(test 9
       (string-index-right "abcd:efgh;ijkl" (char-set-complement char-set:letter)))
(test #f
       (string-index-right "abcd:efgh;ijkl" char-set:digit))
(test 4
       (string-index-right "abcd:efgh;ijkl" (char-set-complement char-set:letter) 2 5))

(test 2
       (string-count "abc def\tghi jkl" #\space))
(test 3
       (string-count "abc def\tghi jkl" char-set:whitespace))
(test 2
       (string-count "abc def\tghi jkl" char-set:whitespace 4))
(test 1
       (string-count "abc def\tghi jkl" char-set:whitespace 4 9))
(test 3
       (string-contains "Ma mere l'oye" "mer"))
(test #f
       (string-contains "Ma mere l'oye" "Mer"))
(test 3
       (string-contains-ci "Ma mere l'oye" "Mer"))
(test #f
       (string-contains-ci "Ma mere l'oye" "Meer"))

(test "--Capitalize This Sentence."
       (string-titlecase "--capitalize tHIS sentence."))
(test "3Com Makes Routers."
       (string-titlecase "3com makes routers."))
(test "alSo Whatever"
       (let ((s (string-copy "also whatever")))
         (string-titlecase! s 2 9)
         s))

(test "SPEAK LOUDLY"
       (string-upcase "speak loudly"))
(test "PEAK"
       (string-upcase "speak loudly" 1 5))
(test "sPEAK loudly"
       (let ((s (string-copy "speak loudly")))
         (string-upcase! s 1 5)
         s))

(test "speak softly"
       (string-downcase "SPEAK SOFTLY"))
(test "peak"
       (string-downcase "SPEAK SOFTLY" 1 5))
(test "Speak SOFTLY"
       (let ((s (string-copy "SPEAK SOFTLY")))
         (string-downcase! s 1 5)
         s))

(test "nomel on nolem on"
       (string-reverse "no melon no lemon"))
(test "nomel on"
       (string-reverse "no melon no lemon" 9))
(test "on"
       (string-reverse "no melon no lemon" 9 11))
(test "nomel on nolem on"
       (let ((s (string-copy "no melon no lemon")))
         (string-reverse! s) s))
(test "no melon nomel on"
       (let ((s (string-copy "no melon no lemon")))
         (string-reverse! s 9) s))
(test "no melon on lemon"
       (let ((s (string-copy "no melon no lemon")))
         (string-reverse! s 9 11) s))

(test #f
       (let ((s "test")) (eq? s (string-append s))))
(test #f
       (let ((s "test")) (eq? s (string-concatenate (list s)))))
(test "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
       (string-concatenate
        '("A" "B" "C" "D" "E" "F" "G" "H"
          "I" "J" "K" "L" "M" "N" "O" "P"
          "Q" "R" "S" "T" "U" "V" "W" "X" "Y" "Z"
          "a" "b" "c" "d" "e" "f" "g" "h"
          "i" "j" "k" "l" "m" "n" "o" "p"
          "q" "r" "s" "t" "u" "v" "w" "x" "y" "z")))
(test "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
       (string-concatenate/shared
        '("A" "B" "C" "D" "E" "F" "G" "H"
          "I" "J" "K" "L" "M" "N" "O" "P"
          "Q" "R" "S" "T" "U" "V" "W" "X" "Y" "Z"
          "a" "b" "c" "d" "e" "f" "g" "h"
          "i" "j" "k" "l" "m" "n" "o" "p"
          "q" "r" "s" "t" "u" "v" "w" "x" "y" "z")))
(test "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA"
       (string-concatenate-reverse
        '("A" "B" "C" "D" "E" "F" "G" "H"
          "I" "J" "K" "L" "M" "N" "O" "P"
          "Q" "R" "S" "T" "U" "V" "W" "X" "Y" "Z"
          "a" "b" "c" "d" "e" "f" "g" "h"
          "i" "j" "k" "l" "m" "n" "o" "p"
          "q" "r" "s" "t" "u" "v" "w" "x" "y" "z")))
(test #f
       (let ((s "test"))
         (eq? s (string-concatenate-reverse (list s)))))
(test "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA"
       (string-concatenate-reverse/shared
        '("A" "B" "C" "D" "E" "F" "G" "H"
          "I" "J" "K" "L" "M" "N" "O" "P"
          "Q" "R" "S" "T" "U" "V" "W" "X" "Y" "Z"
          "a" "b" "c" "d" "e" "f" "g" "h"
          "i" "j" "k" "l" "m" "n" "o" "p"
          "q" "r" "s" "t" "u" "v" "w" "x" "y" "z")))

(test "svool"
       (string-map (lambda (c)
                     (integer->char (- 219 (char->integer c))))
                   "hello"))
(test "vool"
       (string-map (lambda (c)
                     (integer->char (- 219 (char->integer c))))
                   "hello" 1))
(test "vo"
       (string-map (lambda (c)
                     (integer->char (- 219 (char->integer c))))
                   "hello" 1 3))
(test "svool"
       (let ((s (string-copy "hello")))
         (string-map! (lambda (c)
                        (integer->char (- 219 (char->integer c))))
                      s)
         s))
(test "hvool"
       (let ((s (string-copy "hello")))
         (string-map! (lambda (c)
                        (integer->char (- 219 (char->integer c))))
                      s 1)
         s))
(test "hvolo"
       (let ((s (string-copy "hello")))
         (string-map! (lambda (c)
                        (integer->char (- 219 (char->integer c))))
                      s 1 3)
         s))

(test '(#\o #\l #\l #\e #\h . #t)
       (string-fold cons #t "hello"))
(test '(#\l #\e . #t)
       (string-fold cons #t "hello" 1 3))
(test '(#\h #\e #\l #\l #\o . #t)
       (string-fold-right cons #t "hello"))
(test '(#\e #\l . #t)
       (string-fold-right cons #t "hello" 1 3))

(test "hello"
       (string-unfold null? car cdr '(#\h #\e #\l #\l #\o)))
(test "hi hello"
       (string-unfold null? car cdr '(#\h #\e #\l #\l #\o) "hi "))
(test "hi hello ho"
       (string-unfold null? car cdr
                      '(#\h #\e #\l #\l #\o) "hi "
                      (lambda (x) " ho")))

(test "olleh"
       (string-unfold-right null? car cdr '(#\h #\e #\l #\l #\o)))
(test "olleh hi"
       (string-unfold-right null? car cdr '(#\h #\e #\l #\l #\o) " hi"))
(test "ho olleh hi"
       (string-unfold-right null? car cdr
                            '(#\h #\e #\l #\l #\o) " hi"
                            (lambda (x) "ho ")))

(test "CLtL"
       (let ((out (open-output-string))
             (prev #f))
         (string-for-each (lambda (c)
                            (if (or (not prev)
                                    (char-whitespace? prev))
                                (write-char c out))
                            (set! prev c))
                          "Common Lisp, the Language")

         (get-output-string out)))
(test "oLtL"
       (let ((out (open-output-string))
             (prev #f))
         (string-for-each (lambda (c)
                            (if (or (not prev)
                                    (char-whitespace? prev))
                                (write-char c out))
                            (set! prev c))
                          "Common Lisp, the Language" 1)
         (get-output-string out)))
(test "oL"
       (let ((out (open-output-string))
             (prev #f))
         (string-for-each (lambda (c)
                            (if (or (not prev)
                                    (char-whitespace? prev))
                                (write-char c out))
                            (set! prev c))
                          "Common Lisp, the Language" 1 10)
         (get-output-string out)))
(test '(4 3 2 1 0)
       (let ((r '()))
         (string-for-each-index (lambda (i) (set! r (cons i r))) "hello")
         r))
(test '(4 3 2 1)
       (let ((r '()))
         (string-for-each-index (lambda (i) (set! r (cons i r))) "hello" 1)
         r))
(test '(2 1)
       (let ((r '()))
         (string-for-each-index (lambda (i) (set! r (cons i r))) "hello" 1 3)
         r))

(test "cdefab"
       (xsubstring "abcdef" 2))
(test "efabcd"
       (xsubstring "abcdef" -2))
(test "abcabca"
       (xsubstring "abc" 0 7))
(test "abcabca"
       (xsubstring "abc"
                   30000000000000000000000000000000
                   30000000000000000000000000000007))
(test "defdefd"
       (xsubstring "abcdefg" 0 7 3 6))
(test ""
       (xsubstring "abcdefg" 9 9 3 6))

(test "ZZcdefabZZ"
       (let ((s (make-string 10 #\Z)))
         (string-xcopy! s 2 "abcdef" 2)
         s))
(test "ZZdefdefZZ"
       (let ((s (make-string 10 #\Z)))
         (string-xcopy! s 2 "abcdef" 0 6 3)
         s))

(test "abcdXYZghi"
       (string-replace "abcdefghi" "XYZ" 4 6))
(test "abcdZghi"
       (string-replace "abcdefghi" "XYZ" 4 6 2))
(test "abcdZefghi"
       (string-replace "abcdefghi" "XYZ" 4 4 2))
(test "abcdefghi"
       (string-replace "abcdefghi" "XYZ" 4 4 1 1))
(test "abcdhi"
       (string-replace "abcdefghi" "" 4 7))

(test '("Help" "make" "programs" "run," "run," "RUN!")
       (string-tokenize "Help make programs run, run, RUN!"))
(test '("Help" "make" "programs" "run" "run" "RUN")
       (string-tokenize "Help make programs run, run, RUN!"
                        char-set:letter))
(test '("programs" "run" "run" "RUN")
       (string-tokenize "Help make programs run, run, RUN!"
                        char-set:letter 10))
(test '("elp" "make" "programs" "run" "run")
       (string-tokenize "Help make programs run, run, RUN!"
                        char-set:lower-case))

(test "rrrr"
       (string-filter "Help make programs run, run, RUN!" #\r ))
(test "HelpmakeprogramsrunrunRUN"
       (string-filter "Help make programs run, run, RUN!"
                      char-set:letter))
(test "programsrunrun"
       (string-filter "Help make programs run, run, RUN!"
                      (lambda (c) (char-lower-case? c)) 10))
(test ""
       (string-filter "" (lambda (c) (char-lower-case? c))))
(test "Help make pogams un, un, RUN!"
       (string-delete "Help make programs run, run, RUN!" #\r))
(test "   , , !"
       (string-delete "Help make programs run, run, RUN!"
                      char-set:letter))
(test " , , RUN!"
       (string-delete "Help make programs run, run, RUN!"
                      (lambda (c) (char-lower-case? c)) 10))
(test ""
       (string-delete "" (lambda (c) (char-lower-case? c))))

(test-end)
