#ifndef TUR_LANG_DIALECTS_H
#define TUR_LANG_DIALECTS_H

/* lang_dialects.h -- the `#lang` BASE axis.
 *
 * `#lang <base>[/<reader>]` selects one mutually-exclusive base dialect: a
 * LANGUAGE (`turmeric` | `saffron`) and, after an optional slash, a reader
 * (`curly-infix`, `neoteric`, `sweet`).  That is the whole grammar -- a
 * trailing token after the base name is a hard error (TUR-E0330).
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

/* saffron-lang-plan S2: is the file this span belongs to written in Saffron?
 *
 * The dialect lives on the SourceFile, and every Form carries the file_id of
 * the file it was read from, so this is a registry lookup rather than state
 * threaded through elaboration.  That is what makes the answer PER-FILE: a
 * Saffron program that loads a Turmeric module gets Turmeric defaults for that
 * module's forms and Saffron defaults for its own, with no extra work -- the
 * contract boundary of D5 falls out of asking the question this way.
 *
 * False for an unknown file_id, so an unregistered or synthetic span keeps
 * today's behaviour. */
bool lang_span_is_saffron(Span sp);

/* saffron-lang-plan S1: print the `#lang` BASE axis -- the (language, reader)
 * pairs a base token can name -- for `tur dialects`.  Rendered from the two
 * enums rather than tabulated: the legal bases are their cross-product, and a
 * table would have to be kept in step with both. */
void lang_dialects_print(void);
void lang_dialects_print_json(void);

/* saffron-lang-plan S1 / try-turmeric-lang-toggle-plan T1: iterate that same
 * base axis, machine-readably.
 *
 * lang_dialects_print renders the cross-product for the CLI; this is its twin
 * for a second consumer -- the playground's `turi_wasm_lang_registry` -- so
 * that consumer reads the two enums instead of keeping its own copy.  A
 * hardcoded copy is precisely what drifted when Saffron landed: the WASM
 * picker went on offering four bases after there were eight, so `#lang
 * saffron` worked when typed but could not be selected.
 *
 * `base` is composed into the caller's struct (the spelling is built, not a
 * static string); the remaining fields point at static storage.  `experiment`
 * is the EXPERIMENTS[] name gating the LANGUAGE half, or NULL when the base is
 * stable -- a caller badges the row with it rather than hiding the row, since
 * the directive is itself the enable (D9).  Returns false past the end. */
typedef struct LangBaseDescriptor {
    char        base[64];    /* token as written, e.g. "saffron/sweet" */
    const char *language;    /* "turmeric" | "saffron" */
    const char *reader;      /* unqualified reader suffix, e.g. "sweet" */
    const char *experiment;  /* gating EXPERIMENTS[] name, or NULL */
} LangBaseDescriptor;

size_t lang_bases_count(void);
bool   lang_base_at(size_t i, LangBaseDescriptor *out);

/* The base token naming one (language, reader) pair -- the bare language name
 * when the reader is that language's default, else "<language>/<suffix>".
 * The inverse of lang_base_from_name, for a caller that holds the two axes and
 * needs to say which base it is in (e.g. reporting a live session's `#lang`). */
void lang_base_spelling_of(LangDialect d, ReaderType r, char *out, size_t cap);

#endif /* TUR_LANG_DIALECTS_H */
