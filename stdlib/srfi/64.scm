;;; srfi/64 -- SRFI 64, A Scheme API for test suites.
;;;
;;; Taylan Kammer's R7RS implementation, from the SRFI's repository
;;; (contrib/taylan.kammer/), under its MIT licence (stdlib/srfi/COPYING).
;;; The reference splits it into four libraries; here they are one, their
;;; bodies in order.  Changes for Turmeric are marked "Turmeric:":
;;;   - test-runner-factory and test-runner-current are globals behind
;;;     procedures, not parameter objects set by calling them;
;;;   - the default runner writes no log file, and its outermost test-end
;;;     exits with status 1 after a failure or an unexpected pass
;;;     (r7rs-srfi-plan S6);
;;;   - test-error takes #t, a predicate or an SRFI 35 condition type as
;;;     the error type (an R7RS error object has the condition types &error
;;;     and &message);
;;;   - test-read-eval-string is syntax, so that only a program using it
;;;     links (scheme eval);
;;;   - no source locations: this lowering has no syntax-case.
;;; A failing primitive (`(vector-ref v 99)`, `(car 5)`) raises an error
;;; object, which test-error catches
;;; (docs/archive/r7rs-type-errors-are-uncatchable-panics.md).
;;; docs/archive/r7rs-srfi-plan.md, S6.
;;;
;;; SPDX-FileCopyrightText: 2015 Taylan Kammer <taylan.kammer@gmail.com>
;;; SPDX-License-Identifier: MIT
(define-library (srfi 64)
  (export
    ;; Execution
    test-begin test-end test-group test-group-with-cleanup

    test-skip test-expect-fail
    test-match-name test-match-nth
    test-match-all test-match-any

    test-assert test-eqv test-eq test-equal test-approximate
    test-error test-read-eval-string

    test-apply test-with-runner

    test-exit

    ;; Test runner
    test-runner-null test-runner? test-runner-reset

    test-result-alist test-result-alist!
    test-result-ref test-result-set!
    test-result-remove test-result-clear

    test-runner-pass-count
    test-runner-fail-count
    test-runner-xpass-count
    test-runner-xfail-count
    test-runner-skip-count

    test-runner-test-name

    test-runner-group-path
    test-runner-group-stack

    test-runner-aux-value test-runner-aux-value!

    test-result-kind test-passed?

    test-runner-on-test-begin test-runner-on-test-begin!
    test-runner-on-test-end test-runner-on-test-end!
    test-runner-on-group-begin test-runner-on-group-begin!
    test-runner-on-group-end test-runner-on-group-end!
    test-runner-on-final test-runner-on-final!
    test-runner-on-bad-count test-runner-on-bad-count!
    test-runner-on-bad-end-name test-runner-on-bad-end-name!

    test-runner-factory test-runner-create
    test-runner-current test-runner-get

    ;; Simple test runner
    test-runner-simple
    test-on-group-begin-simple test-on-group-end-simple test-on-final-simple
    test-on-test-begin-simple test-on-test-end-simple
    test-on-bad-count-simple test-on-bad-end-name-simple
    )
  (import (scheme base) (scheme case-lambda) (scheme complex) (scheme write)
          (scheme process-context) (srfi 35) (srfi 48))
  (begin

    ;;; ---- test-runner.body.scm ----


    ;;; The data type

    (define-record-type <test-runner>
      (make-test-runner) test-runner?

      (result-alist test-result-alist test-result-alist!)

      (pass-count test-runner-pass-count test-runner-pass-count!)
      (fail-count test-runner-fail-count test-runner-fail-count!)
      (xpass-count test-runner-xpass-count test-runner-xpass-count!)
      (xfail-count test-runner-xfail-count test-runner-xfail-count!)
      (skip-count test-runner-skip-count test-runner-skip-count!)
      (total-count %test-runner-total-count %test-runner-total-count!)

      ;; Stack (list) of (count-at-start . expected-count):
      (count-list %test-runner-count-list %test-runner-count-list!)

      ;; Normally #f, except when in a test-apply.
      (run-list %test-runner-run-list %test-runner-run-list!)

      (skip-list %test-runner-skip-list %test-runner-skip-list!)
      (fail-list %test-runner-fail-list %test-runner-fail-list!)

      (skip-save %test-runner-skip-save %test-runner-skip-save!)
      (fail-save %test-runner-fail-save %test-runner-fail-save!)

      (group-stack test-runner-group-stack test-runner-group-stack!)

      ;; Note: on-test-begin and on-test-end are unrelated to the test-begin and
      ;; test-end forms in the execution library.  They're called at the
      ;; beginning/end of each individual test, whereas the test-begin and test-end
      ;; forms demarcate test groups.

      (on-group-begin test-runner-on-group-begin test-runner-on-group-begin!)
      (on-test-begin test-runner-on-test-begin test-runner-on-test-begin!)
      (on-test-end test-runner-on-test-end test-runner-on-test-end!)
      (on-group-end test-runner-on-group-end test-runner-on-group-end!)
      (on-final test-runner-on-final test-runner-on-final!)
      (on-bad-count test-runner-on-bad-count test-runner-on-bad-count!)
      (on-bad-end-name test-runner-on-bad-end-name test-runner-on-bad-end-name!)

      (on-bad-error-type %test-runner-on-bad-error-type
                         %test-runner-on-bad-error-type!)

      (aux-value test-runner-aux-value test-runner-aux-value!))

    (define (test-runner-group-path runner)
      (reverse (test-runner-group-stack runner)))

    (define (test-runner-reset runner)
      (test-result-alist! runner '())
      (test-runner-pass-count! runner 0)
      (test-runner-fail-count! runner 0)
      (test-runner-xpass-count! runner 0)
      (test-runner-xfail-count! runner 0)
      (test-runner-skip-count! runner 0)
      (%test-runner-total-count! runner 0)
      (%test-runner-count-list! runner '())
      (%test-runner-run-list! runner #f)
      (%test-runner-skip-list! runner '())
      (%test-runner-fail-list! runner '())
      (%test-runner-skip-save! runner '())
      (%test-runner-fail-save! runner '())
      (test-runner-group-stack! runner '()))

    (define (test-runner-null)
      (define (test-null-callback . args) #f)
      (let ((runner (make-test-runner)))
        (test-runner-reset runner)
        (test-runner-on-group-begin! runner test-null-callback)
        (test-runner-on-group-end! runner test-null-callback)
        (test-runner-on-final! runner test-null-callback)
        (test-runner-on-test-begin! runner test-null-callback)
        (test-runner-on-test-end! runner test-null-callback)
        (test-runner-on-bad-count! runner test-null-callback)
        (test-runner-on-bad-end-name! runner test-null-callback)
        (%test-runner-on-bad-error-type! runner test-null-callback)
        runner))


    ;;; State

    (define test-result-ref
      (case-lambda
        ((runner key)
         (test-result-ref runner key #f))
        ((runner key default)
         (let ((entry (assq key (test-result-alist runner))))
           (if entry (cdr entry) default)))))

    (define (test-result-set! runner key value)
      (let* ((alist (test-result-alist runner))
             (entry (assq key alist)))
        (if entry
            (set-cdr! entry value)
            (test-result-alist! runner (cons (cons key value) alist)))))

    (define (test-result-remove runner key)
      (test-result-alist! runner (let loop ((alist (test-result-alist runner)))
                                   (cond ((null? alist) '())
                                         ((eq? key (caar alist)) (loop (cdr alist)))
                                         (else (cons (car alist) (loop (cdr alist))))))))

    (define (test-result-clear runner)
      (test-result-alist! runner '()))

    (define (test-runner-test-name runner)
      (or (test-result-ref runner 'name) ""))

    (define test-result-kind
      (case-lambda
        (() (test-result-kind (test-runner-get)))
        ((runner) (test-result-ref runner 'result-kind))))

    (define test-passed?
      (case-lambda
        (() (test-passed? (test-runner-get)))
        ((runner) (memq (test-result-kind runner) '(pass xpass)))))


    ;;; Factory and current instance

    ;; Turmeric: a global each, not a parameter object -- the reference sets them
    ;; by calling the parameter with the new value, which an R7RS parameter does
    ;; not do.  SRFI 64 allows a global.  The factory defaults to
    ;; test-runner-simple (the reference's top-level `when` sets that).
    (define %factory #f)
    (define test-runner-factory
      (case-lambda
        (() (or %factory test-runner-simple))
        ((factory) (set! %factory factory))))

    (define (test-runner-create) ((test-runner-factory)))

    (define %current #f)
    (define test-runner-current
      (case-lambda
        (() %current)
        ((runner) (set! %current runner))))

    (define (test-runner-get)
      (or (test-runner-current)
          (error "test-runner not initialized - test-begin missing?")))

    ;;; test-runner.scm ends here

    ;;; ---- test-runner-simple.body.scm ----

    ;;; Helpers

    (define (string-join strings delimiter)
      (if (null? strings)
          ""
          (let loop ((result (car strings))
                     (rest (cdr strings)))
            (if (null? rest)
                result
                (loop (string-append result delimiter (car rest))
                      (cdr rest))))))

    (define (truncate-string string length)
      (define (newline->space c) (if (char=? #\newline c) #\space c))
      (let* ((string (string-map newline->space string))
             (fill "...")
             (fill-len (string-length fill))
             (string-len (string-length string)))
        (if (<= string-len (+ length fill-len))
            string
            (let-values (((q r) (floor/ length 4)))
              ;; Left part gets 3/4 plus the remainder.
              (let ((left-end (+ (* q 3) r))
                    (right-start (- string-len q)))
                (string-append (substring string 0 left-end)
                               fill
                               (substring string right-start string-len)))))))

    (define (print runner format-string . args)
      (apply format #t format-string args))

    ;;; Main

    (define (test-runner-simple)
      (let ((runner (test-runner-null)))
        (test-runner-reset runner)
        (test-runner-on-group-begin!     runner test-on-group-begin-simple)
        (test-runner-on-group-end!       runner test-on-group-end-simple)
        (test-runner-on-final!           runner test-on-final-simple)
        (test-runner-on-test-begin!      runner test-on-test-begin-simple)
        (test-runner-on-test-end!        runner test-on-test-end-simple)
        (test-runner-on-bad-count!       runner test-on-bad-count-simple)
        (test-runner-on-bad-end-name!    runner test-on-bad-end-name-simple)
        (%test-runner-on-bad-error-type! runner on-bad-error-type)
        runner))

    (define (test-on-group-begin-simple runner name count)
      (when (null? (test-runner-group-stack runner))
        (print runner "Test suite begin: ~a~%" name)))

    (define (test-on-group-end-simple runner)
      (let ((name (car (test-runner-group-stack runner))))
        (when (= 1 (length (test-runner-group-stack runner)))
          (print runner "Test suite end: ~a~%" name))))

    (define (test-on-final-simple runner)
      (print runner "Passes:            ~a\n" (test-runner-pass-count runner))
      (print runner "Expected failures: ~a\n" (test-runner-xfail-count runner))
      (print runner "Failures:          ~a\n" (test-runner-fail-count runner))
      (print runner "Unexpected passes: ~a\n" (test-runner-xpass-count runner))
      (print runner "Skipped tests:     ~a~%" (test-runner-skip-count runner)))

    (define (test-on-test-begin-simple runner)
      (values))

    (define (test-on-test-end-simple runner)
      (let* ((result-kind (test-result-kind runner))
             (result-kind-name (case result-kind
                                 ((pass) "PASS") ((fail) "FAIL")
                                 ((xpass) "XPASS") ((xfail) "XFAIL")
                                 ((skip) "SKIP")))
             (name (let ((name (test-runner-test-name runner)))
                     (if (string=? "" name)
                         (truncate-string
                          (format #f "~a" (test-result-ref runner 'source-form))
                          30)
                         name)))
             (label (string-join (append (test-runner-group-path runner)
                                         (list name))
                                 ": ")))
        (print runner "[~a] ~a~%" result-kind-name label)
        (when (memq result-kind '(fail xpass))
          ;; Turmeric: `nil` could not be bound in a #lang r7rs program
          ;; (docs/archive/r7rs-turmeric-syntax-leaks.md; it can now), so `missing`.
          (let ((missing (cons #f #f)))
            (define (found? value)
              (not (eq? missing value)))
            (define (maybe-print value message)
              (when (found? value)
                (print runner message value)))
            (let ((file (test-result-ref runner 'source-file "(unknown file)"))
                  (line (test-result-ref runner 'source-line "(unknown line)"))
                  (expression (test-result-ref runner 'source-form))
                  (expected-value (test-result-ref runner 'expected-value missing))
                  (actual-value (test-result-ref runner 'actual-value missing))
                  (expected-error (test-result-ref runner 'expected-error missing))
                  (actual-error (test-result-ref runner 'actual-error missing)))
              ;; Turmeric: no source locations (see source-info), so the
              ;; form alone rather than "(unknown file):(unknown line):".
              (if (test-result-ref runner 'source-file #f)
                  (print runner "~a:~a: ~s~%" file line expression)
                  (print runner "~s~%" expression))
              (maybe-print expected-value "Expected value: ~s~%")
              (maybe-print expected-error "Expected error: ~a~%")
              (when (or (found? expected-value) (found? expected-error))
                (maybe-print actual-value "Returned value: ~s~%"))
              (maybe-print actual-error "Raised error: ~a~%")
              (newline))))))

    (define (test-on-bad-count-simple runner count expected-count)
      (print runner "*** Total number of tests was ~a but should be ~a. ***~%"
              count expected-count)
      (print runner
             "*** Discrepancy indicates testsuite error or exceptions. ***~%"))

    (define (test-on-bad-end-name-simple runner begin-name end-name)
      (error (format #f "Test-end \"~a\" does not match test-begin \"~a\"."
                     end-name begin-name)))

    (define (on-bad-error-type runner type error)
      (print runner "WARNING: unknown error type predicate: ~a~%" type)
      (print runner "         error was: ~a~%" error))

    ;;; test-runner-simple.scm ends here

    ;;; ---- source-info.body.scm ----

    ;;; Source information: none here -- the reference reads it from
    ;;; syntax-case where an implementation has one, and this lowering has only
    ;;; syntax-rules, so a failing test prints its form without a file and line.
    (define-syntax source-info
      (syntax-rules ()
        ((_ <x>)
         #f)))

    (define (set-source-info! runner source-info)
      (when source-info
        (test-result-set! runner 'source-file (car source-info))
        (test-result-set! runner 'source-line (cdr source-info))))

    ;;; ---- execution.body.scm ----

    ;;; Note: to prevent producing massive amounts of code from the macro-expand
    ;;; phase (which makes compile times suffer and may hit code size limits in some
    ;;; systems), keep macro bodies minimal by delegating work to procedures.


    ;;; Grouping


    ;; Turmeric: the runner test-begin installs comes from the factory, as SRFI 64
    ;; says (the reference made a simple runner and gave it a log file; this one
    ;; writes no file).  After the outermost test-end reports, it exits with
    ;; status 1 when a test failed or passed unexpectedly, so `tur test` and a
    ;; shell see a failing suite (r7rs-srfi-plan S6); a clean suite returns and
    ;; the program goes on.
    (define (maybe-install-default-runner suite-name)
      (when (not (test-runner-current))
        (let* ((runner (test-runner-create))
               (on-final (test-runner-on-final runner)))
          (test-runner-on-final! runner
                                 (lambda (r)
                                   (on-final r)
                                   (if (or (> (test-runner-fail-count r) 0)
                                           (> (test-runner-xpass-count r) 0))
                                       (exit 1))))
          (test-runner-current runner))))

    (define test-begin
      (case-lambda
        ((name)
         (test-begin name #f))
        ((name count)
         (maybe-install-default-runner name)
         (let ((r (test-runner-current)))
           (let ((skip-list (%test-runner-skip-list r))
                 (skip-save (%test-runner-skip-save r))
                 (fail-list (%test-runner-fail-list r))
                 (fail-save (%test-runner-fail-save r))
                 (total-count (%test-runner-total-count r))
                 (count-list (%test-runner-count-list r))
                 (group-stack (test-runner-group-stack r)))
             ((test-runner-on-group-begin r) r name count)
             (%test-runner-skip-save! r (cons skip-list skip-save))
             (%test-runner-fail-save! r (cons fail-list fail-save))
             (%test-runner-count-list! r (cons (cons total-count count)
                                               count-list))
             (test-runner-group-stack! r (cons name group-stack)))))))

    (define test-end
      (case-lambda
        (()
         (test-end #f))
        ((name)
         (let* ((r (test-runner-get))
                (groups (test-runner-group-stack r)))
           (test-result-clear r)
           (when (null? groups)
             (error "test-end not in a group"))
           (when (and name (not (equal? name (car groups))))
             ((test-runner-on-bad-end-name r) r name (car groups)))
           (let* ((count-list (%test-runner-count-list r))
                  (expected-count (cdar count-list))
                  (saved-count (caar count-list))
                  (group-count (- (%test-runner-total-count r) saved-count)))
             (when (and expected-count
                        (not (= expected-count group-count)))
               ((test-runner-on-bad-count r) r group-count expected-count))
             ((test-runner-on-group-end r) r)
             (test-runner-group-stack! r (cdr (test-runner-group-stack r)))
             (%test-runner-skip-list! r (car (%test-runner-skip-save r)))
             (%test-runner-skip-save! r (cdr (%test-runner-skip-save r)))
             (%test-runner-fail-list! r (car (%test-runner-fail-save r)))
             (%test-runner-fail-save! r (cdr (%test-runner-fail-save r)))
             (%test-runner-count-list! r (cdr count-list))
             (when (null? (test-runner-group-stack r))
               ((test-runner-on-final r) r)))))))

    (define-syntax test-group
      (syntax-rules ()
        ((_ <name> <body> . <body>*)
         (%test-group <name> (lambda () <body> . <body>*)))))

    (define (%test-group name thunk)
      (begin
        (maybe-install-default-runner name)
        (let ((runner (test-runner-get)))
          (test-result-clear runner)
          (test-result-set! runner 'name name)
          (unless (test-skip? runner)
            (dynamic-wind
              (lambda () (test-begin name))
              thunk
              (lambda () (test-end name)))))))

    (define-syntax test-group-with-cleanup
      (syntax-rules ()
        ((_ <name> <body> <body>* ... <cleanup>)
         (test-group <name>
           (dynamic-wind (lambda () #f)
                         (lambda () <body> <body>* ...)
                         (lambda () <cleanup>))))))


    ;;; Skipping, expected-failing, matching

    (define (test-skip . specs)
      (let ((runner (test-runner-get)))
        (%test-runner-skip-list!
         runner (cons (apply test-match-all specs)
                      (%test-runner-skip-list runner)))))

    (define (test-skip? runner)
      (let ((run-list (%test-runner-run-list runner))
            (skip-list (%test-runner-skip-list runner)))
        (or (and run-list (not (any-pred run-list runner)))
            (any-pred skip-list runner))))

    (define (test-expect-fail . specs)
      (let ((runner (test-runner-get)))
        (%test-runner-fail-list!
         runner (cons (apply test-match-all specs)
                      (%test-runner-fail-list runner)))))

    (define (test-match-any . specs)
      (let ((preds (map make-pred specs)))
        (lambda (runner)
          (any-pred preds runner))))

    (define (test-match-all . specs)
      (let ((preds (map make-pred specs)))
        (lambda (runner)
          (every-pred preds runner))))

    (define (make-pred spec)
      (cond
       ((procedure? spec)
        spec)
       ((integer? spec)
        (test-match-nth 1 spec))
       ((string? spec)
        (test-match-name spec))
       (else
        (error "not a valid test specifier" spec))))

    (define test-match-nth
      (case-lambda
        ((n) (test-match-nth n 1))
        ((n count)
         (let ((i 0))
           (lambda (runner)
             (set! i (+ i 1))
             (and (>= i n) (< i (+ n count))))))))

    (define (test-match-name name)
      (lambda (runner)
        (equal? name (test-runner-test-name runner))))

    ;;; Beware: all predicates must be called because they might have side-effects;
    ;;; no early returning or and/or short-circuiting of procedure calls allowed.

    (define (any-pred preds object)
      (let loop ((matched? #f)
                 (preds preds))
        (if (null? preds)
            matched?
            (let ((result ((car preds) object)))
              (loop (or matched? result)
                    (cdr preds))))))

    (define (every-pred preds object)
      (let loop ((failed? #f)
                 (preds preds))
        (if (null? preds)
            (not failed?)
            (let ((result ((car preds) object)))
              (loop (or failed? (not result))
                    (cdr preds))))))

    ;;; Actual testing

    (define-syntax false-if-error
      (syntax-rules ()
        ((_ <expression> <runner>)
         (guard (err
                 (else
                  (test-result-set! <runner> 'actual-error err)
                  #f))
           <expression>))))

    (define (test-prelude source-info runner name form)
      (test-result-clear runner)
      (set-source-info! runner source-info)
      (when name
        (test-result-set! runner 'name name))
      (test-result-set! runner 'source-form form)
      (let ((skip? (test-skip? runner)))
        (if skip?
            (test-result-set! runner 'result-kind 'skip)
            (let ((fail-list (%test-runner-fail-list runner)))
              (when (any-pred fail-list runner)
                ;; For later inspection only.
                (test-result-set! runner 'result-kind 'xfail))))
        ((test-runner-on-test-begin runner) runner)
        (not skip?)))

    (define (test-postlude runner)
      (let ((result-kind (test-result-kind runner)))
        (case result-kind
          ((pass)
           (test-runner-pass-count! runner (+ 1 (test-runner-pass-count runner))))
          ((fail)
           (test-runner-fail-count! runner (+ 1 (test-runner-fail-count runner))))
          ((xpass)
           (test-runner-xpass-count! runner (+ 1 (test-runner-xpass-count runner))))
          ((xfail)
           (test-runner-xfail-count! runner (+ 1 (test-runner-xfail-count runner))))
          ((skip)
           (test-runner-skip-count! runner (+ 1 (test-runner-skip-count runner)))))
        (%test-runner-total-count! runner (+ 1 (%test-runner-total-count runner)))
        ((test-runner-on-test-end runner) runner)))

    (define (set-result-kind! runner pass?)
      (test-result-set! runner 'result-kind
                        (if (eq? (test-result-kind runner) 'xfail)
                            (if pass? 'xpass 'xfail)
                            (if pass? 'pass 'fail))))

    ;;; We need to use some trickery to get the source info right.  The important
    ;;; thing is to pass a syntax object that is a pair to `source-info', and make
    ;;; sure this syntax object comes from user code and not from ourselves.

    (define-syntax test-assert
      (syntax-rules ()
        ((_ . <rest>)
         (test-assert/source-info (source-info <rest>) . <rest>))))

    (define-syntax test-assert/source-info
      (syntax-rules ()
        ((_ <source-info> <expr>)
         (test-assert/source-info <source-info> #f <expr>))
        ((_ <source-info> <name> <expr>)
         (%test-assert <source-info> <name> '<expr> (lambda () <expr>)))))

    (define (%test-assert source-info name form thunk)
      (let ((runner (test-runner-get)))
        (when (test-prelude source-info runner name form)
          (let ((val (false-if-error (thunk) runner)))
            (test-result-set! runner 'actual-value val)
            (set-result-kind! runner val)))
        (test-postlude runner)))

    (define-syntax test-compare
      (syntax-rules ()
        ((_ . <rest>)
         (test-compare/source-info (source-info <rest>) . <rest>))))

    (define-syntax test-compare/source-info
      (syntax-rules ()
        ((_ <source-info> <compare> <expected> <expr>)
         (test-compare/source-info <source-info> <compare> #f <expected> <expr>))
        ((_ <source-info> <compare> <name> <expected> <expr>)
         (%test-compare <source-info> <compare> <name> <expected> '<expr>
                        (lambda () <expr>)))))

    (define (%test-compare source-info compare name expected form thunk)
      (let ((runner (test-runner-get)))
        (when (test-prelude source-info runner name form)
          (test-result-set! runner 'expected-value expected)
          (let ((pass? (false-if-error
                        (let ((val (thunk)))
                          (test-result-set! runner 'actual-value val)
                          (compare expected val))
                        runner)))
            (set-result-kind! runner pass?)))
        (test-postlude runner)))

    (define-syntax test-equal
      (syntax-rules ()
        ((_ . <rest>)
         (test-compare/source-info (source-info <rest>) equal? . <rest>))))

    (define-syntax test-eqv
      (syntax-rules ()
        ((_ . <rest>)
         (test-compare/source-info (source-info <rest>) eqv? . <rest>))))

    (define-syntax test-eq
      (syntax-rules ()
        ((_ . <rest>)
         (test-compare/source-info (source-info <rest>) eq? . <rest>))))

    (define (approx= margin)
      (lambda (value expected)
        (let ((rval (real-part value))
              (ival (imag-part value))
              (rexp (real-part expected))
              (iexp (imag-part expected)))
          (and (>= rval (- rexp margin))
               (>= ival (- iexp margin))
               (<= rval (+ rexp margin))
               (<= ival (+ iexp margin))))))

    (define-syntax test-approximate
      (syntax-rules ()
        ((_ . <rest>)
         (test-approximate/source-info (source-info <rest>) . <rest>))))

    (define-syntax test-approximate/source-info
      (syntax-rules ()
        ((_ <source-info> <expected> <expr> <error-margin>)
         (test-approximate/source-info
          <source-info> #f <expected> <expr> <error-margin>))
        ((_ <source-info> <name> <expected> <expr> <error-margin>)
         (test-compare/source-info
          <source-info> (approx= <error-margin>) <name> <expected> <expr>))))

    ;; Turmeric: an error type is #t, a predicate or an SRFI 35 condition
    ;; type (r7rs-srfi-plan S7).
    (define (error-matches? err type)
      (cond
       ((eq? type #t)
        #t)
       ((procedure? type)
        (type err))
       ((condition-type? type)
        (and (condition? err) (condition-has-type? err type)))
       (else
        (let ((runner (test-runner-get)))
          ((%test-runner-on-bad-error-type runner) runner type err))
        #f)))

    (define-syntax test-error
      (syntax-rules ()
        ((_ . <rest>)
         (test-error/source-info (source-info <rest>) . <rest>))))

    (define-syntax test-error/source-info
      (syntax-rules ()
        ((_ <source-info> <expr>)
         (test-error/source-info <source-info> #f #t <expr>))
        ((_ <source-info> <error-type> <expr>)
         (test-error/source-info <source-info> #f <error-type> <expr>))
        ((_ <source-info> <name> <error-type> <expr>)
         (%test-error <source-info> <name> <error-type> '<expr>
                      (lambda () <expr>)))))

    (define (%test-error source-info name error-type form thunk)
      (let ((runner (test-runner-get)))
        (when (test-prelude source-info runner name form)
          (test-result-set! runner 'expected-error error-type)
          (let ((pass? (guard (err (else (test-result-set!
                                          runner 'actual-error err)
                                         (error-matches? err error-type)))
                         (let ((val (thunk)))
                           (test-result-set! runner 'actual-value val))
                         #f)))
            (set-result-kind! runner pass?)))
        (test-postlude runner)))

    ;; Turmeric: syntax, not a procedure.  `eval` and `environment` are spelled
    ;; where it is used, so only a program that calls it needs (scheme eval),
    ;; which links the interpreter into a compiled program (r7rs-guide, Eval);
    ;; every other SRFI 64 program stays small.  The reader comes the same way:
    ;; r7rs-read is (scheme read)'s procedure, which (scheme eval) loads, so an
    ;; unused import of this library does not carry the reader either.  The
    ;; default environment is (scheme base)'s.
    (define-syntax test-read-eval-string
      (syntax-rules ()
        ((_ string)
         (%test-read-eval-string string r7rs-read
                                 (lambda (form) (eval form (environment '(scheme base))))))
        ((_ string env)
         (%test-read-eval-string string r7rs-read (lambda (form) (eval form env))))))

    (define (%test-read-eval-string string read evaluate)
      (let* ((port (open-input-string string))
             (form (read port)))
        (if (eof-object? (read-char port))
            (evaluate form)
            (error "(not at eof)"))))


    ;;; Test runner control flow

    (define-syntax test-with-runner
      (syntax-rules ()
        ((_ <runner> <body> . <body>*)
         (let ((saved-runner (test-runner-current)))
           (dynamic-wind
             (lambda () (test-runner-current <runner>))
             (lambda () <body> . <body>*)
             (lambda () (test-runner-current saved-runner)))))))

    ;; (srfi 1)'s last and drop-right, which are all the reference used it for.
    (define (last lst) (if (null? (cdr lst)) (car lst) (last (cdr lst))))
    (define (drop-right lst n)
      (let loop ((lst lst) (len (length lst)))
        (if (<= len n) '() (cons (car lst) (loop (cdr lst) (- len 1))))))

    (define (test-apply first . rest)
      (let ((runner (if (test-runner? first)
                        first
                        (or (test-runner-current) (test-runner-create))))
            (run-list (if (test-runner? first)
                          (drop-right rest 1)
                          (cons first (drop-right rest 1))))
            (proc (last rest)))
        (test-with-runner runner
          (let ((saved-run-list (%test-runner-run-list runner)))
            (%test-runner-run-list! runner run-list)
            (proc)
            (%test-runner-run-list! runner saved-run-list)))))


    ;;; Indicate success/failure via exit status

    (define (test-exit)
      (let ((runner (test-runner-current)))
        (if (and (zero? (test-runner-xpass-count runner))
                 (zero? (test-runner-fail-count runner)))
            (exit 0)
            (exit 1))))

    ;;; execution.scm ends here
    ))
