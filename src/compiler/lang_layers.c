/* lang_layers.c -- the curated `#lang` layer registry.
 *
 * The LANG_LAYERS[] table below is the single source of truth for which
 * space-separated `#lang` trailing tokens are legal layers.  See
 * lang_layers.h for the model, docs/upcoming/lang-layers-plan.md for the
 * design, and the "`#lang` Layers -- curated only" section of CLAUDE.md for
 * the anti-proliferation process.
 *
 * To add a layer: append one row with every field populated and (for a
 * reader layer) a `reader_hook`, or (for a semantic layer) an `experiment`
 * naming an existing EXPERIMENTS[] row.  Prefer NOT adding a layer -- a
 * one-off syntax convenience belongs in a `#use-reader-macros` file. */
#include "lang_layers.h"

#include <string.h>

#include "forms.h"
#include "reader_macros.h"

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
    /* Semantic layers (LAYER_SEMANTIC + `experiment`) land here once their
     * backing EXPERIMENTS[] row exists.  `refined` (#lang turmeric refined ==
     * --enable=refined scoped to one file) rides the refinement-types work;
     * see docs/upcoming/lang-layers-plan.md phase L4. */
};

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
