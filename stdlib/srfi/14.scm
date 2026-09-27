;;; srfi/14 -- SRFI 14, Character-set library.
;;;
;;; Written for Turmeric (r7rs-srfi-plan D6: the reference implementation is
;;; Latin-1 bit strings, and chibi's rests on its own integer sets).  A char
;;; set is a record around an inversion list: a vector of strictly increasing
;;; code points, an even number of them, and the set is the union of the
;;; half-open ranges [v0, v1), [v2, v3), ...  Every operation makes a new
;;; vector and never changes one, so sets share them freely.  Membership is a
;;; binary search; the algebra is one merge of two lists.
;;;
;;; The linear-update procedures (the ones ending in `!`) are the pure ones:
;;; SRFI 14 allows, and does not require, them to reuse their first argument,
;;; and a set here never changes once it is made.
;;;
;;; The standard sets follow SRFI 14's 2019 CharsetDefs note, which brings
;;; them to current Unicode: lower-case, upper-case and letter are the
;;; Lowercase, Uppercase and Alphabetic properties, whitespace White_Space,
;;; digit Nd, title-case Lt, punctuation P*, symbol S*, graphic L* N* M* S*
;;; P*, blank Zs and U+0009 -- read from the tables of stdlib/r7rs/unicode.tur
;;; (tools/gen-r7rs-unicode.py) the first time a program uses each set, so an
;;; import costs nothing at startup.  char-set:full is every character: every
;;; Unicode scalar value, the surrogates U+D800-U+DFFF left out.
;;; docs/upcoming/r7rs-srfi-plan.md, S5.
(define-library (srfi 14)
  (export
    char-set? char-set= char-set<= char-set-hash
    char-set-cursor char-set-ref char-set-cursor-next end-of-char-set?
    char-set-fold char-set-unfold char-set-unfold! char-set-for-each char-set-map
    char-set-copy char-set
    list->char-set string->char-set list->char-set! string->char-set!
    char-set-filter ucs-range->char-set ->char-set
    char-set-filter! ucs-range->char-set!
    char-set->list char-set->string
    char-set-size char-set-count char-set-contains? char-set-every char-set-any
    char-set-adjoin char-set-delete char-set-adjoin! char-set-delete!
    char-set-complement char-set-union char-set-intersection
    char-set-complement! char-set-union! char-set-intersection!
    char-set-difference char-set-xor char-set-diff+intersection
    char-set-difference! char-set-xor! char-set-diff+intersection!
    char-set:lower-case char-set:upper-case char-set:title-case
    char-set:letter char-set:digit char-set:letter+digit
    char-set:graphic char-set:printing char-set:whitespace
    char-set:iso-control char-set:punctuation char-set:symbol
    char-set:hex-digit char-set:blank char-set:ascii
    char-set:empty char-set:full)
  (import (scheme base))
  (begin

    ;; ---- the record ----------------------------------------------------

    ;; `bounds` is the inversion list, or #f for a standard set not built
    ;; yet; `which` names that standard set (see build-standard).
    (define-record-type <char-set>
      (make-cs bounds which)
      char-set?
      (bounds cs-bounds set-cs-bounds!)
      (which cs-which))

    (define (bounds->cs v) (make-cs v #f))

    (define (cs-v cs)
      (or (cs-bounds cs)
          (let ((v (build-standard (cs-which cs))))
            (set-cs-bounds! cs v)
            v)))

    ;; ---- inversion lists -----------------------------------------------

    ;; How many bounds are <= x: odd when x is a member.
    (define (count-le v x)
      (let loop ((lo 0) (hi (vector-length v)))
        (if (< lo hi)
            (let ((mid (quotient (+ lo hi) 2)))
              (if (<= (vector-ref v mid) x) (loop (+ mid 1) hi) (loop lo mid)))
            lo)))

    (define (member? v x) (odd? (count-le v x)))

    ;; The least member >= x, or #f.
    (define (next-member v x)
      (let ((k (count-le v x)))
        (cond ((odd? k) x)
              ((< k (vector-length v)) (vector-ref v k))
              (else #f))))

    ;; Push the range [lo, hi) onto `out`, a bounds list in reverse, joining
    ;; it to the last range when they meet or overlap.  Ranges come in
    ;; ascending order of lo.
    (define (add-range out lo hi)
      (cond ((null? out) (list hi lo))
            ((< (car out) lo) (cons hi (cons lo out)))
            ((< (car out) hi) (cons hi (cdr out)))
            (else out)))

    (define (finish out) (list->vector (reverse out)))

    ;; op: or, and, minus (in a, not in b), xor.
    (define (combine op x y)
      (case op
        ((or) (or x y))
        ((and) (and x y))
        ((minus) (and x (not y)))
        (else (not (eq? x y)))))

    ;; The bounds of a op b: one pass over both, in ascending order,
    ;; emitting a bound wherever membership of the result changes.
    (define (sweep op a b)
      (let ((na (vector-length a)) (nb (vector-length b)))
        (let loop ((i 0) (j 0) (in-a #f) (in-b #f) (in #f) (out '()))
          (if (and (= i na) (= j nb))
              (finish out)
              (let* ((p (cond ((= i na) (vector-ref b j))
                              ((= j nb) (vector-ref a i))
                              (else (min (vector-ref a i) (vector-ref b j)))))
                     (step-a (and (< i na) (= (vector-ref a i) p)))
                     (step-b (and (< j nb) (= (vector-ref b j) p)))
                     (now-a (if step-a (not in-a) in-a))
                     (now-b (if step-b (not in-b) in-b))
                     (now (combine op now-a now-b)))
                (loop (if step-a (+ i 1) i) (if step-b (+ j 1) j) now-a now-b now
                      (if (eq? now in) out (cons p out))))))))

    (define full-bounds (vector 0 #xD800 #xE000 #x110000))

    ;; ---- code point lists ----------------------------------------------

    (define (append-reverse rev tail)
      (if (null? rev) tail (append-reverse (cdr rev) (cons (car rev) tail))))

    (define (merge-ints a b)
      (let loop ((a a) (b b) (acc '()))
        (cond ((null? a) (append-reverse acc b))
              ((null? b) (append-reverse acc a))
              ((<= (car a) (car b)) (loop (cdr a) b (cons (car a) acc)))
              (else (loop a (cdr b) (cons (car b) acc))))))

    ;; A bottom-up merge sort: no deep recursion on long lists.
    (define (sort-ints xs)
      (let pass ((runs (map (lambda (x) (list x)) xs)))
        (cond ((null? runs) '())
              ((null? (cdr runs)) (car runs))
              (else
               (pass (let pairs ((rs runs) (acc '()))
                       (cond ((null? rs) (reverse acc))
                             ((null? (cdr rs)) (reverse (cons (car rs) acc)))
                             (else (pairs (cddr rs) (cons (merge-ints (car rs) (cadr rs)) acc))))))))))

    ;; The bounds of a list of code points, in any order, repeats allowed.
    (define (points->bounds pts)
      (let loop ((pts (sort-ints pts)) (out '()))
        (if (null? pts)
            (finish out)
            (loop (cdr pts) (add-range out (car pts) (+ (car pts) 1))))))

    (define (chars->bounds chars) (points->bounds (map char->integer chars)))

    ;; (kons c acc) over the set's code points c in ascending order.
    (define (fold-points kons knil cs)
      (let ((v (cs-v cs)))
        (let loop ((k 0) (acc knil))
          (if (= k (vector-length v))
              acc
              (let ((hi (vector-ref v (+ k 1))))
                (let inner ((c (vector-ref v k)) (acc acc))
                  (if (= c hi)
                      (loop (+ k 2) acc)
                      (inner (+ c 1) (kons c acc)))))))))

    ;; The first true (pred c) over the members in ascending order, or #f.
    (define (any-point pred cs)
      (let ((v (cs-v cs)))
        (let loop ((k 0))
          (if (= k (vector-length v))
              #f
              (let ((hi (vector-ref v (+ k 1))))
                (let inner ((c (vector-ref v k)))
                  (if (= c hi)
                      (loop (+ k 2))
                      (or (pred c) (inner (+ c 1))))))))))

    (define (base-bounds base)
      (if (pair? base) (cs-v (car base)) (vector)))

    ;; ---- general procedures --------------------------------------------

    (define (bounds=? a b)
      (and (= (vector-length a) (vector-length b))
           (let loop ((k 0))
             (or (= k (vector-length a))
                 (and (= (vector-ref a k) (vector-ref b k)) (loop (+ k 1)))))))

    (define (char-set= . css)
      (or (null? css)
          (let loop ((v (cs-v (car css))) (rest (cdr css)))
            (or (null? rest)
                (and (bounds=? v (cs-v (car rest)))
                     (loop v (cdr rest)))))))

    (define (char-set<= . css)
      (or (null? css)
          (let loop ((v (cs-v (car css))) (rest (cdr css)))
            (or (null? rest)
                (let ((w (cs-v (car rest))))
                  (and (= 0 (vector-length (sweep 'minus v w)))
                       (loop w (cdr rest))))))))

    ;; A bound of 0 or none: the hash's own range, [0, 2^30).
    (define (char-set-hash cs . bound)
      (let* ((v (cs-v cs))
             (h (let loop ((k 0) (h (vector-length v)))
                  (if (= k (vector-length v))
                      h
                      (loop (+ k 1) (modulo (+ (* h 31) (vector-ref v k)) 1073741789))))))
        (if (and (pair? bound) (> (car bound) 0)) (modulo h (car bound)) h)))

    ;; ---- iteration -----------------------------------------------------

    ;; A cursor is the code point it stands at; #f is the end.
    (define (char-set-cursor cs) (next-member (cs-v cs) 0))
    (define (char-set-ref cs cursor) (integer->char cursor))
    (define (char-set-cursor-next cs cursor) (next-member (cs-v cs) (+ cursor 1)))
    (define (end-of-char-set? cursor) (not cursor))

    (define (char-set-fold kons knil cs)
      (fold-points (lambda (c acc) (kons (integer->char c) acc)) knil cs))

    (define (char-set-unfold p f g seed . base)
      (let loop ((seed seed) (acc '()))
        (if (p seed)
            (bounds->cs (sweep 'or (base-bounds base) (chars->bounds acc)))
            (loop (g seed) (cons (f seed) acc)))))
    (define char-set-unfold! char-set-unfold)

    (define (char-set-for-each proc cs)
      (fold-points (lambda (c acc) (proc (integer->char c)) acc) #f cs)
      (if #f #f))

    (define (char-set-map proc cs)
      (bounds->cs (chars->bounds (fold-points (lambda (c acc) (cons (proc (integer->char c)) acc)) '() cs))))

    ;; ---- creating char sets --------------------------------------------

    (define (char-set-copy cs) (bounds->cs (cs-v cs)))

    (define (char-set . chars) (bounds->cs (chars->bounds chars)))

    (define (list->char-set chars . base)
      (bounds->cs (sweep 'or (base-bounds base) (chars->bounds chars))))
    (define list->char-set! list->char-set)

    (define (string->char-set s . base)
      (bounds->cs (sweep 'or (base-bounds base) (chars->bounds (string->list s)))))
    (define string->char-set! string->char-set)

    (define (char-set-filter pred cs . base)
      (bounds->cs
       (sweep 'or (base-bounds base)
              (points->bounds
               (fold-points (lambda (c acc) (if (pred (integer->char c)) (cons c acc) acc)) '() cs)))))
    (define char-set-filter! char-set-filter)

    ;; The characters in [lower, upper).  A range reaching past U+10FFFF or
    ;; into the surrogates holds code points that are not characters: with
    ;; error? true that is an error, else they are left out.
    (define (ucs-range->char-set lower upper . rest)
      (let ((error? (and (pair? rest) (car rest)))
            (base (if (and (pair? rest) (pair? (cdr rest))) (cdr rest) '())))
        (if (and error? (< lower upper)
                 (or (> upper #x110000) (and (< lower #xE000) (> upper #xD800))))
            (error "ucs-range->char-set: the range holds code points that are not characters" lower upper))
        (bounds->cs
         (sweep 'or (base-bounds base)
                (if (< lower upper)
                    (sweep 'and full-bounds (vector lower upper))
                    (vector))))))
    (define ucs-range->char-set! ucs-range->char-set)

    (define (->char-set x)
      (cond ((char-set? x) x)
            ((string? x) (string->char-set x))
            ((char? x) (char-set x))
            ((or (pair? x) (null? x)) (list->char-set x))
            (else (error "->char-set: not a char set, string, char or list of chars" x))))

    ;; ---- querying char sets --------------------------------------------

    (define (char-set->list cs)
      (reverse (fold-points (lambda (c acc) (cons (integer->char c) acc)) '() cs)))

    (define (char-set->string cs) (list->string (char-set->list cs)))

    (define (char-set-size cs)
      (let ((v (cs-v cs)))
        (let loop ((k 0) (n 0))
          (if (= k (vector-length v))
              n
              (loop (+ k 2) (+ n (- (vector-ref v (+ k 1)) (vector-ref v k))))))))

    (define (char-set-count pred cs)
      (fold-points (lambda (c n) (if (pred (integer->char c)) (+ n 1) n)) 0 cs))

    (define (char-set-contains? cs char) (member? (cs-v cs) (char->integer char)))

    (define (char-set-every pred cs)
      (not (any-point (lambda (c) (not (pred (integer->char c)))) cs)))

    (define (char-set-any pred cs)
      (any-point (lambda (c) (pred (integer->char c))) cs))

    ;; ---- char-set algebra ----------------------------------------------

    (define (char-set-adjoin cs . chars)
      (bounds->cs (sweep 'or (cs-v cs) (chars->bounds chars))))
    (define char-set-adjoin! char-set-adjoin)

    (define (char-set-delete cs . chars)
      (bounds->cs (sweep 'minus (cs-v cs) (chars->bounds chars))))
    (define char-set-delete! char-set-delete)

    (define (char-set-complement cs) (bounds->cs (sweep 'minus full-bounds (cs-v cs))))
    (define char-set-complement! char-set-complement)

    (define (fold-bounds op init css)
      (let loop ((v init) (css css))
        (if (null? css) v (loop (sweep op v (cs-v (car css))) (cdr css)))))

    (define (char-set-union . css) (bounds->cs (fold-bounds 'or (vector) css)))
    (define char-set-union! char-set-union)

    (define (char-set-intersection . css) (bounds->cs (fold-bounds 'and full-bounds css)))
    (define char-set-intersection! char-set-intersection)

    (define (char-set-difference cs . css)
      (bounds->cs (sweep 'minus (cs-v cs) (fold-bounds 'or (vector) css))))
    (define char-set-difference! char-set-difference)

    (define (char-set-xor . css) (bounds->cs (fold-bounds 'xor (vector) css)))
    (define char-set-xor! char-set-xor)

    ;; Partitions cs: the part in none of css, and the part in some.
    (define (char-set-diff+intersection cs . css)
      (let ((v (cs-v cs)) (others (fold-bounds 'or (vector) css)))
        (values (bounds->cs (sweep 'minus v others))
                (bounds->cs (sweep 'and v others)))))
    (define char-set-diff+intersection! char-set-diff+intersection)

    ;; ---- standard char sets --------------------------------------------

    ;; 0-9: the runs of stdlib/r7rs/unicode.tur's tables (r7rs-uc-run__),
    ;; which expand stride-2 runs into single code points.  10 and 11 are
    ;; unions of those.
    (define (build-standard which)
      (case which
        ((10) (sweep 'or (cs-v char-set:letter) (cs-v char-set:digit)))
        ((11) (sweep 'or (cs-v char-set:graphic) (cs-v char-set:whitespace)))
        (else
         (let loop ((i 0) (out '()))
           (let ((lo (r7rs-uc-run__ which i 0)))
             (if (< lo 0)
                 (finish out)
                 (let ((hi (r7rs-uc-run__ which i 1))
                       (stride (r7rs-uc-run__ which i 2)))
                   (loop (+ i 1)
                         (if (= stride 1)
                             (add-range out lo (+ hi 1))
                             (let each ((c lo) (out out))
                               (if (> c hi)
                                   out
                                   (each (+ c stride) (add-range out c (+ c 1))))))))))))))

    (define char-set:letter (make-cs #f 0))
    (define char-set:upper-case (make-cs #f 1))
    (define char-set:lower-case (make-cs #f 2))
    (define char-set:whitespace (make-cs #f 3))
    (define char-set:digit (make-cs #f 4))
    (define char-set:title-case (make-cs #f 5))
    (define char-set:punctuation (make-cs #f 6))
    (define char-set:symbol (make-cs #f 7))
    (define char-set:graphic (make-cs #f 8))
    (define char-set:blank (make-cs #f 9))
    (define char-set:letter+digit (make-cs #f 10))
    (define char-set:printing (make-cs #f 11))
    (define char-set:iso-control (bounds->cs (vector 0 #x20 #x7F #xA0)))
    (define char-set:hex-digit (bounds->cs (vector #x30 #x3A #x41 #x47 #x61 #x67)))
    (define char-set:ascii (bounds->cs (vector 0 #x80)))
    (define char-set:empty (bounds->cs (vector)))
    (define char-set:full (bounds->cs full-bounds))))
