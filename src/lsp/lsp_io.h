#ifndef TUR_LSP_IO_H
#define TUR_LSP_IO_H

#include <stddef.h>

/* Read one JSON-RPC 2.0 message from fd_in (blocks).
 * Returns heap-allocated NUL-terminated body, or NULL on EOF/error.
 * Caller must free(). */
char *lsp_read_message(int fd_in);

/* Write one JSON-RPC 2.0 message to fd_out with Content-Length framing. */
void lsp_write_message(int fd_out, const char *json, size_t len);

#endif
