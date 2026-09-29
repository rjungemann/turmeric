/* jit_prune -- drop what a JIT'd program never reaches before c2mir sees it.
 *
 * jit-suite-pays-for-the-whole-prelude: every program the emitter writes
 * carries the whole auto-loaded stdlib prelude -- ~380 static functions and
 * ~160 KB of C for `(println 42)` -- plus the system headers that prelude
 * needs (sockets, inet, regex, hamt.h).  The cc path does not pay for that:
 * cc drops an unreferenced static function.  c2mir compiles every definition
 * it is given, so under `tur jit` the prelude and those headers were most of
 * the per-program compile, and most of the JIT fixture suite's wall-clock.
 *
 * jit_prune_split_source rewrites a split TU (the text jit_try_split_preamble
 * produces: [hoisted prefix][committed decls region][program half]) in place:
 *
 *   1. In the program half, a `static` function definition or prototype, a
 *      `static` object, a TUR_FATBOX_DEF, or an `extern` declaration is kept
 *      only when something live names it.  Everything else -- the prefix,
 *      the decls region, and every program-half entity that is not one of
 *      those kinds (macros, typedefs, type and non-static definitions) -- is
 *      a root.
 *      Identifiers are read outside comments and literals.
 *   2. A handful of heavy system includes in the decls region is dropped when
 *      no identifier they declare survives in the program's own text.
 *
 * A reference the scan misses cannot pass silently: c2mir rejects the pruned
 * program, cmd_jit retries the full TU (TUR-W0071), and tests/run-jit.sh fails
 * any fixture that needed that retry.
 *
 * Returns true when it rewrote `src`; false leaves it untouched (no split
 * marker, or TUR_JIT_NO_PRUNE=1). */
#ifndef TUR_JIT_PRUNE_H
#define TUR_JIT_PRUNE_H

#include <stdbool.h>
#include <stddef.h>
#include "buf.h"

typedef struct JitPruneStats {
    size_t bytes_before;
    size_t bytes_after;
    unsigned nodes;          /* removable entities found */
    unsigned nodes_dropped;
    unsigned includes_dropped;
} JitPruneStats;

bool jit_prune_split_source(Buf *src, JitPruneStats *stats);

/* The same program-half pruning for a TU the split declined (a preamble that
 * differs from the committed one: every `#lang r7rs` and `#lang saffron`
 * program, and any build flag that moves the preamble).  Everything up to the
 * end-of-preamble marker is a root and no include is dropped. */
bool jit_prune_full_source(Buf *src, JitPruneStats *stats);

#endif
