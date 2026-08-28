#ifndef TUR_LSP_UTIL_H
#define TUR_LSP_UTIL_H

#include <stddef.h>

/* Extract the Turmeric identifier that spans the given (1-based) line and
 * column in text.  Writes a NUL-terminated name into out_name[0..name_cap-1].
 * Returns 1 on success (non-empty word found), 0 otherwise.
 *
 * Turmeric identifier characters: [A-Za-z0-9\-?!*\/><+=_] */
int lsp_word_at_pos(const char *text, size_t text_len,
                    int line_1based, int col_1based,
                    char *out_name, size_t name_cap);

/* Byte offset of a (1-based) line/column position in text.  A position past
 * the end of a line clamps to that line's end; a position past the end of the
 * buffer clamps to text_len.  Never fails, so callers get a usable offset for
 * any position a client can send. */
size_t lsp_offset_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based);

/* Extract the partial identifier ending at the cursor -- the completion
 * prefix.  Unlike lsp_word_at_pos this only ever scans *left*: the word the
 * cursor sits before is not what the user is typing.
 *
 * That difference is load-bearing at the start of a buffer.  lsp_word_at_pos
 * steps right when the cursor is not on an identifier character, so a request
 * at {line:0, character:0} of `(defn add ...)` came back with the prefix
 * "defn" and completion filtered every candidate away -- an empty list for the
 * most obvious request a client can make.  Scanning left yields "", which
 * correctly means "no prefix, offer everything".
 *
 * Writes a NUL-terminated (possibly empty) prefix.  Returns its length. */
size_t lsp_prefix_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based,
                         char *out_prefix, size_t cap);

/* The call being written at the cursor, for signatureHelp.
 *
 * Scans forward from the start of the buffer -- string- and comment-aware --
 * maintaining a stack of open `(` forms, and reports the head symbol of the
 * innermost one plus which argument the cursor sits in.  Forward scanning is
 * what makes quoting work: a `(` inside a string or a `;` comment never opens
 * a frame, which a backward scan cannot tell without re-reading the file
 * anyway.
 *
 * Returns 1 when the cursor is inside a call with a named head, 0 otherwise
 * (top level, or a head that is itself still being typed).
 * *active_param_out receives the 0-based argument index. */
int lsp_enclosing_call(const char *text, size_t text_len,
                       size_t cursor_off,
                       char *out_name, size_t name_cap,
                       int *active_param_out);

/* Build a "file://" URI from an absolute filesystem path.
 * Writes into dest[0..dest_cap-1].  Returns dest. */
char *lsp_path_to_uri(const char *path, char *dest, size_t dest_cap);

/* Reported once per occurrence.  `line0` is 0-based; `col0` is a *byte*
 * offset into that line, matching the utf-8 positionEncoding the server
 * negotiates.  `len` is the match length in bytes. */
typedef void (*LspOccurrenceFn)(int line0, int col0, int len, void *user);

/* Every whole-identifier occurrence of `name` in `text`, skipping the places
 * where a name is not a use of that name.
 *
 * The skipping is the entire value of this over a regular expression, and it
 * is the line c2mp draws too (vite-wasm/src/symbols.js:721): `total` inside
 * `subtotal`, inside a comment, or inside a string literal is not a use of
 * `total`, and a pattern match over the source cannot tell.  Four regions are
 * skipped: `;` line comments, `"..."` string literals (backslash-escaped),
 * `#| ... |#` block comments (nesting), and ```` ```c ... ``` ```` inline-C
 * bodies -- the last because a C identifier that happens to spell a Turmeric
 * one is a different language's variable.
 *
 * Word boundaries use the same identifier character class as
 * lsp_word_at_pos, so a highlight lands on exactly what a hover would. */
void lsp_scan_occurrences(const char *text, size_t text_len,
                          const char *name,
                          LspOccurrenceFn fn, void *user);

#endif
