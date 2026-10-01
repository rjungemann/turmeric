#ifndef TUR_LSP_IO_H
#define TUR_LSP_IO_H

#include <stdbool.h>
#include <stddef.h>

/* Framing limits (security-audit-plan M-3).  A header block longer than
 * LSP_MAX_HEADER_BYTES, or a Content-Length above LSP_MAX_BODY_BYTES, is a
 * framing error.  64 MiB is far above any source file an editor sends in a
 * didOpen, and low enough that a hostile length cannot ask for the heap. */
#define LSP_MAX_HEADER_BYTES ((size_t)8 * 1024)
#define LSP_MAX_BODY_BYTES   ((size_t)64 * 1024 * 1024)

/* Read one JSON-RPC 2.0 message from fd_in (blocks).
 * Returns heap-allocated NUL-terminated body, or NULL on EOF/error.
 * Caller must free(). */
char *lsp_read_message(int fd_in);

/* Parse the Content-Length out of a NUL-terminated header block.  The name
 * matches case-insensitively at the start of a line; the value must be
 * decimal digits only, non-zero, and at most LSP_MAX_BODY_BYTES.  Repeated
 * headers must agree.  Returns false on any violation.  Exposed for the unit
 * test and the fuzz target. */
bool lsp_parse_content_length(const char *headers, size_t *out_len);

/* Write one JSON-RPC 2.0 message to fd_out with Content-Length framing. */
void lsp_write_message(int fd_out, const char *json, size_t len);

#endif
