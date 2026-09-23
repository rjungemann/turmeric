#ifndef TUR_SCHEME_LOWER_H
#define TUR_SCHEME_LOWER_H

/* scheme_lower.h -- r7rs-lang-plan R2: the Scheme core forms, as a Form ->
 * Form lowering onto Turmeric's own binding and control forms.
 *
 * A `#lang r7rs` file reads (R1) into the same Form model every other dialect
 * uses; what makes it Scheme is what its special forms MEAN.  R2 answers that
 * for the core forms by rewriting them, before elaboration, into the Turmeric
 * forms with the same meaning:
 *
 *   (define (f a . r) body...)   -> (defn f [a & r : any] body'...)
 *   (define x e)                 -> (def [^mut] x [: any] e')
 *   (lambda (a b) body...)       -> (fn [a b] body')
 *   (let ((a i)...) body...)     -> (let [a i'...] body')     (parallel when it matters)
 *   (let loop ((a i)...) body)   -> (letrec [loop (fn [a...] body')] (loop i'...))
 *   (let* / letrec / letrec*)    -> nested let / letrec
 *   (do ((v i s)...) (t r...) c) -> a letrec loop
 *   (begin e...)                 -> (do e'...)      (spliced at top level)
 *   (if c t)                     -> (if c' t' nil)
 *   (cond ...) (case ...)        -> if chains, with `=>` and `else`
 *   (and ...) (or ...)           -> value-returning if chains (Scheme's, not Turmeric's bool ones)
 *   (when ...) (unless ...)      -> if
 *   (case-lambda ...)            -> a variadic fn dispatching on argument count
 *   (define-values / let-values / let*-values) -> temporaries over the Values carrier
 *   internal defines             -> letrec (lambdas) / let (values), R7RS letrec* order
 *
 * Only forms whose source file is LANG_R7RS are touched (the decision is
 * per-file, off the span, exactly like lang_span_is_dynamic); a Turmeric-shaped
 * form inside such a file -- `(defn ...)`, `(let [x 1] ...)` -- passes through
 * with its subforms lowered, so a Scheme file may still reach for a Turmeric
 * form by its Turmeric spelling.  Nothing under `quote` is touched.
 *
 * Two facts about the substrate shape what comes out:
 *
 *   - Turmeric's `set!` needs `^mut` at the binding site and a value of the
 *     binding's type.  A Scheme variable that is ever `set!` is therefore
 *     bound as `^mut name : any` (the whole file is scanned for `set!` targets
 *     first), and everything else keeps Saffron's local inference.
 *   - A stdlib name cannot be redefined at top level, and the R7RS prelude
 *     must define `car`, `list`, `length`, ... which the typed stdlib already
 *     owns on its carrier list.  So the prelude's procedures are spelled
 *     `r7rs-<name>` and this pass renames the Scheme spelling to the prelude
 *     one (scheme_lower_rename_table) wherever it occurs unquoted.  R7's
 *     `(import (scheme base))` maps onto the same table.
 *
 * Runs after `(load ...)` expansion in both the entry program and an imported
 * module, so a loaded or imported Scheme file is lowered too.  It never sees
 * the top-level `main` fold: a lowered Scheme expression at top level is an
 * ordinary statement, and elab_toplevel.c folds those into `main` as it does
 * for any file. */

#include <stdbool.h>
#include <stdint.h>

#include "forms.h"
#include "runtime/arena.h"
#include "symbols.h"

/* Lower the Scheme forms among `forms` (a top-level sequence: a program or a
 * module).  Returns a fresh arena array and its length through `out_n`;
 * forms from non-Scheme files are passed through by pointer.  The length can
 * grow (a top-level `begin` or `define-values` splices) and never shrinks.
 * Diagnostics are emitted for malformed Scheme forms; the offending form is
 * dropped and elaboration continues so that diag_had_error() reports it. */
Form **scheme_lower_program(Arena *a, SymbolTable *st,
                            Form *const *forms, uint32_t n, uint32_t *out_n);

/* True when any form in `forms` belongs to a LANG_R7RS file -- a cheap test a
 * caller can make before paying for the pass. */
bool scheme_lower_needed(Form *const *forms, uint32_t n);

#endif /* TUR_SCHEME_LOWER_H */
