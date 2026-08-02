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
 * docs/upcoming/v1/refined-graduation-plan.md.
 * ------------------------------------------------------------------------- */
static const char *const GRADUATED_LAYERS[] = {
    "refined",  /* graduated 2026-08-01; static #refine{...} discharge is unconditional */
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
