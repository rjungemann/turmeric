;;; srfi/35 -- SRFI 35, Conditions.
;;;
;;; The SRFI's reference implementation, condition types and conditions as
;;; records.  Copyright (C) Richard Kelsey, Michael Sperber (2002); MIT, see
;;; stdlib/srfi/COPYING.  Changes for Turmeric are marked "Turmeric:":
;;;   - the five SRFI 1 procedures it uses are local, over eq? and one list,
;;;     so that importing it (and SRFI 64, which does) does not splice SRFI 1;
;;;   - an R7RS error object is a condition, of types &error and &message,
;;;     its message the object's error-object-message; the reverse does not
;;;     hold, and error-object? stays false for an SRFI 35 condition
;;;     (r7rs-srfi-plan section 7, question 4);
;;;   - condition-subtype? and condition-type-field-supertype are exported
;;;     under neither name, as in the SRFI.
;;; docs/upcoming/r7rs-srfi-plan.md, S7.
(define-library (srfi 35)
  (export
    make-condition-type condition-type?
    make-condition condition? condition-has-type? condition-ref
    make-compound-condition extract-condition
    define-condition-type condition
    &condition
    &message message-condition? condition-message
    &serious serious-condition?
    &error error?)
  (import (scheme base))
  (begin

    ;; Turmeric: SRFI 1's any, find, lset-intersection, lset= and
    ;; lset-difference, for the one-list, eq? cases used below.
    (define (any pred lis)
      (and (pair? lis)
           (or (pred (car lis)) (any pred (cdr lis)))))

    (define (find pred lis)
      (cond ((null? lis) #f)
            ((pred (car lis)) (car lis))
            (else (find pred (cdr lis)))))

    (define (lset-intersection = lis1 lis2)
      (let loop ((lis lis1))
        (cond ((null? lis) '())
              ((memq (car lis) lis2) (cons (car lis) (loop (cdr lis))))
              (else (loop (cdr lis))))))

    (define (lset-difference = lis1 lis2)
      (let loop ((lis lis1))
        (cond ((null? lis) '())
              ((memq (car lis) lis2) (loop (cdr lis)))
              (else (cons (car lis) (loop (cdr lis)))))))

    (define (lset= = lis1 lis2)
      (and (null? (lset-difference = lis1 lis2))
           (null? (lset-difference = lis2 lis1))))

    (define-record-type :condition-type
      (really-make-condition-type name supertype fields all-fields)
      condition-type?
      (name condition-type-name)
      (supertype condition-type-supertype)
      (fields condition-type-fields)
      (all-fields condition-type-all-fields))

    (define (make-condition-type name supertype fields)
      (if (not (symbol? name))
          (error "make-condition-type: name is not a symbol"
                 name))
      (if (not (condition-type? supertype))
          (error "make-condition-type: supertype is not a condition type"
                 supertype))
      (if (not
           (null? (lset-intersection eq?
                                     (condition-type-all-fields supertype)
                                     fields)))
          (error "duplicate field name" ))
      (really-make-condition-type name
                                  supertype
                                  fields
                                  (append (condition-type-all-fields supertype)
                                          fields)))

    (define-syntax define-condition-type
      (syntax-rules ()
        ((define-condition-type ?name ?supertype ?predicate
           (?field1 ?accessor1) ...)
         (begin
           (define ?name
             (make-condition-type '?name
                                  ?supertype
                                  '(?field1 ...)))
           (define (?predicate thing)
             (and (condition? thing)
                  (condition-has-type? thing ?name)))
           (define (?accessor1 condition)
             (condition-ref (extract-condition condition ?name)
                            '?field1))
           ...))))

    (define (condition-subtype? subtype supertype)
      (let recur ((subtype subtype))
        (cond ((not subtype) #f)
              ((eq? subtype supertype) #t)
              (else
               (recur (condition-type-supertype subtype))))))

    (define (condition-type-field-supertype condition-type field)
      (let loop ((condition-type condition-type))
        (cond ((not condition-type) #f)
              ((memq field (condition-type-fields condition-type))
               condition-type)
              (else
               (loop (condition-type-supertype condition-type))))))

    ;; Turmeric: the standard types, moved up from the end of the file so
    ;; that each is defined before condition-type-field-alist names it.
    (define &condition (really-make-condition-type '&condition
                                                   #f
                                                   '()
                                                   '()))

    (define-condition-type &message &condition
      message-condition?
      (message condition-message))

    (define-condition-type &serious &condition
      serious-condition?)

    (define-condition-type &error &serious
      error?)

    ;; The type-field-alist is of the form
    ;; ((<type> (<field-name> . <value>) ...) ...)
    (define-record-type :condition
      (really-make-condition type-field-alist)
      record-condition?
      (type-field-alist record-condition-type-field-alist))

    ;; Turmeric: an R7RS error object reads as the condition
    ;; (condition (&error) (&message (message <its message>))).
    (define (condition? thing)
      (or (record-condition? thing)
          (error-object? thing)))

    (define (condition-type-field-alist condition)
      (if (error-object? condition)
          (list (list &error)
                (list &message
                      (cons 'message (error-object-message condition))))
          (record-condition-type-field-alist condition)))

    (define (make-condition type . field-plist)
      (let ((alist (let label ((plist field-plist))
                     (if (null? plist)
                         '()
                         (cons (cons (car plist)
                                     (cadr plist))
                               (label (cddr plist)))))))
        (if (not (lset= eq?
                        (condition-type-all-fields type)
                        (map car alist)))
            (error "condition fields don't match condition type"))
        (really-make-condition (list (cons type alist)))))

    (define (condition-has-type? condition type)
      (any (lambda (has-type)
             (condition-subtype? has-type type))
           (condition-types condition)))

    (define (condition-ref condition field)
      (type-field-alist-ref (condition-type-field-alist condition)
                            field))

    (define (type-field-alist-ref type-field-alist field)
      (let loop ((type-field-alist type-field-alist))
        (cond ((null? type-field-alist)
               (error "type-field-alist-ref: field not found"
                      type-field-alist field))
              ((assq field (cdr (car type-field-alist)))
               => cdr)
              (else
               (loop (cdr type-field-alist))))))

    (define (make-compound-condition condition-1 . conditions)
      (really-make-condition
       (apply append (map condition-type-field-alist
                          (cons condition-1 conditions)))))

    (define (extract-condition condition type)
      (let ((entry (find (lambda (entry)
                           (condition-subtype? (car entry) type))
                         (condition-type-field-alist condition))))
        (if (not entry)
            (error "extract-condition: invalid condition type"
                   condition type))
        (really-make-condition
         (list (cons type
                     (map (lambda (field)
                            (assq field (cdr entry)))
                          (condition-type-all-fields type)))))))

    (define-syntax condition
      (syntax-rules ()
        ((condition (?type1 (?field1 ?value1) ...) ...)
         (type-field-alist->condition
          (list
           (cons ?type1
                 (list (cons '?field1 ?value1) ...))
           ...)))))

    (define (type-field-alist->condition type-field-alist)
      (really-make-condition
       (map (lambda (entry)
              (cons (car entry)
                    (map (lambda (field)
                           (or (assq field (cdr entry))
                               (cons field
                                     (type-field-alist-ref type-field-alist field))))
                         (condition-type-all-fields (car entry)))))
            type-field-alist)))

    (define (condition-types condition)
      (map car (condition-type-field-alist condition)))

    (define (check-condition-type-field-alist the-type-field-alist)
      (let loop ((type-field-alist the-type-field-alist))
        (if (not (null? type-field-alist))
            (let* ((entry (car type-field-alist))
                   (type (car entry))
                   (field-alist (cdr entry))
                   (fields (map car field-alist))
                   (all-fields (condition-type-all-fields type)))
              (for-each (lambda (missing-field)
                          (let ((supertype
                                 (condition-type-field-supertype type missing-field)))
                            (if (not
                                 (any (lambda (entry)
                                        (let ((type (car entry)))
                                          (condition-subtype? type supertype)))
                                      the-type-field-alist))
                                (error "missing field in condition construction"
                                       type
                                       missing-field))))
                        (lset-difference eq? all-fields fields))
              (loop (cdr type-field-alist))))))))
