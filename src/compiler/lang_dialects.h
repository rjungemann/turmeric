#ifndef TUR_LANG_DIALECTS_H
#define TUR_LANG_DIALECTS_H

/* lang_dialects.h -- the `#lang` BASE axis.
 *
 * `#lang <base>[/<reader>]` selects one mutually-exclusive base dialect: a
 * LANGUAGE (`turmeric` | `saffron` | `r7rs`) and, for a language whose trait
 * row is `reader_axis_free`, an optional slash and a reader (`curly-infix`,
 * `neoteric`, `sweet`).  `r7rs` brings its own reader and takes no slash.
 * That is the whole grammar -- a trailing token after the base name is a
 * hard error (TUR-E0330), and an unknown base is TUR-E0331.
 *
 * There used to be a second, additive axis here: an order-independent SET of
 * `#lang` layer tokens backed by a LANG_LAYERS[] registry, threaded through
 * four compile paths as a bitset.  It was decommissioned -- see
 * docs/archive/lang-layers-decommission-plan.md.  In its whole life it held
 * two rows, never more than one at a time, and both features that would have
 * justified keeping it turned out to belong on other axes.  A one-off syntax
 * convenience belongs in a `#use-reader-macros` file; a semantic gate belongs
 * in EXPERIMENTS[] behind `--enable=`; an always-on `#`-dispatch belongs in
 * reader_macros_install_builtins. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h"            /* LangDialect, ReaderType, Span, SourceFile */
#include "runtime/arena.h"
#include "symbols.h"

/* r7rs-lang-plan R0 / D1: the per-language TRAIT row.
 *
 * Everything the elaborator and the emitter used to ask by testing for
 * LANG_SAFFRON by name was really asking one of these questions -- "is this
 * file dynamically typed?", "which reader does the bare base token mean?",
 * "does this language autoload a prelude?" -- so the answers live in one row
 * per language and the identity test goes away.  A second dynamic language
 * (`#lang r7rs`) then inherits the whole dynamic surface by setting one bit
 * here, instead of growing an `if (lang == LANG_R7RS)` beside every
 * `LANG_SAFFRON` test (the plan's R1 risk: two dynamic substrates). */
typedef struct LangTraits {
    const char *name;             /* "turmeric" | "saffron" | "r7rs" */
    ReaderType  default_reader;   /* what the bare base token selects */
    bool        reader_axis_free; /* may be spelled over the four Turmeric readers */
    bool        dynamic;          /* an unannotated param/return means `any` */
    const char *prelude;          /* stdlib autoload tail (e.g. "saffron/prelude.tur"), or NULL */
    const char *experiment;       /* gating EXPERIMENTS[] row, or NULL when stable (D11) */
} LangTraits;

/* The trait row for a dialect.  Never NULL: an out-of-range value gets the
 * Turmeric row, so an unwired construction site keeps the default behaviour
 * rather than dereferencing garbage. */
const LangTraits *lang_traits(LangDialect d);

/* saffron-lang-plan S2 / r7rs-lang-plan R0: is the file this span belongs to
 * written in a DYNAMICALLY TYPED language (`LangTraits.dynamic`)?
 *
 * This used to be lang_span_is_saffron, and every one of its callers was
 * asking this question, not "is it Saffron specifically" -- the canonical
 * site is elab_fns.c's `? TY_ANY : TY_INT` default.  The dialect lives on the
 * SourceFile, and every Form carries the file_id of the file it was read
 * from, so this is a registry lookup rather than state threaded through
 * elaboration.  That is what makes the answer PER-FILE: a dynamic program
 * that loads a Turmeric module gets Turmeric defaults for that module's
 * forms and dynamic defaults for its own, with no extra work -- the contract
 * boundary of Saffron's D5 falls out of asking the question this way.
 *
 * False for an unknown file_id, so an unregistered or synthetic span keeps
 * today's behaviour. */
bool lang_span_is_dynamic(Span sp);

/* saffron-lang-plan S1: print the `#lang` BASE axis -- the (language, reader)
 * pairs a base token can name -- for `tur dialects`.  Rendered from the two
 * enums rather than tabulated: the legal bases are their cross-product, and a
 * table would have to be kept in step with both. */
void lang_dialects_print(void);
void lang_dialects_print_json(void);

/* saffron-lang-plan S1 / try-turmeric-lang-toggle-plan T1: iterate that same
 * base axis, machine-readably.
 *
 * lang_dialects_print renders the LANG_BASES[] table for the CLI; this is
 * its twin for a second consumer -- the playground's
 * `turi_wasm_lang_registry` -- so that consumer reads the one table instead
 * of keeping its own copy.  A hardcoded copy is precisely what drifted when
 * Saffron landed: the WASM picker went on offering four bases after there
 * were eight, so `#lang saffron` worked when typed but could not be selected.
 *
 * `base` is composed into the caller's struct (the spelling is built, not a
 * static string); the remaining fields point at static storage.  `experiment`
 * is the EXPERIMENTS[] name gating the LANGUAGE half, or NULL when the base is
 * stable -- a caller badges the row with it rather than hiding the row, since
 * the directive is itself the enable (D9).  Returns false past the end. */
typedef struct LangBaseDescriptor {
    char        base[64];    /* token as written, e.g. "saffron/sweet" */
    const char *language;    /* "turmeric" | "saffron" | "r7rs" */
    const char *reader;      /* unqualified reader suffix, e.g. "sweet", or "scheme" */
    const char *experiment;  /* gating EXPERIMENTS[] name, or NULL */
} LangBaseDescriptor;

size_t lang_bases_count(void);
bool   lang_base_at(size_t i, LangBaseDescriptor *out);

/* The base token naming one (language, reader) pair -- the bare language name
 * when the reader is that language's default, else "<language>/<suffix>".
 * The inverse of lang_base_lookup, for a caller that holds the two axes and
 * needs to say which base it is in (e.g. reporting a live session's `#lang`). */
void lang_base_spelling_of(LangDialect d, ReaderType r, char *out, size_t cap);

/* r7rs-lang-plan R1: resolve a `#lang` base token (`name`, `len` bytes, no
 * NUL needed) against LANG_BASES[].  True and both out-params set when the
 * token names a legal pair; false otherwise, leaving them untouched.  The
 * reader's lang_base_from_name is this plus the legacy `sweet-exp` alias,
 * which is accepted on input and deliberately not a table row. */
bool lang_base_lookup(const char *name, size_t len,
                      LangDialect *out_dialect, ReaderType *out_reader);

#endif /* TUR_LANG_DIALECTS_H */
