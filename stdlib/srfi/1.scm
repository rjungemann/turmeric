;;; srfi/1 -- SRFI 1, List Library.
;;;
;;; chibi-scheme's lib/srfi/1.sld -- its portable `(else ...)` branch and the
;;; nine files it includes, in its order -- as written.  Copyright (c)
;;; 2009-2021 Alex Shinn; BSD-3, see stdlib/srfi/COPYING.
;;;
;;; The names SRFI 1 shares with (scheme base) and (scheme cxr) -- `map`,
;;; `for-each`, `member`, `assoc`, `append`, `list-copy`, `make-list`, the
;;; c[ad]r family and the rest -- are R7RS's own procedures, re-exported:
;;; R7RS adopted SRFI 1's extensions of them (`map` over unequal lengths,
;;; `member` and `assoc` with a comparison), so importing (srfi 1) beside
;;; (scheme base) binds one procedure per name (r7rs-srfi-plan D5).  The
;;; linear-update `!` procedures are the pure ones, which the SRFI allows.
;;; docs/archive/r7rs-srfi-plan.md, S3.
(define-library (srfi 1)
  (export
    xcons cons* make-list list-tabulate list-copy circular-list iota
    proper-list? circular-list? dotted-list? not-pair? null-list? list= first
    second third fourth fifth sixth seventh eighth ninth tenth car+cdr take
    drop take-right drop-right take! drop-right! split-at split-at! last
    last-pair length+ concatenate append! concatenate! reverse! append-reverse
    append-reverse! zip unzip1 unzip2 unzip3 unzip4 unzip5 count fold unfold
    pair-fold reduce fold-right unfold-right pair-fold-right reduce-right
    append-map append-map! map! pair-for-each filter-map map-in-order filter
    partition remove filter! partition! remove! find find-tail any every
    list-index take-while drop-while take-while! span break span! break!
    delete delete-duplicates delete! delete-duplicates! alist-cons alist-copy
    alist-delete alist-delete! lset<= lset= lset-adjoin lset-union lset-union!
    lset-intersection lset-intersection! lset-difference lset-difference!
    lset-xor lset-xor! lset-diff+intersection lset-diff+intersection! cons
    list pair? null? car cdr caar cadr cdar cddr caaar caadr cadar caddr cdaar
    cdadr cddar cdddr caaaar caaadr caadar caaddr cadaar cadadr caddar cadddr
    cdaaar cdaadr cdadar cdaddr cddaar cddadr cdddar cddddr list-ref length
    append reverse map for-each member memq memv assoc assq assv set-car!
    set-cdr!)
  (import (scheme base) (scheme cxr))
  (begin
;; ---- chibi lib/srfi/1.sld, the (else ...) branch's own definitions ----

      (define reverse! reverse)
      (define (find-tail pred ls)
        (and (pair? ls) (if (pred (car ls)) ls (find-tail pred (cdr ls)))))
      (define (find pred ls)
        (cond ((find-tail pred ls) => car) (else #f)))
      (define (any pred ls . lol)
        (define (any1 pred ls)
          (if (pair? (cdr ls))
              ((lambda (x) (if x x (any1 pred (cdr ls)))) (pred (car ls)))
              (pred (car ls))))
        (define (anyn pred lol)
          (if (every pair? lol)
              ((lambda (x) (if x x (anyn pred (map cdr lol))))
               (apply pred (map car lol)))
              #f))
        (if (null? lol)
            (if (pair? ls) (any1 pred ls) #f)
            (anyn pred (cons ls lol))))
      (define (every pred ls . lol)
        (define (every1 pred ls)
          (if (null? (cdr ls))
              (pred (car ls))
              (if (pred (car ls)) (every1 pred (cdr ls)) #f)))
        (if (null? lol)
            (if (pair? ls) (every1 pred ls) #t)
            (not (apply any (lambda xs (not (apply pred xs))) ls lol))))

;; ---- chibi lib/srfi/1/predicates.scm ----
;; predicates.scm -- list prediates
;; Copyright (c) 2009 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (proper-list? x)
  (cond ((null? x) #t)
        ((pair? x) (proper-list? (cdr x)))
        (else #f)))

(define (circular-list? x)
  (and (pair? x) (pair? (cdr x))
       (let race ((hare (cdr x)) (tortoise x))
         (or (eq? hare tortoise)
             (and (pair? hare) (pair? (cdr hare))
                  (race (cddr hare) (cdr tortoise)))))))

(define (dotted-list? x)
  (not (proper-list? x)))

(define (not-pair? x) (not (pair? x)))

(define (null-list? x) (null? x)) ; no error

(define (list= eq . lists)
  (let lp1 ((lists lists))
    (or (null? lists)
        (null? (cdr lists))
        (let lp2 ((ls1 (car lists)) (ls2 (cadr lists)))
          (if (null? ls1)
              (and (null? ls2)
                   (lp1 (cdr lists)))
              (and (pair? ls2)
                   (eq (car ls1) (car ls2))
                   (lp2 (cdr ls1) (cdr ls2))))))))

(define (length+ x)
  (if (not (pair? x))
      0
      (let lp ((hare (cdr x)) (tortoise x) (res 1))
        (and (not (eq? hare tortoise))
             (if (pair? hare)
                 (if (not (pair? (cdr hare)))
                     (+ res 1)
                     (lp (cddr hare) (cdr tortoise) (+ res 2)))
                 res)))))

;; ---- chibi lib/srfi/1/selectors.scm ----
;; selectors.scm -- extended list selectors
;; Copyright (c) 2009 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define first car)
(define second cadr)
(define (third ls)   (car (cdr (cdr ls))))
(define (fourth ls)  (car (cdr (cdr (cdr ls)))))
(define (fifth ls)   (car (cdr (cdr (cdr (cdr ls))))))
(define (sixth ls)   (car (cdr (cdr (cdr (cdr (cdr ls)))))))
(define (seventh ls) (car (cdr (cdr (cdr (cdr (cdr (cdr ls))))))))
(define (eighth ls)  (car (cdr (cdr (cdr (cdr (cdr (cdr (cdr ls)))))))))
(define (ninth ls)   (car (cdr (cdr (cdr (cdr (cdr (cdr (cdr (cdr ls))))))))))
(define (tenth ls)   (car (cdr (cdr (cdr (cdr (cdr (cdr (cdr (cdr (cdr ls)))))))))))

(define (car+cdr x) (values (car x) (cdr x)))

(define (take ls i)
  (let lp ((i i) (ls ls) (res '()))
    (if (<= i 0)
        (reverse! res)
        (lp (- i 1) (cdr ls) (cons (car ls) res)))))

(define (take! ls i)
  (if (<= i 0)
      '()
      (let ((tail (list-tail ls (- i 1))))
        (set-cdr! tail '())
        ls)))

(define (drop ls i)
  (if (<= i 0) ls (drop (cdr ls) (- i 1))))

(define (take-right ls i)
  (drop ls (- (length+ ls) i)))

(define (drop-right ls i)
  (take ls (- (length+ ls) i)))

(define (drop-right! ls i)
  (take! ls (- (length+ ls) i)))

(define (split-at ls i)
  (let lp ((i i) (ls ls) (res '()))
    (if (<= i 0)
        (values (reverse! res) ls)
        (lp (- i 1) (cdr ls) (cons (car ls) res)))))

(define (split-at! ls i)
  (if (<= i 0)
      (values '() ls)
      (let* ((tail (list-tail ls (- i 1)))
             (right (cdr tail)))
        (set-cdr! tail '())
        (values ls right))))

(define (last ls) (if (null? (cdr ls)) (car ls) (last (cdr ls))))
(define (last-pair ls) (if (null? (cdr ls)) ls (last-pair (cdr ls))))

;; ---- chibi lib/srfi/1/search.scm ----
;; search.scm -- list searching and splitting
;; Copyright (c) 2009-2011 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (take-while pred ls)
  (let lp ((ls ls) (res '()))
    (if (and (pair? ls) (pred (car ls)))
        (lp (cdr ls) (cons (car ls) res))
        (reverse! res))))

(define take-while! take-while)

(define (drop-while pred ls)
  (or (find-tail (lambda (x) (not (pred x))) ls) '()))

(define (span pred ls)
  (let lp ((ls ls) (res '()))
    (if (and (pair? ls) (pred (car ls)))
        (lp (cdr ls) (cons (car ls) res))
        (values (reverse! res) ls))))

(define span! span)

(define (break pred ls) (span (lambda (x) (not (pred x))) ls))

(define break! break)

(define (list-index pred ls . lists)
  (if (null? lists)
      (let lp ((ls ls) (n 0))
        (and (pair? ls) (if (pred (car ls)) n (lp (cdr ls) (+ n 1)))))
      (let lp ((lists (cons ls lists)) (n 0))
        (and (every pair? lists)
             (if (apply pred (map car lists)) n (lp (map cdr lists) (+ n 1)))
             ))))

;; ---- chibi lib/srfi/1/misc.scm ----
;; misc.scm -- miscellaneous list utilities
;; Copyright (c) 2009-2012 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (map-onto proc ls init)
  (let lp ((ls (reverse ls)) (res init))
    (if (null? ls) res (lp (cdr ls) (cons (proc (car ls)) res)))))

(define (append! . lists) (concatenate! lists))

(define (concatenate lists)
  (let lp ((ls (reverse lists)) (res '()))
    (if (null? ls) res (lp (cdr ls) (append (car ls) res)))))

(define (concatenate! lists)
  (if (null? lists)
      '()
      (let loop ((acc    '())
                 (prev   '())
                 (rem    lists))
          (cond
            ((null? rem) acc)
            ((null? acc) (let ((cur (car rem))) (loop cur cur (cdr rem))))
            ((null? (car rem)) (loop acc prev (cdr rem)))
            (else (let ((cur (car rem)))
                    (set-cdr! (last-pair prev) cur)
                    (loop acc cur (cdr rem))))))))

(define (append-reverse rev tail)
  (if (null? rev) tail (append-reverse (cdr rev) (cons (car rev) tail))))

(define (append-reverse! rev tail)
  (if (null? rev)
      tail
      (let ((head (reverse! rev)))
        (set-cdr! rev tail)
        head)))

(define (zip . lists) (apply map list lists))

(define (unzip1 ls) (map first ls))
(define (unzip2 ls) (values (map first ls) (map second ls)))
(define (unzip3 ls) (values (map first ls) (map second ls) (map third ls)))
(define (unzip4 ls)
  (values (map first ls) (map second ls) (map third ls) (map fourth ls)))
(define (unzip5 ls)
  (values (map first ls) (map second ls) (map third ls) (map fourth ls)
          (map fifth ls)))

(define (count pred ls . lists)
  (if (null? lists)
      (let lp ((ls ls) (res 0))
        (if (pair? ls) (lp (cdr ls) (if (pred (car ls)) (+ res 1) res)) res))
      (let lp ((lists (cons ls lists)) (res 0))
        (if (every pair? lists)
            (lp (map cdr lists) (if (apply pred (map car lists)) (+ res 1) res))
            res))))

;; ---- chibi lib/srfi/1/constructors.scm ----
;; constructors.scm -- list construction utilities
;; Copyright (c) 2009-2012 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (xcons a b) (cons b a))

(define (cons* x . args)
  (let lp ((rev '()) (x x) (ls args))
    (if (null? ls)
        (append-reverse rev x)
        (lp (cons x rev) (car ls) (cdr ls)))))

(define (list-tabulate n proc)
  (let lp ((n (- n 1)) (res '()))
    (if (< n 0) res (lp (- n 1) (cons (proc n) res)))))

(define (circular-list x . args)
  (let ((res (cons x args)))
    (set-cdr! (last-pair res) res)
    res))

(define (iota count . o)
  (let ((start (if (pair? o) (car o) 0))
        (step (if (and (pair? o) (pair? (cdr o))) (cadr o) 1)))
    (let lp ((i count) (res '()))
      (if (<= i 0)
          res
          (lp (- i 1) (cons (+ start (* (- i 1) step)) res))))))

;; ---- chibi lib/srfi/1/fold.scm ----
;; fold.scm -- list fold/reduce utilities
;; Copyright (c) 2009-2012 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (fold kons knil ls . lists)
  (if (null? lists)
      (let lp ((ls ls) (acc knil))
        (if (pair? ls) (lp (cdr ls) (kons (car ls) acc)) acc))
      (let lp ((lists (cons ls lists)) (acc knil))
        (if (every pair? lists)
            (lp (map cdr lists) (apply kons (map-onto car lists (list acc))))
            acc))))

(define (fold-right kons knil ls . lists)
  (if (null? lists)
      (let lp ((ls ls))
        (if (pair? ls) (kons (car ls) (lp (cdr ls))) knil))
      (let lp ((lists (cons ls lists)))
        (if (every pair? lists)
            (apply kons (map-onto car lists (list (lp (map cdr lists)))))
            knil))))

(define (pair-fold kons knil ls . lists)
  (if (null? lists)
      (let lp ((ls ls) (acc knil))
        (if (pair? ls) (lp (cdr ls) (kons ls acc)) acc))
      (let lp ((lists (cons ls lists)) (acc knil))
        (if (every pair? lists)
            (lp (map cdr lists) (apply kons (append lists (list acc))))
            acc))))

(define (pair-fold-right kons knil ls . lists)
  (if (null? lists)
      (let lp ((ls ls))
        (if (pair? ls) (kons ls (lp (cdr ls))) knil))
      (let lp ((lists (cons ls lists)))
        (if (every pair? lists)
            (apply kons (append lists (list (lp (map cdr lists)))))
            knil))))

(define (reduce f identity ls)
  (if (null? ls) identity (fold f (car ls) (cdr ls))))

(define (reduce-right f identity ls)
  (if (null? ls) identity (fold-right f identity ls)))

(define (unfold p f g seed . o)
  (let lp ((seed seed))
    (if (p seed)
        (if (pair? o) ((car o) seed) '())
        (cons (f seed) (lp (g seed))))))

(define (unfold-right p f g seed . o)
  (let lp ((seed seed) (res (if (pair? o) (car o) '())))
    (if (p seed) res (lp (g seed) (cons (f seed) res)))))

(define (append-map-helper append f ls lists)
  (if (null? lists)
      (if (null? ls)
          '()
          (let lp ((ls (reverse ls)) (res '()))
            (if (null? ls) res (lp (cdr ls) (append (f (car ls)) res)))))
      (if (and (pair? ls) (every pair? lists))
          (let lp ((lists (cons ls lists)))
            (let ((vals (apply f (map car lists)))
                  (cdrs (map cdr lists)))
              (if (every pair? cdrs) (append vals (lp cdrs)) vals)))
          '())))

(define (append-map f ls . lists)
  (append-map-helper append f ls lists))

(define (append-map! f ls . lists)
  (append-map-helper append! f ls lists))

(define map! map)
(define map-in-order map)

(define (pair-for-each f ls . lists)
  (if (pair? lists)
      (apply pair-fold (lambda args (apply f (drop-right args 1))) #f ls lists)
      (pair-fold (lambda (x _) (f x)) #f ls)))

(define (filter-map f ls . lists)
  (if (null? lists)
      (let lp ((ls ls) (res '()))
        (if (pair? ls)
            (let ((x (f (car ls)))) (lp (cdr ls) (if x (cons x res) res)))
            (reverse res)))
      (filter (lambda (x) x) (apply map f ls lists))))

(define (take-up-to-reverse from to init)
  (if (eq? from to)
      init
      (take-up-to-reverse (cdr from) to (cons (car from) init))))

(define (remove pred ls)
  (let lp ((ls ls) (rev '()))
    (let ((tail (find-tail pred ls)))
      (if tail
          (lp (cdr tail) (take-up-to-reverse ls tail rev))
          (if (pair? rev) (append-reverse rev ls) ls)))))

(define (filter pred ls) (remove (lambda (x) (not (pred x))) ls))

(define (partition pred ls)
  (let lp ((ls ls) (good '()) (bad '()))
    (cond ((null? ls) (values (reverse! good) (reverse! bad)))
          ((pred (car ls)) (lp (cdr ls) (cons (car ls) good) bad))
          (else (lp (cdr ls) good (cons (car ls) bad))))))

(define filter! filter)
(define remove! remove)
(define partition! partition)

;; ---- chibi lib/srfi/1/deletion.scm ----
;; deletion.scm -- list deletion utilities
;; Copyright (c) 2009-2012 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (delete x ls . o)
  (let ((eq (if (pair? o) (car o) equal?)))
    (if (eq? eq eq?)
        (let lp ((ls ls) (rev '())) ;; fast path for delq
          (let ((tail (memq x ls)))
            (if tail
                (lp (cdr tail) (take-up-to-reverse ls tail rev))
                (if (pair? rev) (append-reverse! rev ls) ls))))
        (remove (lambda (y) (eq x y)) ls))))

(define delete! delete)

(define (delete-duplicates ls . o)
  (let ((eq (if (pair? o) (car o) equal?)))
    (let lp ((ls ls) (res '()))
      (if (pair? ls)
          (lp (cdr ls) (if (member (car ls) res eq) res (cons (car ls) res)))
          (reverse! res)))))

(define delete-duplicates! delete-duplicates)

;; ---- chibi lib/srfi/1/alists.scm ----
;; alist.scm -- association list utilities
;; Copyright (c) 2009 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (alist-cons key value ls) (cons (cons key value) ls))

(define (alist-copy ls) (map (lambda (x) (cons (car x) (cdr x))) ls))

(define (alist-delete key ls . o)
  (let ((eq (if (pair? o) (car o) equal?)))
    (remove (lambda (x) (eq key (car x))) ls)))

(define alist-delete! alist-delete)

;; ---- chibi lib/srfi/1/lset.scm ----
;; lset.scm -- list set library
;; Copyright (c) 2009-2012 Alex Shinn.  All rights reserved.
;; BSD-style license: http://synthcode.com/license.txt

(define (lset<= eq . sets)
  (if (null? sets)
      #t
      (let lp1 ((set1 (car sets)) (sets (cdr sets)))
        (if (null? sets)
            #t
            (let ((set2 (car sets)))
              (let lp2 ((ls set1))
                (if (pair? ls)
                    (and (member (car ls) set2 eq) (lp2 (cdr ls)))
                    (lp1 set2 (cdr sets)))))))))

(define (lset= eq . sets)
  (and (apply lset<= eq sets)
       (apply lset<= (lambda (b a) (eq a b)) (reverse sets))))

(define (lset-adjoin eq set . elts)
  (lset-union2 eq set elts))

(define (lset-union2 eq a b)
  (if (null? b)
      a
      (lset-union2 eq (if (member (car b) a eq) a (cons (car b) a)) (cdr b))))

(define (lset-union eq . sets)
  (reduce (lambda (a b) (lset-union2 eq b a)) '() sets))

(define (lset-intersection eq . sets)
  (reduce (lambda (a b) (filter (lambda (x) (member x a eq)) b)) '() sets))

(define (lset-diff2 eq a b)
  (remove (lambda (x) (member x a eq)) b))

(define (lset-difference eq . sets)
  (reduce (lambda (a b) (lset-diff2 eq a b)) '() sets))

(define (lset-xor eq . sets)
  (reduce (lambda (a b) (append (lset-diff2 eq a b) (lset-diff2 eq b a)))
          '()
          sets))

(define (lset-diff+intersection eq . sets)
  (values (apply lset-difference eq sets) (apply lset-intersection eq sets)))

(define lset-diff+intersection! lset-diff+intersection)
(define lset-xor! lset-xor)
(define lset-difference! lset-difference)
(define lset-intersection! lset-intersection)
(define lset-union! lset-union)
))
