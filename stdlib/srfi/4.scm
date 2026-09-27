;;; srfi/4 -- SRFI 4, Homogeneous numeric vector datatypes.
;;;
;;; As in the SRFI's contributed R7RS library (contrib/cowan/ in its
;;; repository), the u8vector is the bytevector (its procedures are SRFI 66's,
;;; re-exported, so importing both binds each name once), and each of the
;;; other nine is a record holding its type's tag.  Where cowan's record holds
;;; a bytevector read through R6RS's (rnrs bytevectors), this one holds a
;;; Scheme vector of the elements, checked on the way in: an exact integer in
;;; the type's range, or for f32 and f64 any real, stored inexact (an f32
;;; element keeps a double's precision, which the SRFI allows).  An element
;;; out of range raises an error object.  As in Racket, there is no reader
;;; syntax: `#s16(1 2)` does not read, and `#u8(1 2)` is R7RS's own.  Written
;;; for Turmeric from the SRFI's document (MIT, see stdlib/srfi/COPYING).
;;; docs/upcoming/r7rs-srfi-plan.md, S7.
(define-library (srfi 4)
  (export
    make-u8vector make-s8vector make-u16vector make-s16vector make-u32vector
    make-s32vector make-u64vector make-s64vector make-f32vector make-f64vector
    u8vector s8vector u16vector s16vector u32vector
    s32vector u64vector s64vector f32vector f64vector
    u8vector? s8vector? u16vector? s16vector? u32vector?
    s32vector? u64vector? s64vector? f32vector? f64vector?
    u8vector-length s8vector-length u16vector-length s16vector-length
    u32vector-length s32vector-length u64vector-length s64vector-length
    f32vector-length f64vector-length
    u8vector-ref s8vector-ref u16vector-ref s16vector-ref u32vector-ref
    s32vector-ref u64vector-ref s64vector-ref f32vector-ref f64vector-ref
    u8vector-set! s8vector-set! u16vector-set! s16vector-set! u32vector-set!
    s32vector-set! u64vector-set! s64vector-set! f32vector-set! f64vector-set!
    u8vector->list s8vector->list u16vector->list s16vector->list
    u32vector->list s32vector->list u64vector->list s64vector->list
    f32vector->list f64vector->list
    list->u8vector list->s8vector list->u16vector list->s16vector
    list->u32vector list->s32vector list->u64vector list->s64vector
    list->f32vector list->f64vector)
  (import (scheme base) (srfi 66))
  (begin

    ;; s8 through f64: a tag and a Scheme vector of the elements.
    (define-record-type <hvector>
      (make-hvector tag elements)
      hvector?
      (tag hvector-tag)
      (elements hvector-elements))

    ;; The elements of v, which must be a `tag` vector.
    (define (hvector-elements-of who tag v)
      (if (and (hvector? v) (eq? (hvector-tag v) tag))
          (hvector-elements v)
          (error (string-append (symbol->string who) ": expected a vector of type "
                                (symbol->string tag) "vector")
                 v)))

    ;; x as a `tag` element: an exact integer in lo..hi, or for a float
    ;; type (lo #f) a real, made inexact.
    (define (hvector-element who tag lo hi x)
      (cond
       ((not lo)
        (if (real? x)
            (inexact x)
            (error (string-append (symbol->string who) ": not a real") x)))
       ((and (exact-integer? x) (<= lo x hi))
        x)
       (else
        (error (string-append (symbol->string who) ": out of range for "
                              (symbol->string tag) "vector")
               x))))

    ;; A `tag` vector of elements, each checked for `who`.
    (define (hvector-from-list who tag lo hi elements)
      (make-hvector
       tag
       (list->vector
        (map (lambda (x) (hvector-element who tag lo hi x)) elements))))

    (define-syntax define-hvector
      (syntax-rules ()
        ((_ tag lo hi make-v v v? v-length v-ref v-set! v->list list->v)
         (begin
           (define (make-v n . fill)
             (make-hvector
              'tag
              (make-vector n (hvector-element 'make-v 'tag lo hi
                                              (if (pair? fill) (car fill) 0)))))
           (define (list->v elements)
             (hvector-from-list 'list->v 'tag lo hi elements))
           (define (v . elements)
             (hvector-from-list 'v 'tag lo hi elements))
           (define (v? obj)
             (and (hvector? obj) (eq? (hvector-tag obj) 'tag)))
           (define (v-length vec)
             (vector-length (hvector-elements-of 'v-length 'tag vec)))
           (define (v-ref vec i)
             (vector-ref (hvector-elements-of 'v-ref 'tag vec) i))
           (define (v-set! vec i x)
             (vector-set! (hvector-elements-of 'v-set! 'tag vec) i
                          (hvector-element 'v-set! 'tag lo hi x)))
           (define (v->list vec)
             (vector->list (hvector-elements-of 'v->list 'tag vec)))))))

    (define-hvector s8 -128 127
      make-s8vector s8vector s8vector? s8vector-length
      s8vector-ref s8vector-set! s8vector->list list->s8vector)
    (define-hvector u16 0 65535
      make-u16vector u16vector u16vector? u16vector-length
      u16vector-ref u16vector-set! u16vector->list list->u16vector)
    (define-hvector s16 -32768 32767
      make-s16vector s16vector s16vector? s16vector-length
      s16vector-ref s16vector-set! s16vector->list list->s16vector)
    (define-hvector u32 0 4294967295
      make-u32vector u32vector u32vector? u32vector-length
      u32vector-ref u32vector-set! u32vector->list list->u32vector)
    (define-hvector s32 -2147483648 2147483647
      make-s32vector s32vector s32vector? s32vector-length
      s32vector-ref s32vector-set! s32vector->list list->s32vector)
    (define-hvector u64 0 18446744073709551615
      make-u64vector u64vector u64vector? u64vector-length
      u64vector-ref u64vector-set! u64vector->list list->u64vector)
    (define-hvector s64 -9223372036854775808 9223372036854775807
      make-s64vector s64vector s64vector? s64vector-length
      s64vector-ref s64vector-set! s64vector->list list->s64vector)
    (define-hvector f32 #f #f
      make-f32vector f32vector f32vector? f32vector-length
      f32vector-ref f32vector-set! f32vector->list list->f32vector)
    (define-hvector f64 #f #f
      make-f64vector f64vector f64vector? f64vector-length
      f64vector-ref f64vector-set! f64vector->list list->f64vector)))
