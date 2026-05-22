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

/* Build a "file://" URI from an absolute filesystem path.
 * Writes into dest[0..dest_cap-1].  Returns dest. */
char *lsp_path_to_uri(const char *path, char *dest, size_t dest_cap);

#endif
