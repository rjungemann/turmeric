#ifndef TUR_EMIT_SPLIT_H
#define TUR_EMIT_SPLIT_H

/* r7rs-programs-compile-slowly: a `#lang r7rs` program built as two C units.
 *
 * Every Scheme program reaches most of the R7RS prelude before it does
 * anything -- the uncaught-error printer alone pulls in `write` and the
 * numeric tower -- so compiling the prelude once and linking it is the only
 * way the build stops costing seconds.  The emitter writes the same
 * elaborated program twice:
 *
 *   the LIBRARY unit  -- the runtime preamble and every auto-loaded stdlib
 *                        definition (the prelude among them), with external
 *                        linkage, and nothing of the program.  It does not
 *                        depend on the program, so its object is cached by a
 *                        hash of its text and compiled once.
 *   the CLIENT unit   -- the program, declaring what it uses from the library
 *                        unit instead of defining it.
 *
 * Runtime state lives in the library unit only.  Stateless helpers (the
 * runtime preamble's `static` functions, typedefs, constructors of ADTs) are
 * written into both and each unit compiles the ones it calls.
 * emit_split_state is the text transform that gets the state right for text
 * the emitter writes verbatim: the runtime preamble, hoisted inline-C and a
 * stdlib file's file-scope C block. */

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"

typedef enum {
    EMIT_SPLIT_NONE = 0,   /* one unit, as always */
    EMIT_SPLIT_LIB,        /* the library unit */
    EMIT_SPLIT_CLIENT,     /* the program unit */
} EmitSplitMode;

/* Rewrite the file-scope state in `src[0..len)` for one side of the split,
 * appending the result to `out`:
 *
 *   LIB     every file-scope variable loses `static`, so this unit defines it
 *           with external linkage.
 *   CLIENT  every file-scope variable becomes an `extern` declaration (its
 *           initializers dropped), every externally-linked function
 *           definition becomes its prototype, and a constructor or destructor
 *           is demoted to an ordinary (unused) function, so the program unit
 *           runs on the library unit's single instance of each and runs its
 *           start-up code once.
 *
 * Passed through untouched in both: `static` functions, typedefs, prototypes,
 * `extern` declarations, read-only (`const`-qualified) data, preprocessor
 * lines, comments, and everything inside a function body.  A function-local
 * `static` is therefore NOT shared -- tests/check-r7rs-prelude-split.sh finds
 * one by comparing the data symbols, local and global, the two objects
 * define. */
void emit_split_state(const char *src, size_t len, EmitSplitMode mode, Buf *out);

/* Every symbol the library unit exports is renamed with this prefix, in both
 * units.  In one unit the runtime's state and the stdlib's functions are
 * `static`; exported, their names are the same ones the runtime archives
 * define -- libturi.a carries the S2 split runtime, with its own
 * `g_tur_any_types`, `tur_scheduler` and the rest -- and the linker would
 * bind the embedded evaluator to the program's copies instead of its own. */
#define EMIT_SPLIT_PREFIX "tur_sl_"

/* Record a name the library unit gives external linkage.  emit_split_state
 * records the ones it exports itself (in EMIT_SPLIT_LIB mode); the emitter
 * records the stdlib definitions. */
void emit_split_note_export(const char *name);
void emit_split_exports_clear(void);

/* Copy `src[0..len)` to `out` with every identifier recorded by
 * emit_split_note_export (or the LIB-mode transform) given EMIT_SPLIT_PREFIX.
 * String and character literals and comments are left as written. */
void emit_split_rename(const char *src, size_t len, Buf *out);

/* r7rs-prelude-library-cold-compile: the library unit as `n` pieces that
 * compile concurrently and link as one object.  Almost all of the unit's
 * compile is optimizing its ~1,200 external functions, so each piece keeps
 * the whole unit's declarations and static helpers and defines only its share
 * of those, balanced by size:
 *
 *   - an external function is defined in one piece and prototyped in the
 *     others;
 *   - a file-scope variable is defined in piece 0 and declared `extern` in
 *     the others -- a `static` one loses `static` and moves to the private
 *     prefix EMIT_SPLIT_PIECE_PREFIX, so nothing the runtime archives define
 *     can capture it; read-only data is copied freely;
 *   - a constructor or destructor runs from one piece (piece 0 for a static
 *     one), demoted to an ordinary function elsewhere;
 *   - a SMALL external function (body at most `dup_max` bytes, no local
 *     `static`, no `__func__`) is also a `static` copy in every other piece,
 *     reached by a function-like macro so that only direct calls use it:
 *     `-O2` inlines it there as it did in one unit, and taking its address
 *     still names the one external definition.
 *
 * Writes pieces[0..n) (each buf_init'd by the caller) and returns 0, or
 * nonzero when the text does not split (then compile it whole). */
#define EMIT_SPLIT_PIECE_PREFIX "tur_sp_"
int emit_split_pieces(const char *src, size_t len, int n, size_t dup_max, Buf *pieces);

#endif /* TUR_EMIT_SPLIT_H */
