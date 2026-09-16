/* lang_dialects.c -- the `#lang` BASE axis: which (language, reader) pairs a
 * `#lang` line can name, and what applying one does.
 *
 * `#lang <base>` selects one mutually-exclusive base dialect and nothing
 * else.  There is no second, additive axis: the `#lang` LAYER registry that
 * used to live in this file (as lang_layers.c) was decommissioned -- see
 * docs/archive/lang-layers-decommission-plan.md.  Its one live member,
 * `stringed`, became an unconditional reader macro
 * (reader_macros_install_builtins); a one-off syntax convenience belongs in a
 * `#use-reader-macros` file, and a semantic gate belongs in EXPERIMENTS[].
 *
 * The base set itself is rendered from the two enums below rather than
 * tabulated, because the legal bases are exactly their cross-product. */
#include "lang_dialects.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "forms.h"
#include "reader_macros.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"

/* saffron-lang-plan S1: the base axis, for `tur dialects`.
 *
 * Rendered from the two enums rather than a table: the legal bases are the
 * cross-product of {language} x {reader}, and a table would have to be kept in
 * step with both.  `sweet-exp` is omitted on purpose -- it is a legacy alias
 * accepted on input and never generated (reader_type_name), so listing it would
 * advertise a spelling new code should not use. */
static const LangDialect DIALECTS[] = { LANG_TURMERIC, LANG_SAFFRON };
static const ReaderType  READERS[]  = { READER_TURMERIC, READER_CURLY_INFIX,
                                        READER_NEOTERIC, READER_SWEET };

/* The base token for a (language, reader) pair: the bare language name when
 * the reader is that language's default, else "<language>/<reader-suffix>". */
static void lang_base_spelling(LangDialect d, ReaderType r,
                               char *out, size_t cap) {
    const char *lang = lang_dialect_name(d);
    if (r == READER_TURMERIC) { snprintf(out, cap, "%s", lang); return; }
    /* reader_type_name is the fully-qualified "turmeric/<suffix>"; take the
     * suffix and re-qualify it under this language. */
    const char *full = reader_type_name(r);
    const char *slash = strchr(full, '/');
    snprintf(out, cap, "%s/%s", lang, slash ? slash + 1 : full);
}

/* The reader half, unqualified.  reader_type_name returns the fully-qualified
 * "turmeric/<suffix>", which reads as a contradiction in a Saffron row; the
 * language already has its own column. */
static const char *lang_reader_suffix(ReaderType r) {
    const char *full = reader_type_name(r);
    const char *slash = strchr(full, '/');
    return slash ? slash + 1 : "s-expr";
}

void lang_base_spelling_of(LangDialect d, ReaderType r, char *out, size_t cap) {
    lang_base_spelling(d, r, out, cap);
}

size_t lang_bases_count(void) {
    return (sizeof(DIALECTS) / sizeof(DIALECTS[0]))
         * (sizeof(READERS)  / sizeof(READERS[0]));
}

bool lang_base_at(size_t i, LangBaseDescriptor *out) {
    if (!out || i >= lang_bases_count()) return false;
    size_t nreaders = sizeof(READERS) / sizeof(READERS[0]);
    LangDialect d = DIALECTS[i / nreaders];
    ReaderType  r = READERS[i % nreaders];
    lang_base_spelling(d, r, out->base, sizeof out->base);
    out->language = lang_dialect_name(d);
    out->reader   = lang_reader_suffix(r);
    /* No dialect is experiment-gated any more: `saffron` graduated at 0.46.0
     * and every base is stable.  The field stays because the SHAPE is what the
     * playground picker and `tur dialects --json` consume -- a future gated
     * dialect fills it in here and is badged rather than hidden, with no
     * consumer change.  NULL means "no badge". */
    out->experiment = NULL;
    return true;
}

void lang_dialects_print(void) {
    printf("%-22s %-9s %-12s %s\n", "BASE", "LANGUAGE", "READER", "STATUS");
    for (size_t di = 0; di < sizeof(DIALECTS) / sizeof(DIALECTS[0]); di++) {
        for (size_t ri = 0; ri < sizeof(READERS) / sizeof(READERS[0]); ri++) {
            char base[64];
            lang_base_spelling(DIALECTS[di], READERS[ri], base, sizeof base);
            /* Every base is stable since saffron graduated at 0.46.0.  The
             * column stays so a future gated dialect has somewhere to say so. */
            const char *status = "stable";
            printf("%-22s %-9s %-12s %s\n", base,
                   lang_dialect_name(DIALECTS[di]),
                   lang_reader_suffix(READERS[ri]), status);
        }
    }
}

void lang_dialects_print_json(void) {
    printf("[");
    bool first = true;
    for (size_t di = 0; di < sizeof(DIALECTS) / sizeof(DIALECTS[0]); di++) {
        for (size_t ri = 0; ri < sizeof(READERS) / sizeof(READERS[0]); ri++) {
            char base[64];
            lang_base_spelling(DIALECTS[di], READERS[ri], base, sizeof base);
            if (!first) printf(",");
            first = false;
            printf("\n    {\"base\":\"%s\",\"language\":\"%s\",\"reader\":\"%s\"",
                   base, lang_dialect_name(DIALECTS[di]),
                   lang_reader_suffix(READERS[ri]));
            /* No `"experiment"` key on any base since saffron graduated at
             * 0.46.0; a future gated dialect adds it back here. */
            printf("}");
        }
    }
    printf("\n  ]");
}

/* saffron-lang-plan S2: see lang_dialects.h for why this is a registry lookup
 * rather than threaded state. */
bool lang_span_is_saffron(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    return f != NULL && f->lang == LANG_SAFFRON;
}

/* saffron GRADUATED at 0.46.0: a non-default dialect is no longer gated, warns
 * nothing, and cannot be switched off by a manifest.  What remains is the one
 * side effect the gate used to carry incidentally -- flipping `g_opt_saffron`,
 * which the emitter reads to decide whether this build emits the `any` type and
 * instance registries and the dynamic-dispatch panic (emit_module.c).  Setting
 * it HERE, at the moment a `#lang saffron` file is read, is exactly when
 * `experiment_enable` used to set it, so the emitted C is unchanged on both
 * arms: a build with no Saffron TU still emits none of it.
 *
 * Returns bool, and every caller still checks it, because that is the shape a
 * future gated dialect needs; today no dialect can fail. */
bool lang_dialect_apply(LangDialect d, const char *path) {
    (void)path;
    if (d == LANG_TURMERIC) return true;           /* the default: nothing to do */
    g_opt_saffron = true;
    return true;
}
