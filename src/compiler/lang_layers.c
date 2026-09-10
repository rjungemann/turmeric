/* lang_layers.c -- the curated `#lang` layer registry.
 *
 * The LANG_LAYERS[] table below is the single source of truth for which
 * space-separated `#lang` trailing tokens are legal layers.  See
 * lang_layers.h for the model, docs/archive/lang-layers-plan.md for the
 * design, and the "`#lang` Layers -- curated only" section of CLAUDE.md for
 * the anti-proliferation process.
 *
 * To add a layer: append one row with every field populated and (for a
 * reader layer) a `reader_hook`, or (for a semantic layer) an `experiment`
 * naming an existing EXPERIMENTS[] row.  Prefer NOT adding a layer -- a
 * one-off syntax convenience belongs in a `#use-reader-macros` file. */
#include "lang_layers.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "forms.h"
#include "reader_macros.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"

/* ------------------------------------------------------------------------- *
 * Reader-layer hooks.
 * ------------------------------------------------------------------------- */

/* `stringed` => `#s"text"` reads as `(string/from-cstr "text")`, a fresh
 * owned String (see stdlib/string-reader.tur, owned-string-type-plan).  This
 * is the same macro that `#use-reader-macros "stdlib/string-reader.tur"`
 * installs -- built-in and curated here rather than file-loaded.
 *
 * Idempotent: a persistent REPL / --interpret registry may run this hook on
 * every eval, and the batch-compile registry is `strict` (a second register
 * of the same (name, delim) is a hard error), so we register only when `#s"`
 * is absent.  The template lives in `arena`, which for every caller outlives
 * the registry it is stored in (the compile arena for a batch build; a
 * pooled eval arena freed only at env teardown for the interpreter). */
static void stringed_reader_hook(struct ReaderMacroRegistry *reg,
                                 Arena *arena, SymbolTable *st) {
    StrSlice name = strslice("s", 1);
    if (reader_macros_lookup(reg, name, '"')) return;  /* already installed */

    /* Build the expansion template `(string/from-cstr $body)`.  `$body` is
     * replaced by a string literal of the `#s"..."` body at dispatch time
     * (reader.c::expand_raw_template). */
    Form **items = (Form **)arena_alloc(arena, sizeof(Form *) * 2);
    items[0] = form_sym(arena, SPAN_UNKNOWN,
                        symtab_intern(st, strslice("string/from-cstr", 16)));
    items[1] = form_sym(arena, SPAN_UNKNOWN,
                        symtab_intern(st, strslice("$body", 5)));
    Form *tmpl = form_list(arena, SPAN_UNKNOWN, items, 2);

    reader_macros_register(reg, name, '"', RM_BODY_STRING, tmpl, SPAN_UNKNOWN);
}

/* ------------------------------------------------------------------------- *
 * The registry.
 * ------------------------------------------------------------------------- */

static const LangLayerDescriptor LANG_LAYERS[] = {
    { "stringed",
      LAYER_READER,
      stringed_reader_hook,
      NULL,                        /* reader layer: no experiment */
      "#s\"...\" owned-String literal (string/from-cstr)",
      "v1" },
    /* RT0 refined GRADUATED 2026-08-01 -- static refinement discharge is
     * unconditional, so the layer row is deleted rather than left to
     * accumulate, and the token moves to GRADUATED_LAYERS[] below. */
};

/* ------------------------------------------------------------------------- *
 * Graduated layers.
 *
 * A `#lang` layer that graduates is DELETED from LANG_LAYERS[] -- CLAUDE.md is
 * explicit that layers graduate to always-on rather than accumulating.  But a
 * deleted row makes every file that still names the token fail outright with
 * TUR-E0330, and that is a harsher landing than the same graduation gives a
 * CLI flag: `--enable=<graduated>` is accepted as a no-op with TUR-W0063,
 * because `GRADUATED[]` in experiments.c exists for exactly that.
 *
 * This is the layer-side equivalent.  A name listed here is accepted and
 * ignored, with a one-time notice, so a file carrying `#lang turmeric <name>`
 * keeps compiling across the graduation boundary.  Entries age out one minor
 * line after graduation, matching the experiment convention -- the shim is a
 * migration window, not a permanent alias.
 *
 * The shim landed empty ahead of use, deliberately: it has to be present
 * BEFORE or WITH the first row deletion, because adding it afterwards would
 * mean shipping one release in which every `#lang turmeric <name>` file
 * breaks.  Its mechanism was verified with a temporary entry at that time -- a
 * graduated token warned once and compiled (exit 0) on both the compiled and
 * the interpreter path, a genuinely unknown token still reported TUR-E0330,
 * and a live layer was unaffected.  Testing it that way rather than at
 * graduation is the point -- an empty list exercises nothing, and graduation
 * is the worst moment to find out the shim does not work.
 *
 * `stringed` is the only remaining live layer and is not graduating.  See
 * docs/archive/refined-graduation-plan.md.
 *
 * EMPTY AGAIN since 2026-08-22: `refined` (graduated 2026-08-01, v0.33.0) aged
 * out at 0.38.0, five minor lines later, so `#lang turmeric refined` is once
 * more the hard TUR-E0330 an unknown layer token gets (pinned by
 * errors/lang-layer-retired-name).  It went in the same change that retired
 * `"refined"` from GRADUATED[] in experiments.c: the layer shim and the
 * experiment shim were always a pair -- a semantic layer IS its experiment,
 * scoped to one file -- so keeping one without the other would have left
 * `#lang turmeric refined` accepted while `--enable=refined` was refused, which
 * is exactly the inconsistency the pairing exists to prevent.
 *
 * The table stays here, empty, for the reason recorded above: it has to be
 * present BEFORE the first row deletion of the next layer to graduate.
 * ------------------------------------------------------------------------- */
static const char *const GRADUATED_LAYERS[] = {
    NULL,
};

static bool g_layer_grad_warned = false;

bool lang_layer_is_graduated(const char *name, size_t len) {
    if (!name) return false;
    for (size_t i = 0; GRADUATED_LAYERS[i]; i++) {
        if (strlen(GRADUATED_LAYERS[i]) == len &&
            memcmp(GRADUATED_LAYERS[i], name, len) == 0) {
            if (!g_layer_grad_warned) {
                g_layer_grad_warned = true;
                fprintf(stderr,
                        "warning [TUR-W0064]: #lang layer '%.*s' graduated and "
                        "is now on by default; the token is no longer needed\n",
                        (int)len, name);
            }
            return true;
        }
    }
    return false;
}

size_t lang_layers_count(void) {
    return sizeof(LANG_LAYERS) / sizeof(LANG_LAYERS[0]);
}

const LangLayerDescriptor *lang_layer_at(size_t i) {
    if (i >= lang_layers_count()) return NULL;
    return &LANG_LAYERS[i];
}

long lang_layer_index(const char *name, size_t len) {
    if (!name) return -1;
    size_t n = lang_layers_count();
    for (size_t i = 0; i < n; i++) {
        if (strlen(LANG_LAYERS[i].name) == len &&
            memcmp(LANG_LAYERS[i].name, name, len) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* saffron-lang-plan D9: the LANGUAGE axis's twin of
 * lang_layers_apply_semantic below.
 *
 * A dialect is a semantic choice the same way a semantic layer is, so it gets the
 * same policy rather than a second, parallel enable path: the directive IS the
 * enable for its experiment, scoped to this file at CLI precedence -- unless
 * the project manifest scoped `:experiments` and left it out, which is a
 * deliberate no and therefore a hard error.  Silently compiling the file as
 * Turmeric would run it under semantics it did not ask for, which is the worse
 * failure of the two.
 *
 * Kept here, beside the layer version, so the two policies stay legible as one
 * decision; the reader owns parsing the axis, not applying it. */
/* saffron-lang-plan S1: the base axis, for `tur lang-layers`.
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
     * playground picker and `tur lang-layers --json` consume -- a future gated
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

/* saffron-lang-plan S2: see lang_layers.h for why this is a registry lookup
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

bool lang_layers_apply_semantic(LangLayerSet set, const char *path) {
    if (!set) return true;
    bool ok = true;
    size_t n = lang_layers_count();
    for (size_t i = 0; i < n; i++) {
        if (!lang_layer_is_set(set, (long)i)) continue;
        const LangLayerDescriptor *d = &LANG_LAYERS[i];
        if (d->kind != LAYER_SEMANTIC || !d->experiment) continue;
        if (experiment_is_enabled(d->experiment)) continue;
        /* A project that scoped its own :experiments list and left this one
         * out has said no.  Honour that loudly -- silently ignoring a `#lang`
         * layer would compile the file under different semantics than it
         * asked for. */
        if (g_manifest_experiments_scoped) {
            diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                      "%s requires `#lang` layer '%s', which is disabled by the "
                      "project manifest (add :%s to :experiments in build.tur, "
                      "or drop the layer from the #lang line)",
                      path ? path : "this file", d->name, d->experiment);
            ok = false;
            continue;
        }
        /* Otherwise the layer IS the enable, scoped to this file: exactly
         * --enable=<experiment>, at CLI precedence. */
        experiment_enable(d->experiment, XF_SRC_CLI);
    }
    return ok;
}

void lang_layers_apply_readers(LangLayerSet set,
                               struct ReaderMacroRegistry *reg,
                               Arena *arena, SymbolTable *st) {
    if (!set || !reg) return;
    size_t n = lang_layers_count();
    for (size_t i = 0; i < n; i++) {
        if (!lang_layer_is_set(set, (long)i)) continue;
        const LangLayerDescriptor *d = &LANG_LAYERS[i];
        if (d->kind == LAYER_READER && d->reader_hook) {
            d->reader_hook(reg, arena, st);
        }
    }
}
