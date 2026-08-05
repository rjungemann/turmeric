#ifndef TUR_LANG_LAYERS_H
#define TUR_LANG_LAYERS_H

/* lang_layers.h -- the curated `#lang` layer registry.
 *
 * `#lang <base>[/<dialect>] <layer>*` selects one mutually-exclusive base
 * reader (the slash-namespaced name, handled by detect_lang -> ReaderType)
 * plus an order-independent *set* of additive layers (the space-separated
 * trailing tokens).  This file owns that set.
 *
 * The LANG_LAYERS[] table in lang_layers.c is the single source of truth: a
 * `#lang` layer token is legal only if it has a row there.  Two kinds:
 *
 *   - LAYER_READER   -- flips on a `#`-dispatch (e.g. `stringed` => #s"...").
 *                       Additive and commutative with every other reader
 *                       layer; its `reader_hook` registers the dispatch at
 *                       reader init, before the first form.
 *   - LAYER_SEMANTIC -- flips on an elaboration/checker gate.  It MUST point
 *                       at an existing EXPERIMENTS[] row (`experiment`); the
 *                       experiment carries the lifecycle + expires_at.  No
 *                       parallel enable path.
 *
 * See docs/upcoming/lang-layers-plan.md and the "`#lang` Layers -- curated
 * only" section in CLAUDE.md. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h"            /* LangLayerSet, SourceFile */
#include "runtime/arena.h"
#include "symbols.h"

struct ReaderMacroRegistry;

/* A reader layer's activation hook.  Called once at reader init with the
 * registry the read loop dispatches against; it registers the layer's
 * `#`-dispatch macro (idempotently -- a persistent REPL/interp registry may
 * see the same hook on every eval).  `arena`/`st` build the expansion
 * template. */
typedef void (*LangReaderHook)(struct ReaderMacroRegistry *reg,
                               Arena *arena, SymbolTable *st);

typedef enum LangLayerKind {
    LAYER_READER,    /* additive `#`-dispatch, activated by reader_hook */
    LAYER_SEMANTIC,  /* elaboration/checker gate, bridged to `experiment` */
} LangLayerKind;

typedef struct LangLayerDescriptor {
    const char     *name;         /* token as written in `#lang` */
    LangLayerKind   kind;
    LangReaderHook  reader_hook;  /* LAYER_READER: registers the dispatch */
    const char     *experiment;   /* LAYER_SEMANTIC: EXPERIMENTS[] name */
    const char     *summary;      /* one line, for listings/docs */
    const char     *since;        /* version introduced */
} LangLayerDescriptor;

/* Table iteration (for `tur lang-layers` and docs). */
size_t                     lang_layers_count(void);
const LangLayerDescriptor *lang_layer_at(size_t i);

/* Look up a layer token by name+len.  Returns its index (0-based) or -1 if
 * the token is not a registered layer. */
long lang_layer_index(const char *name, size_t len);

/* True when `name` is a layer that GRADUATED -- deleted from LANG_LAYERS[]
 * because its behaviour became unconditional.  The token is then accepted and
 * ignored rather than reported as unknown (TUR-E0330), so a file that opted in
 * per-file keeps compiling across the boundary.  Warns once, TUR-W0064.
 * Mirrors GRADUATED[] in experiments.c, which does this for `--enable`. */
bool lang_layer_is_graduated(const char *name, size_t len);

/* Fold a single layer (by index) into `set`.  The bit position is the
 * table index, so a set stays valid only against this build's table. */
static inline LangLayerSet lang_layer_add(LangLayerSet set, long idx) {
    return (idx >= 0 && idx < 32) ? (set | ((LangLayerSet)1u << idx)) : set;
}

static inline bool lang_layer_is_set(LangLayerSet set, long idx) {
    return idx >= 0 && idx < 32 && (set & ((LangLayerSet)1u << idx)) != 0;
}

/* Run every enabled reader layer's activation hook against `reg`.  Called
 * from read_all_with_registry after the registry is set up and before the
 * read loop.  A no-op when `set` is empty. */
void lang_layers_apply_readers(LangLayerSet set,
                               struct ReaderMacroRegistry *reg,
                               Arena *arena, SymbolTable *st);

/* Apply every enabled SEMANTIC layer: turn on its backing experiment (the
 * layer IS the enable, scoped to one file -- there is no parallel enable
 * path).  A project manifest that scoped its own `:experiments` list and left
 * the experiment out makes this a hard error instead; returns false in that
 * case, having emitted the diagnostic.  `path` is used in the message only. */
bool lang_layers_apply_semantic(LangLayerSet set, const char *path);

#endif /* TUR_LANG_LAYERS_H */
