#ifndef TUR_READER_H
#define TUR_READER_H

#include "arena.h"
#include "forms.h"
#include "symbols.h"
#include "diag.h"

/* Reads all top-level forms from src into a list of Form*.
 * Returns a NULL-terminated array (newly arena-allocated), with *out_count set.
 * On error, emits diagnostics and returns NULL. */
Form **read_all(Arena *arena, SymbolTable *st, const SourceFile *file,
                uint32_t *out_count);

/* Variant of read_all that uses a caller-owned reader-macro registry. If
 * `registry` is non-NULL, top-level `(reader-macros/define ...)` forms
 * register into it (mutating across calls — used by the REPL/eval driver
 * so a macro defined on one turn is visible on the next). If NULL, the
 * function falls back to per-call registry semantics, identical to
 * `read_all`.
 *
 * Forward-declared `ReaderMacroRegistry` to keep this header lean; the
 * full type lives in compiler/reader_macros.h. */
struct ReaderMacroRegistry;
Form **read_all_with_registry(Arena *arena, SymbolTable *st,
                              const SourceFile *file,
                              struct ReaderMacroRegistry *registry,
                              uint32_t *out_count);

/* TR2 (turi-incremental-elaboration-design): read only the tail of `file`
 * starting at `start_offset` (with `start_line` as the initial line number),
 * producing Forms whose spans are absolute into `file->src`. Lets a long-lived
 * interpreter session parse just the newly appended source while still handing
 * diagnostics the full accumulated blob -- removing the O(N^2) re-parse.
 *
 * `read_all_with_registry` is this with (0, 1); all compiler callers use that,
 * so their behavior is unchanged. NOT valid with a non-zero offset under
 * READER_SWEET (the t-expr preprocessor rewrites the whole buffer, so an
 * original-coordinate offset is meaningless after transformation). */
Form **read_all_with_registry_from(Arena *arena, SymbolTable *st,
                                   const SourceFile *file,
                                   struct ReaderMacroRegistry *registry,
                                   uint32_t start_offset, uint32_t start_line,
                                   uint32_t *out_count);

/* RM4: Load a reader-macro definition file and register every
 * `(reader-macros/define ...)` directive in it into `registry`. Used
 * both by the in-source `#use-reader-macros "path"` directive and by
 * the spice-manifest `:reader-macros [...]` preloader. `abs_path` must
 * be absolute or otherwise resolvable from the current working
 * directory. Returns 0 on success, -1 on failure (diagnostic emitted). */
int reader_macros_load_file(Arena *arena, SymbolTable *st,
                            const char *abs_path,
                            struct ReaderMacroRegistry *registry);

#endif
