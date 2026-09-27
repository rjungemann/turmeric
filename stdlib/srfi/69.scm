;;; srfi/69 -- SRFI 69, Basic hash tables.
;;;
;;; Written for Turmeric (r7rs-srfi-plan D6: the reference implementation is
;;; tied to its host's hashing, and chibi's is C).  A table is a record over
;;; a vector of buckets, each an association list of (key . value) pairs,
;;; doubled when it holds more than two entries a bucket.  Everything in it
;;; is a Scheme value, so the collector sees it all.
;;;
;;; The hash functions are the prelude's (stdlib/r7rs/prelude.tur, "hashing"):
;;; `hash` agrees with equal?, `hash-by-identity` with eq? and eqv? (the same
;;; predicate here), and the string hashes with string=? and string-ci=?.
;;; Each takes SRFI 69's optional bound.
;;;
;;; A table made without a hash function hashes a string key case-folded and
;;; anything else with `hash`.  That is right for all five standard
;;; equivalences (eq?, eqv?, equal?, string=? and string-ci=?), where the
;;; reference picks one by comparing the equivalence with each of them by
;;; eq?, which a standard procedure did not pass here until 2026-09-27
;;; (docs/archive/r7rs-prelude-procedures-lose-identity.md); the default
;;; stays, being right for all five.  For any other equivalence, pass the
;;; hash function that agrees with it.
;;; docs/upcoming/r7rs-srfi-plan.md, S4.
(define-library (srfi 69)
  (export
    make-hash-table hash-table? alist->hash-table
    hash-table-equivalence-function hash-table-hash-function
    hash-table-ref hash-table-ref/default hash-table-set! hash-table-delete!
    hash-table-exists? hash-table-update! hash-table-update!/default
    hash-table-size hash-table-keys hash-table-values hash-table-walk
    hash-table-fold hash-table->alist hash-table-copy hash-table-merge!
    hash string-hash string-ci-hash hash-by-identity)
  (import (scheme base))
  (begin

    ;; ---- hash functions ------------------------------------------------

    (define (bounded h bound)
      (if (null? bound) h (modulo h (car bound))))
    (define (hash obj . bound) (bounded (r7rs-equal-hash__ obj) bound))
    (define (string-hash s . bound) (bounded (r7rs-string-hash__ s) bound))
    (define (string-ci-hash s . bound) (bounded (r7rs-string-ci-hash__ s) bound))
    (define (hash-by-identity obj . bound) (bounded (r7rs-eqv-hash__ obj) bound))
    (define (default-hash key)
      (if (string? key) (r7rs-string-ci-hash__ key) (r7rs-equal-hash__ key)))

    ;; ---- the table -----------------------------------------------------

    (define-record-type <hash-table>
      (make-table equiv hashf buckets size)
      hash-table?
      (equiv hash-table-equivalence-function)
      (hashf hash-table-hash-function)
      (buckets table-buckets set-table-buckets!)
      (size hash-table-size set-table-size!))

    (define (make-hash-table . args)
      (let ((equiv (if (pair? args) (car args) equal?))
            (hashf (if (and (pair? args) (pair? (cdr args))) (cadr args) default-hash)))
        (make-table equiv hashf (make-vector 16 '()) 0)))

    (define (bucket-index table key buckets)
      (modulo ((hash-table-hash-function table) key) (vector-length buckets)))

    ;; The (key . value) pair for key in one bucket, or #f.
    (define (bucket-find equiv bucket key)
      (cond ((null? bucket) #f)
            ((equiv (caar bucket) key) (car bucket))
            (else (bucket-find equiv (cdr bucket) key))))

    (define (entry table key)
      (let ((buckets (table-buckets table)))
        (bucket-find (hash-table-equivalence-function table)
                     (vector-ref buckets (bucket-index table key buckets))
                     key)))

    ;; Twice as many buckets, the entries moved over as they are.
    (define (grow! table)
      (let* ((old (table-buckets table))
             (n (* 2 (vector-length old)))
             (new (make-vector n '()))
             (hashf (hash-table-hash-function table)))
        (do ((i 0 (+ i 1))) ((= i (vector-length old)))
          (let move ((bucket (vector-ref old i)))
            (if (pair? bucket)
                (let ((j (modulo (hashf (caar bucket)) n)))
                  (vector-set! new j (cons (car bucket) (vector-ref new j)))
                  (move (cdr bucket))))))
        (set-table-buckets! table new)))

    (define (hash-table-set! table key value)
      (let* ((buckets (table-buckets table))
             (i (bucket-index table key buckets))
             (found (bucket-find (hash-table-equivalence-function table)
                                 (vector-ref buckets i) key)))
        (if found
            (set-cdr! found value)
            (begin
              (vector-set! buckets i (cons (cons key value) (vector-ref buckets i)))
              (set-table-size! table (+ (hash-table-size table) 1))
              (if (> (hash-table-size table) (* 2 (vector-length buckets)))
                  (grow! table))))))

    (define (hash-table-ref table key . rest)
      (let ((found (entry table key)))
        (cond ((not found)
               (if (pair? rest)
                   ((car rest))
                   (error "hash-table-ref: no value is associated with the key" key)))
              ((and (pair? rest) (pair? (cdr rest))) ((cadr rest) (cdr found)))
              (else (cdr found)))))

    (define (hash-table-ref/default table key default)
      (let ((found (entry table key)))
        (if found (cdr found) default)))

    (define (hash-table-exists? table key)
      (if (entry table key) #t #f))

    (define (hash-table-delete! table key)
      (let* ((buckets (table-buckets table))
             (i (bucket-index table key buckets))
             (equiv (hash-table-equivalence-function table)))
        (let remove ((bucket (vector-ref buckets i)) (kept '()))
          (cond ((null? bucket) #f)
                ((equiv (caar bucket) key)
                 (vector-set! buckets i (append-reverse kept (cdr bucket)))
                 (set-table-size! table (- (hash-table-size table) 1)))
                (else (remove (cdr bucket) (cons (car bucket) kept)))))))

    (define (append-reverse rev tail)
      (if (null? rev) tail (append-reverse (cdr rev) (cons (car rev) tail))))

    (define (hash-table-update! table key proc . rest)
      (let ((found (entry table key)))
        (if found
            (set-cdr! found (proc (cdr found)))
            (hash-table-set! table key
                             (proc (if (pair? rest)
                                       ((car rest))
                                       (error "hash-table-update!: no value is associated with the key" key)))))))

    (define (hash-table-update!/default table key proc default)
      (let ((found (entry table key)))
        (if found
            (set-cdr! found (proc (cdr found)))
            (hash-table-set! table key (proc default)))))

    ;; ---- the whole table -----------------------------------------------

    (define (hash-table-fold table kons knil)
      (let ((buckets (table-buckets table)))
        (let loop ((i 0) (acc knil))
          (if (= i (vector-length buckets))
              acc
              (loop (+ i 1)
                    (let inner ((bucket (vector-ref buckets i)) (acc acc))
                      (if (null? bucket)
                          acc
                          (inner (cdr bucket) (kons (caar bucket) (cdar bucket) acc)))))))))

    (define (hash-table-walk table proc)
      (hash-table-fold table (lambda (k v acc) (proc k v) acc) #f)
      (if #f #f))

    (define (hash-table-keys table)
      (hash-table-fold table (lambda (k v acc) (cons k acc)) '()))

    (define (hash-table-values table)
      (hash-table-fold table (lambda (k v acc) (cons v acc)) '()))

    (define (hash-table->alist table)
      (hash-table-fold table (lambda (k v acc) (cons (cons k v) acc)) '()))

    ;; A fresh table with the same associations; the optional `mutable?`
    ;; argument is accepted, and every table here is mutable.
    (define (hash-table-copy table . mutable)
      (let* ((old (table-buckets table))
             (new (make-vector (vector-length old) '())))
        (do ((i 0 (+ i 1))) ((= i (vector-length old)))
          (vector-set! new i (map (lambda (e) (cons (car e) (cdr e))) (vector-ref old i))))
        (make-table (hash-table-equivalence-function table)
                    (hash-table-hash-function table)
                    new
                    (hash-table-size table))))

    (define (hash-table-merge! table1 table2)
      (hash-table-walk table2 (lambda (k v) (hash-table-set! table1 k v)))
      table1)

    ;; The first association for a key wins, as SRFI 69 says.
    (define (alist->hash-table alist . args)
      (let ((table (apply make-hash-table args)))
        (for-each (lambda (e) (hash-table-update!/default table (car e) (lambda (x) x) (cdr e)))
                  alist)
        table))))
