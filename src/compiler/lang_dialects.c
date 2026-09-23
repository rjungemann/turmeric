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
 * r7rs-lang-plan R1 (D1): the base set is a TABLE again, LANG_BASES[] below.
 * It used to be rendered as the cross-product of {language} x {reader}, on
 * the argument that "the legal bases are exactly their cross-product, and a
 * table would have to be kept in step with both".  That stopped being true
 * the moment a language arrived with its own reader: `r7rs/sweet` is not a
 * thing, and `turmeric/r7rs` is not a thing either.  The table has one row
 * per legal pair, and a language's LangTraits row says whether it spans the
 * four Turmeric readers (`reader_axis_free`) or brings exactly one. */
#include "lang_dialects.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "forms.h"
#include "reader_macros.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"

/* r7rs-lang-plan R0 / D1: one trait row per language, indexed by LangDialect.
 * The order MUST match the enum in diag.h; lang_traits() bounds-checks so a
 * stray value degrades to the Turmeric row rather than past the array. */
static const LangTraits LANG_TRAITS[] = {
    /* LANG_TURMERIC */
    { "turmeric", READER_TURMERIC, /*reader_axis_free=*/true,
      /*dynamic=*/false, /*prelude=*/NULL, /*experiment=*/NULL },
    /* LANG_SAFFRON: the dynamic substrate.  An unannotated parameter or
     * return defaults to `any`; the prelude adapts the typed stdlib. */
    { "saffron",  READER_TURMERIC, /*reader_axis_free=*/true,
      /*dynamic=*/true,  /*prelude=*/"saffron/prelude.tur",
      /*experiment=*/NULL },
    /* LANG_R7RS: Saffron's substrate under a Scheme reader (r7rs-lang-plan
     * thesis, Section 1).  `dynamic` is the whole inheritance: unannotated
     * means `any`, the dynamic operator/call/field/match surface and the
     * `any` type-id machinery all come from that one bit.  The reader is its
     * own and there is no reader axis (D1; `r7rs/sweet` is a deliberate
     * deferral, Section 8 Q5).  The prelude is Saffron's for now -- R1's exit
     * criterion is that a `#lang r7rs` file elaborates exactly as the same
     * file under `#lang saffron`, and the Saffron prelude is what supplies
     * the dynamic adaptors over the typed stdlib; R7 replaces it with
     * `(scheme base)`.  `experiment` is the EXPERIMENTS[] row that gates the
     * dialect; the `#lang` line is itself the enable (D11). */
    { "r7rs",     READER_R7RS,     /*reader_axis_free=*/false,
      /*dynamic=*/true,  /*prelude=*/"saffron/prelude.tur",
      /*experiment=*/"r7rs" },
};

const LangTraits *lang_traits(LangDialect d) {
    size_t i = (size_t)d;
    if (i >= sizeof(LANG_TRAITS) / sizeof(LANG_TRAITS[0])) i = 0;
    return &LANG_TRAITS[i];
}

/* The base set: one row per legal (language, reader) pair, in the order
 * `tur dialects` prints them.  A `reader_axis_free` language contributes one
 * row per Turmeric reader; a language with its own reader contributes one.
 * `sweet-exp` is omitted on purpose -- it is a legacy alias accepted on input
 * and never generated (reader_type_name), so listing it would advertise a
 * spelling new code should not use. */
typedef struct LangBase {
    LangDialect lang;
    ReaderType  reader;
} LangBase;

static const LangBase LANG_BASES[] = {
    { LANG_TURMERIC, READER_TURMERIC    },
    { LANG_TURMERIC, READER_CURLY_INFIX },
    { LANG_TURMERIC, READER_NEOTERIC    },
    { LANG_TURMERIC, READER_SWEET       },
    { LANG_SAFFRON,  READER_TURMERIC    },
    { LANG_SAFFRON,  READER_CURLY_INFIX },
    { LANG_SAFFRON,  READER_NEOTERIC    },
    { LANG_SAFFRON,  READER_SWEET       },
    { LANG_R7RS,     READER_R7RS        },
};

#define N_LANG_BASES (sizeof(LANG_BASES) / sizeof(LANG_BASES[0]))

/* The reader half, unqualified.  reader_type_name returns the fully-qualified
 * "turmeric/<suffix>" for the Turmeric readers, which reads as a
 * contradiction in a Saffron row; the language already has its own column.
 * A language-owned reader (READER_R7RS) has no slash and no Turmeric
 * spelling, so it gets a descriptive word instead. */
static const char *lang_reader_suffix(ReaderType r) {
    if (r == READER_R7RS) return "scheme";
    const char *full = reader_type_name(r);
    const char *slash = strchr(full, '/');
    return slash ? slash + 1 : "s-expr";
}

/* The base token for a (language, reader) pair: the bare language name when
 * the reader is that language's default, else "<language>/<reader-suffix>". */
static void lang_base_spelling(LangDialect d, ReaderType r,
                               char *out, size_t cap) {
    const char *lang = lang_dialect_name(d);
    if (r == lang_traits(d)->default_reader) {
        snprintf(out, cap, "%s", lang);
        return;
    }
    snprintf(out, cap, "%s/%s", lang, lang_reader_suffix(r));
}

void lang_base_spelling_of(LangDialect d, ReaderType r, char *out, size_t cap) {
    lang_base_spelling(d, r, out, cap);
}

bool lang_base_lookup(const char *name, size_t len,
                      LangDialect *out_dialect, ReaderType *out_reader) {
    if (!name) return false;
    for (size_t i = 0; i < N_LANG_BASES; i++) {
        char base[64];
        lang_base_spelling(LANG_BASES[i].lang, LANG_BASES[i].reader,
                           base, sizeof base);
        if (strlen(base) == len && memcmp(base, name, len) == 0) {
            if (out_dialect) *out_dialect = LANG_BASES[i].lang;
            if (out_reader)  *out_reader  = LANG_BASES[i].reader;
            return true;
        }
    }
    return false;
}

size_t lang_bases_count(void) {
    return N_LANG_BASES;
}

/* The STATUS a row prints and the badge a picker shows: the gating
 * EXPERIMENTS[] name when the LANGUAGE half is gated, else NULL.  A gated
 * base is badged, never hidden -- the `#lang` line is itself the enable, so
 * the row stays selectable (D11). */
static const char *lang_base_experiment(LangDialect d) {
    return lang_traits(d)->experiment;
}

bool lang_base_at(size_t i, LangBaseDescriptor *out) {
    if (!out || i >= N_LANG_BASES) return false;
    LangDialect d = LANG_BASES[i].lang;
    ReaderType  r = LANG_BASES[i].reader;
    lang_base_spelling(d, r, out->base, sizeof out->base);
    out->language   = lang_dialect_name(d);
    out->reader     = lang_reader_suffix(r);
    out->experiment = lang_base_experiment(d);
    return true;
}

void lang_dialects_print(void) {
    printf("%-22s %-9s %-12s %s\n", "BASE", "LANGUAGE", "READER", "STATUS");
    for (size_t i = 0; i < N_LANG_BASES; i++) {
        LangBaseDescriptor d;
        if (!lang_base_at(i, &d)) continue;
        /* A gated language says so in the column that was kept for it.  The
         * name of the row is what `tur experiments` lists and what the
         * lifecycle warning (TUR-W0060/W0061) cites. */
        char status[80];
        if (d.experiment)
            snprintf(status, sizeof status, "experimental (%s)", d.experiment);
        else
            snprintf(status, sizeof status, "stable");
        printf("%-22s %-9s %-12s %s\n", d.base, d.language, d.reader, status);
    }
}

void lang_dialects_print_json(void) {
    printf("[");
    for (size_t i = 0; i < N_LANG_BASES; i++) {
        LangBaseDescriptor d;
        if (!lang_base_at(i, &d)) continue;
        if (i) printf(",");
        printf("\n    {\"base\":\"%s\",\"language\":\"%s\",\"reader\":\"%s\"",
               d.base, d.language, d.reader);
        /* The `"experiment"` key is present only on a gated base, so a
         * consumer that keys on its presence sees exactly the badged rows. */
        if (d.experiment) printf(",\"experiment\":\"%s\"", d.experiment);
        printf("}");
    }
    printf("\n  ]");
}

/* saffron-lang-plan S2 / r7rs-lang-plan R0: see lang_dialects.h for why this
 * is a registry lookup rather than threaded state, and why it asks the trait
 * rather than the dialect's name. */
bool lang_span_is_dynamic(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    return f != NULL && lang_traits(f->lang)->dynamic;
}

/* saffron GRADUATED at 0.46.0: a non-default dialect is no longer gated, warns
 * nothing, and cannot be switched off by a manifest.  What remains is the one
 * side effect the gate used to carry incidentally -- flipping
 * `g_opt_dynamic_any`, which the emitter reads to decide whether this build
 * emits the `any` type and instance registries and the dynamic-dispatch panic
 * (emit_module.c).  Setting it HERE, at the moment a dynamic-language file is
 * read, is exactly when `experiment_enable` used to set it, so the emitted C
 * is unchanged on both arms: a build with no dynamic TU still emits none of
 * it.  Keyed on the trait, not the dialect's identity (r7rs-lang-plan D2):
 * any language whose row says `dynamic` needs the registries.
 *
 * r7rs-lang-plan D11: a language whose trait row names an EXPERIMENTS[] row
 * is gated, and the `#lang` line is itself the enable -- a user who wrote
 * `#lang r7rs` has opted in, and requiring `--enable=r7rs` as well is
 * ceremony.  Enabled at CLI precedence, so a manifest cannot silently refuse
 * a directive the file itself carries; the lifecycle warning
 * (TUR-W0060/W0061) then fires once per compile from here, which is the
 * dialect's elaboration entry point as far as the registry is concerned.
 *
 * Returns bool, and every caller still checks it, because that is the shape a
 * future gated dialect needs; today no dialect can fail. */
bool lang_dialect_apply(LangDialect d, const char *path) {
    (void)path;
    const LangTraits *t = lang_traits(d);
    if (t->experiment) {
        (void)experiment_enable(t->experiment, XF_SRC_CLI);
        experiment_warn_if_used(t->experiment);
    }
    if (t->dynamic) g_opt_dynamic_any = true;
    return true;
}
