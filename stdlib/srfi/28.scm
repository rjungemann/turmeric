;;; srfi/28 -- SRFI 28, Basic format strings.
;;;
;;; SRFI 48's format (stdlib/srfi/48.scm), re-exported: SRFI 48 is a
;;; superset of SRFI 28, whose ~a, ~s, ~% and ~~ it handles alike, so both
;;; SRFIs share one engine (r7rs-srfi-plan S6), and importing both binds
;;; format once.  The SRFI 48 directives work here too; SRFI 28 calls them
;;; errors, which leaves an implementation free to accept them.
(define-library (srfi 28)
  (export format)
  (import (srfi 48)))
