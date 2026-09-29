;;; srfi/13 -- SRFI 13, String libraries.
;;;
;;; Written for Turmeric (r7rs-srfi-plan D6).  The reference implementation
;;; indexes its strings with string-ref, and string-ref on a literal scans
;;; the UTF-8 from the start (plan 2.5), so it is quadratic here.  Each
;;; procedure below turns the part of a string it reads into a vector of
;;; characters once (string->vector) and works over that; every string it
;;; returns is a fresh, mutable one.
;;;
;;; The names SRFI 13 shares with (scheme base) and (scheme char) are R7RS's
;;; own procedures wherever SRFI 13's is a compatible extension (D5):
;;; string->list, string-copy, string-copy!, string-fill!, string-append and
;;; the rest, and string-upcase and string-downcase, which take SRFI 13's
;;; optional start and end.  Those two map with R7RS's full case mapping
;;; ("stra\xDF;e" upcases to "STRASSE"); the in-place string-upcase! and
;;; string-downcase! map one character to one, as SRFI 13 says.
;;; string-map and string-for-each are not compatible -- SRFI 13's take one
;;; string and a range, R7RS's several strings -- so importing this library
;;; next to (scheme base) needs (except (scheme base) string-map
;;; string-for-each), or a prefix on one of them.
;;;
;;; Char sets come from (srfi 14).  A char/char-set/pred argument may be a
;;; character, a char set or a predicate.  string-filter and string-delete
;;; take it first, as SRFI 13 says; the string-first order of the SRFI's
;;; drafts (which Guile and Gauche kept) is accepted too, since a criterion
;;; is never a string.  Case-insensitive comparison folds a character with
;;; (char-downcase (char-upcase c)), as SRFI 13 specifies.
;;; docs/archive/r7rs-srfi-plan.md, S5.
(define-library (srfi 13)
  (export
    string? string-null? string-every string-any
    make-string string string-tabulate
    string->list list->string reverse-list->string string-join
    string-length string-ref string-copy substring/shared string-copy!
    string-take string-take-right string-drop string-drop-right
    string-pad string-pad-right string-trim string-trim-right string-trim-both
    string-set! string-fill!
    string-compare string-compare-ci
    string<> string= string< string> string<= string>=
    string-ci<> string-ci= string-ci< string-ci> string-ci<= string-ci>=
    string-hash string-hash-ci
    string-prefix-length string-suffix-length
    string-prefix-length-ci string-suffix-length-ci
    string-prefix? string-suffix? string-prefix-ci? string-suffix-ci?
    string-index string-index-right string-skip string-skip-right string-count
    string-contains string-contains-ci
    string-titlecase string-upcase string-downcase
    string-titlecase! string-upcase! string-downcase!
    string-reverse string-reverse! string-append
    string-concatenate string-concatenate/shared string-append/shared
    string-concatenate-reverse string-concatenate-reverse/shared
    string-map string-map! string-fold string-fold-right
    string-unfold string-unfold-right string-for-each string-for-each-index
    xsubstring string-xcopy!
    string-replace string-tokenize
    string-filter string-delete
    string-parse-start+end string-parse-final-start+end let-string-start+end
    check-substring-spec substring-spec-ok?
    make-kmp-restart-vector kmp-step string-kmp-partial-search)
  (import (scheme base) (scheme char) (srfi 14))
  (begin

    ;; ---- start/end arguments -------------------------------------------

    (define (substring-spec-ok? s start end)
      (and (string? s) (exact-integer? start) (exact-integer? end)
           (<= 0 start end (string-length s))))

    (define (check-substring-spec proc s start end)
      (if (not (substring-spec-ok? s start end))
          (error "not a valid substring of the string" proc s start end)))

    ;; (values rest start end): an optional start and end from args.
    (define (string-parse-start+end proc s args)
      (if (not (string? s)) (error "not a string" proc s))
      (let ((len (string-length s)))
        (if (pair? args)
            (let ((start (car args)))
              (if (not (and (exact-integer? start) (<= 0 start len)))
                  (error "start index out of range" proc start s))
              (if (pair? (cdr args))
                  (let ((end (cadr args)))
                    (if (not (and (exact-integer? end) (<= start end len)))
                        (error "end index out of range" proc start end s))
                    (values (cddr args) start end))
                  (values '() start len)))
            (values '() 0 len))))

    (define (string-parse-final-start+end proc s args)
      (call-with-values (lambda () (string-parse-start+end proc s args))
        (lambda (rest start end)
          (if (pair? rest) (error "too many arguments" proc rest))
          (values start end))))

    (define-syntax let-string-start+end
      (syntax-rules ()
        ((_ (start end rest) proc s args body ...)
         (call-with-values (lambda () (string-parse-start+end proc s args))
           (lambda (rest start end) body ...)))
        ((_ (start end) proc s args body ...)
         (call-with-values (lambda () (string-parse-final-start+end proc s args))
           (lambda (start end) body ...)))))

    ;; The characters [start, end) of s, for the procedures that read s.
    (define (chars who s args k)
      (let-string-start+end (start end) who s args
        (k (string->vector s start end) start)))

    ;; Two strings and their optional ranges: (k v1 start1 v2 start2).
    (define (two-chars who s1 s2 args k)
      (let-string-start+end (start1 end1 rest) who s1 args
        (let-string-start+end (start2 end2) who s2 rest
          (k (string->vector s1 start1 end1) start1 (string->vector s2 start2 end2) start2))))

    ;; An optional leading argument, then the rest: (k value rest).
    (define (opt args default k)
      (if (pair? args) (k (car args) (cdr args)) (k default '())))

    (define (criterion who c)
      (cond ((char? c) (lambda (x) (char=? x c)))
            ((char-set? c) (lambda (x) (char-set-contains? c x)))
            ((procedure? c) c)
            (else (error "not a character, char set or predicate" who c))))

    (define (ci c) (char-downcase (char-upcase c)))
    (define (same a b) (char=? a b))
    (define (same-ci a b) (char=? (ci a) (ci b)))

    ;; ---- predicates ----------------------------------------------------

    (define (string-null? s) (= 0 (string-length s)))

    ;; The last application of a predicate is a tail call, and its value the
    ;; answer (SRFI 13's "witness").
    (define (string-every crit s . args)
      (chars 'string-every s args
        (lambda (v start)
          (let ((ok? (criterion 'string-every crit)) (n (vector-length v)))
            (let loop ((i 0))
              (cond ((= i n) #t)
                    ((= i (- n 1)) (ok? (vector-ref v i)))
                    ((ok? (vector-ref v i)) (loop (+ i 1)))
                    (else #f)))))))

    (define (string-any crit s . args)
      (chars 'string-any s args
        (lambda (v start)
          (let ((ok? (criterion 'string-any crit)) (n (vector-length v)))
            (let loop ((i 0))
              (cond ((= i n) #f)
                    ((= i (- n 1)) (ok? (vector-ref v i)))
                    (else (or (ok? (vector-ref v i)) (loop (+ i 1))))))))))

    ;; ---- constructors and lists ----------------------------------------

    (define (string-tabulate proc len)
      (let ((v (make-vector len)))
        (do ((i 0 (+ i 1))) ((= i len) (vector->string v))
          (vector-set! v i (proc i)))))

    (define (reverse-list->string chars) (list->string (reverse chars)))

    ;; The strings with delim before (prefix) or after (suffix) each.
    (define (delimit strings delim after?)
      (let loop ((strings strings) (acc '()))
        (if (null? strings)
            (reverse acc)
            (loop (cdr strings)
                  (if after?
                      (cons delim (cons (car strings) acc))
                      (cons (car strings) (cons delim acc)))))))

    (define (string-join strings . args)
      (let ((delim (if (pair? args) (car args) " "))
            (grammar (if (and (pair? args) (pair? (cdr args))) (cadr args) 'infix)))
        (case grammar
          ((infix strict-infix)
           (cond ((pair? strings)
                  (string-concatenate (cons (car strings) (delimit (cdr strings) delim #f))))
                 ((eq? grammar 'strict-infix)
                  (error "string-join: an empty list with the strict-infix grammar"))
                 (else (string))))
          ((prefix) (string-concatenate (delimit strings delim #f)))
          ((suffix) (string-concatenate (delimit strings delim #t)))
          (else (error "string-join: the grammar is infix, strict-infix, prefix or suffix" grammar)))))

    ;; ---- selection -----------------------------------------------------

    (define (substring/shared s start . end)
      (let-string-start+end (a b) 'substring/shared s (cons start end)
        (string-copy s a b)))

    (define (check-count who s n)
      (if (not (and (exact-integer? n) (<= 0 n (string-length s))))
          (error "count out of range" who n s)))

    (define (string-take s n) (check-count 'string-take s n) (string-copy s 0 n))
    (define (string-drop s n) (check-count 'string-drop s n) (string-copy s n))
    (define (string-take-right s n)
      (check-count 'string-take-right s n)
      (let ((len (string-length s))) (string-copy s (- len n) len)))
    (define (string-drop-right s n)
      (check-count 'string-drop-right s n)
      (string-copy s 0 (- (string-length s) n)))

    (define (string-pad s len . args)
      (opt args #\space
        (lambda (c args)
          (let-string-start+end (start end) 'string-pad s args
            (let ((n (- end start)))
              (if (<= len n)
                  (string-copy s (- end len) end)
                  (string-append (make-string (- len n) c) (string-copy s start end))))))))

    (define (string-pad-right s len . args)
      (opt args #\space
        (lambda (c args)
          (let-string-start+end (start end) 'string-pad-right s args
            (let ((n (- end start)))
              (if (<= len n)
                  (string-copy s start (+ start len))
                  (string-append (string-copy s start end) (make-string (- len n) c))))))))

    ;; The first index in [i, n) whose character fails ok?, or n.
    (define (skip-left ok? v i n)
      (if (and (< i n) (ok? (vector-ref v i))) (skip-left ok? v (+ i 1) n) i))
    ;; One past the last index in [lo, j) whose character fails ok?, or lo.
    (define (skip-right ok? v lo j)
      (if (and (> j lo) (ok? (vector-ref v (- j 1)))) (skip-right ok? v lo (- j 1)) j))

    (define (trim who s args left? right?)
      (opt args char-set:whitespace
        (lambda (crit args)
          (chars who s args
            (lambda (v start)
              (let* ((ok? (criterion who crit))
                     (n (vector-length v))
                     (a (if left? (skip-left ok? v 0 n) 0))
                     (b (if right? (skip-right ok? v a n) n)))
                (vector->string v a b)))))))

    (define (string-trim s . args) (trim 'string-trim s args #t #f))
    (define (string-trim-right s . args) (trim 'string-trim-right s args #f #t))
    (define (string-trim-both s . args) (trim 'string-trim-both s args #t #t))

    ;; ---- comparison ----------------------------------------------------

    (define (prefix-len v1 v2 same?)
      (let ((n (min (vector-length v1) (vector-length v2))))
        (let loop ((i 0))
          (if (and (< i n) (same? (vector-ref v1 i) (vector-ref v2 i))) (loop (+ i 1)) i))))

    (define (suffix-len v1 v2 same?)
      (let ((n1 (vector-length v1)) (n2 (vector-length v2)))
        (let loop ((i 0))
          (if (and (< i n1) (< i n2)
                   (same? (vector-ref v1 (- n1 i 1)) (vector-ref v2 (- n2 i 1))))
              (loop (+ i 1))
              i))))

    ;; (k order index): order -1, 0 or 1 as the range of s1 is less than,
    ;; equal to or greater than that of s2, and the mismatch index into s1.
    (define (compare who s1 s2 args fold? k)
      (two-chars who s1 s2 args
        (lambda (v1 start1 v2 start2)
          (let* ((n1 (vector-length v1)) (n2 (vector-length v2))
                 (i (prefix-len v1 v2 (if fold? same-ci same)))
                 (at (+ start1 i)))
            (cond ((and (= i n1) (= i n2)) (k 0 at))
                  ((= i n1) (k -1 at))
                  ((= i n2) (k 1 at))
                  ((let ((a (vector-ref v1 i)) (b (vector-ref v2 i)))
                     (if fold? (char<? (ci a) (ci b)) (char<? a b)))
                   (k -1 at))
                  (else (k 1 at)))))))

    (define (string-compare s1 s2 lt eq gt . args)
      (compare 'string-compare s1 s2 args #f
               (lambda (o i) (cond ((< o 0) (lt i)) ((= o 0) (eq i)) (else (gt i))))))
    (define (string-compare-ci s1 s2 lt eq gt . args)
      (compare 'string-compare-ci s1 s2 args #t
               (lambda (o i) (cond ((< o 0) (lt i)) ((= o 0) (eq i)) (else (gt i))))))

    (define (order who s1 s2 args fold?) (compare who s1 s2 args fold? (lambda (o i) o)))

    (define (string= s1 s2 . args) (= (order 'string= s1 s2 args #f) 0))
    (define (string<> s1 s2 . args) (not (= (order 'string<> s1 s2 args #f) 0)))
    (define (string< s1 s2 . args) (< (order 'string< s1 s2 args #f) 0))
    (define (string> s1 s2 . args) (> (order 'string> s1 s2 args #f) 0))
    (define (string<= s1 s2 . args) (<= (order 'string<= s1 s2 args #f) 0))
    (define (string>= s1 s2 . args) (>= (order 'string>= s1 s2 args #f) 0))
    (define (string-ci= s1 s2 . args) (= (order 'string-ci= s1 s2 args #t) 0))
    (define (string-ci<> s1 s2 . args) (not (= (order 'string-ci<> s1 s2 args #t) 0)))
    (define (string-ci< s1 s2 . args) (< (order 'string-ci< s1 s2 args #t) 0))
    (define (string-ci> s1 s2 . args) (> (order 'string-ci> s1 s2 args #t) 0))
    (define (string-ci<= s1 s2 . args) (<= (order 'string-ci<= s1 s2 args #t) 0))
    (define (string-ci>= s1 s2 . args) (>= (order 'string-ci>= s1 s2 args #t) 0))

    ;; The prelude's string hash (stdlib/r7rs/prelude.tur, "hashing"), over
    ;; the range -- case-folded for string-hash-ci.  A bound of 0 or none is
    ;; the hash's own range, [0, 2^30).
    (define (hash who s args fold?)
      (opt args 0
        (lambda (bound args)
          (chars who s args
            (lambda (v start)
              (let ((h (r7rs-string-hash__ (vector->string (if fold? (vector-map ci v) v)))))
                (if (and (exact-integer? bound) (> bound 0)) (modulo h bound) h)))))))

    (define (string-hash s . args) (hash 'string-hash s args #f))
    (define (string-hash-ci s . args) (hash 'string-hash-ci s args #t))

    ;; ---- prefixes and suffixes -----------------------------------------

    (define (affix who s1 s2 args len same? whole?)
      (two-chars who s1 s2 args
        (lambda (v1 start1 v2 start2)
          (let ((n (len v1 v2 same?)))
            (if whole? (= n (vector-length v1)) n)))))

    (define (string-prefix-length s1 s2 . args) (affix 'string-prefix-length s1 s2 args prefix-len same #f))
    (define (string-suffix-length s1 s2 . args) (affix 'string-suffix-length s1 s2 args suffix-len same #f))
    (define (string-prefix-length-ci s1 s2 . args) (affix 'string-prefix-length-ci s1 s2 args prefix-len same-ci #f))
    (define (string-suffix-length-ci s1 s2 . args) (affix 'string-suffix-length-ci s1 s2 args suffix-len same-ci #f))
    (define (string-prefix? s1 s2 . args) (affix 'string-prefix? s1 s2 args prefix-len same #t))
    (define (string-suffix? s1 s2 . args) (affix 'string-suffix? s1 s2 args suffix-len same #t))
    (define (string-prefix-ci? s1 s2 . args) (affix 'string-prefix-ci? s1 s2 args prefix-len same-ci #t))
    (define (string-suffix-ci? s1 s2 . args) (affix 'string-suffix-ci? s1 s2 args suffix-len same-ci #t))

    ;; ---- searching -----------------------------------------------------

    ;; The index into s of the first (from-right?: last) character whose
    ;; (ok? c) is want, or #f.
    (define (search-char who s crit args want from-right?)
      (chars who s args
        (lambda (v start)
          (let ((ok? (criterion who crit)) (n (vector-length v)))
            (if from-right?
                (let loop ((i (- n 1)))
                  (cond ((< i 0) #f)
                        ((eq? (if (ok? (vector-ref v i)) #t #f) want) (+ start i))
                        (else (loop (- i 1)))))
                (let loop ((i 0))
                  (cond ((= i n) #f)
                        ((eq? (if (ok? (vector-ref v i)) #t #f) want) (+ start i))
                        (else (loop (+ i 1))))))))))

    (define (string-index s crit . args) (search-char 'string-index s crit args #t #f))
    (define (string-index-right s crit . args) (search-char 'string-index-right s crit args #t #t))
    (define (string-skip s crit . args) (search-char 'string-skip s crit args #f #f))
    (define (string-skip-right s crit . args) (search-char 'string-skip-right s crit args #f #t))

    (define (string-count s crit . args)
      (chars 'string-count s args
        (lambda (v start)
          (let ((ok? (criterion 'string-count crit)))
            (let loop ((i 0) (n 0))
              (if (= i (vector-length v))
                  n
                  (loop (+ i 1) (if (ok? (vector-ref v i)) (+ n 1) n))))))))

    ;; The first i where v2 occurs in v1, or #f.
    (define (find-sub v1 v2 same?)
      (let ((n1 (vector-length v1)) (n2 (vector-length v2)))
        (let loop ((i 0))
          (cond ((> (+ i n2) n1) #f)
                ((let match ((k 0))
                   (or (= k n2)
                       (and (same? (vector-ref v1 (+ i k)) (vector-ref v2 k))
                            (match (+ k 1)))))
                 i)
                (else (loop (+ i 1)))))))

    (define (contains who s1 s2 args same?)
      (two-chars who s1 s2 args
        (lambda (v1 start1 v2 start2)
          (let ((i (find-sub v1 v2 same?)))
            (and i (+ start1 i))))))

    (define (string-contains s1 s2 . args) (contains 'string-contains s1 s2 args same))
    (define (string-contains-ci s1 s2 . args) (contains 'string-contains-ci s1 s2 args same-ci))

    ;; ---- case mapping --------------------------------------------------

    ;; stdlib/r7rs/unicode.tur's simple titlecase mapping (op 3).
    (define (char-titlecase c) (integer->char (r7rs-uc-map__ (char->integer c) 3)))

    ;; Cased: Lowercase, Uppercase or Lt (Unicode's definition).
    (define (cased? c)
      (or (char-upper-case? c) (char-lower-case? c) (char-set-contains? char-set:title-case c)))

    ;; A character after a cased one is downcased, any other titlecased; a
    ;; range starts afresh, whatever comes before it.
    (define (titlecase-vector v)
      (let loop ((i 0) (prev #f))
        (if (< i (vector-length v))
            (let ((c (vector-ref v i)))
              (vector-set! v i (if prev (char-downcase c) (char-titlecase c)))
              (loop (+ i 1) (cased? c)))))
      v)

    (define (string-titlecase s . args)
      (chars 'string-titlecase s args
        (lambda (v start) (vector->string (titlecase-vector v)))))

    ;; Write v into s from index start.
    (define (store! s start v)
      (do ((i 0 (+ i 1))) ((= i (vector-length v)))
        (string-set! s (+ start i) (vector-ref v i))))

    (define (string-titlecase! s . args)
      (chars 'string-titlecase! s args
        (lambda (v start) (store! s start (titlecase-vector v)))))
    (define (string-upcase! s . args)
      (chars 'string-upcase! s args
        (lambda (v start) (store! s start (vector-map char-upcase v)))))
    (define (string-downcase! s . args)
      (chars 'string-downcase! s args
        (lambda (v start) (store! s start (vector-map char-downcase v)))))

    ;; ---- reverse and append --------------------------------------------

    (define (reversed v)
      (let* ((n (vector-length v)) (r (make-vector n)))
        (do ((i 0 (+ i 1))) ((= i n) r)
          (vector-set! r i (vector-ref v (- n i 1))))))

    (define (string-reverse s . args)
      (chars 'string-reverse s args (lambda (v start) (vector->string (reversed v)))))
    (define (string-reverse! s . args)
      (chars 'string-reverse! s args (lambda (v start) (store! s start (reversed v)))))

    ;; Not (apply string-append strings): apply takes at most eight
    ;; arguments here (r7rs-guide), and SRFI 13 names that idiom as the one
    ;; this procedure is for.  One fresh string, each piece copied in.
    (define (string-concatenate strings)
      (let ((out (make-string (let loop ((l strings) (n 0))
                                (if (null? l) n (loop (cdr l) (+ n (string-length (car l)))))))))
        (let loop ((l strings) (at 0))
          (if (null? l)
              out
              (begin
                (string-copy! out at (car l))
                (loop (cdr l) (+ at (string-length (car l)))))))))
    (define string-concatenate/shared string-concatenate)
    (define (string-append/shared . strings) (string-concatenate strings))

    (define (string-concatenate-reverse strings . args)
      (string-concatenate
       (reverse (if (pair? args)
                    (cons (if (pair? (cdr args))
                              (substring/shared (car args) 0 (cadr args))
                              (car args))
                          strings)
                    strings))))
    (define string-concatenate-reverse/shared string-concatenate-reverse)

    ;; ---- fold, unfold and map ------------------------------------------

    (define (string-map proc s . args)
      (chars 'string-map s args (lambda (v start) (vector->string (vector-map proc v)))))
    (define (string-map! proc s . args)
      (chars 'string-map! s args (lambda (v start) (store! s start (vector-map proc v)))))

    (define (string-fold kons knil s . args)
      (chars 'string-fold s args
        (lambda (v start)
          (let loop ((i 0) (acc knil))
            (if (= i (vector-length v)) acc (loop (+ i 1) (kons (vector-ref v i) acc)))))))

    (define (string-fold-right kons knil s . args)
      (chars 'string-fold-right s args
        (lambda (v start)
          (let loop ((i (- (vector-length v) 1)) (acc knil))
            (if (< i 0) acc (loop (- i 1) (kons (vector-ref v i) acc)))))))

    (define (no-final seed) "")

    (define (string-unfold p f g seed . args)
      (let ((base (if (pair? args) (car args) ""))
            (make-final (if (and (pair? args) (pair? (cdr args))) (cadr args) no-final)))
        (let loop ((seed seed) (acc '()))
          (if (p seed)
              (string-append base (list->string (reverse acc)) (make-final seed))
              (loop (g seed) (cons (f seed) acc))))))

    (define (string-unfold-right p f g seed . args)
      (let ((base (if (pair? args) (car args) ""))
            (make-final (if (and (pair? args) (pair? (cdr args))) (cadr args) no-final)))
        (let loop ((seed seed) (acc '()))
          (if (p seed)
              (string-append (make-final seed) (list->string acc) base)
              (loop (g seed) (cons (f seed) acc))))))

    (define (string-for-each proc s . args)
      (chars 'string-for-each s args
        (lambda (v start)
          (do ((i 0 (+ i 1))) ((= i (vector-length v)))
            (proc (vector-ref v i))))))

    (define (string-for-each-index proc s . args)
      (let-string-start+end (start end) 'string-for-each-index s args
        (do ((i start (+ i 1))) ((= i end))
          (proc i))))

    ;; ---- replicate and rotate ------------------------------------------

    (define (xsubstring s from . args)
      (let-string-start+end (start end) 'xsubstring s (if (pair? args) (cdr args) '())
        (let* ((v (string->vector s start end))
               (n (vector-length v))
               (to (if (pair? args) (car args) (+ from n))))
          (cond ((= from to) (string))
                ((= n 0) (error "xsubstring: an empty range cannot be replicated" s start end))
                (else
                 (let ((out (make-vector (- to from))))
                   (do ((k 0 (+ k 1))) ((= k (- to from)) (vector->string out))
                     (vector-set! out k (vector-ref v (modulo (+ from k) n))))))))))

    (define (string-xcopy! target tstart s sfrom . args)
      (string-copy! target tstart (apply xsubstring s sfrom args)))

    ;; ---- insertion and parsing -----------------------------------------

    (define (string-replace s1 s2 start1 end1 . args)
      (check-substring-spec 'string-replace s1 start1 end1)
      (let-string-start+end (start2 end2) 'string-replace s2 args
        (string-append (string-copy s1 0 start1) (string-copy s2 start2 end2) (string-copy s1 end1))))

    (define (string-tokenize s . args)
      (opt args char-set:graphic
        (lambda (cs args)
          (chars 'string-tokenize s args
            (lambda (v start)
              (let ((in? (lambda (c) (char-set-contains? cs c)))
                    (out? (lambda (c) (not (char-set-contains? cs c))))
                    (n (vector-length v)))
                (let loop ((i 0) (acc '()))
                  (let ((b (skip-left out? v i n)))
                    (if (= b n)
                        (reverse acc)
                        (let ((e (skip-left in? v b n)))
                          (loop e (cons (vector->string v b e) acc))))))))))))

    ;; ---- filtering and deleting ----------------------------------------

    (define (keep who crit s args want)
      (if (and (string? crit) (not (string? s)))
          (keep who s crit args want)
          (chars who s args
            (lambda (v start)
              (let ((ok? (criterion who crit)))
                (let loop ((i (- (vector-length v) 1)) (acc '()))
                  (if (< i 0)
                      (list->string acc)
                      (loop (- i 1)
                            (if (eq? (if (ok? (vector-ref v i)) #t #f) want)
                                (cons (vector-ref v i) acc)
                                acc)))))))))

    (define (string-filter crit s . args) (keep 'string-filter crit s args #t))
    (define (string-delete crit s . args) (keep 'string-delete crit s args #f))

    ;; ---- Knuth-Morris-Pratt search -------------------------------------

    ;; The reference implementation's algorithm, over a vector: rv[i] is
    ;; the length of the longest proper prefix of pattern[start, start+i)
    ;; that is also a suffix of it and is not followed by pattern[start+i],
    ;; or -1.
    (define (make-kmp-restart-vector pattern . args)
      (opt args char=?
        (lambda (c= args)
          (chars 'make-kmp-restart-vector pattern args
            (lambda (p start)
              (let* ((rvlen (vector-length p))
                     (rv (make-vector rvlen -1)))
                (if (> rvlen 0)
                    (let ((c0 (vector-ref p 0)))
                      (let lp1 ((i 0) (j -1) (k 0))
                        (if (< i (- rvlen 1))
                            (let lp2 ((j j))
                              (cond ((= j -1)
                                     (let ((i1 (+ i 1)))
                                       (if (not (c= (vector-ref p (+ k 1)) c0))
                                           (vector-set! rv i1 0))
                                       (lp1 i1 0 (+ k 1))))
                                    ((c= (vector-ref p k) (vector-ref p j))
                                     (let ((i1 (+ i 1)) (j1 (+ j 1)))
                                       (vector-set! rv i1 j1)
                                       (lp1 i1 j1 (+ k 1))))
                                    (else (lp2 (vector-ref rv j)))))))))
                rv))))))

    (define (kmp-step pat rv c i c= p-start)
      (let lp ((i i))
        (if (c= c (string-ref pat (+ i p-start)))
            (+ i 1)
            (let ((i (vector-ref rv i)))
              (if (= i -1) 0 (lp i))))))

    ;; -j on a match (j the index in s just past it), else the state to
    ;; carry into the next piece of text.
    (define (string-kmp-partial-search pat rv s i . args)
      (opt args char=?
        (lambda (c= args)
          (opt args 0
            (lambda (p-start args)
              (chars 'string-kmp-partial-search s args
                (lambda (v s-start)
                  (let ((plen (vector-length rv))
                        (p (string->vector pat)))
                    (let lp ((si 0) (vi i))
                      (cond ((= vi plen) (- (+ si s-start)))
                            ((= si (vector-length v)) vi)
                            (else
                             (lp (+ si 1)
                                 (let step ((k vi))
                                   (if (c= (vector-ref v si) (vector-ref p (+ k p-start)))
                                       (+ k 1)
                                       (let ((k (vector-ref rv k)))
                                         (if (= k -1) 0 (step k)))))))))))))))))))
