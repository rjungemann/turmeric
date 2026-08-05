#ifndef TUR_LSP_SINK_H
#define TUR_LSP_SINK_H

#include <stddef.h>

#include "buf.h"

/* -------------------------------------------------------------------------
 * LspSink -- where a handler's outgoing messages go.
 *
 * Every handler in lsp.c used to take an `int fd_out` and call
 * lsp_write_message() on it. That is a transport decision baked into every
 * one of them, and it is the single reason the server could not run anywhere
 * a file descriptor does not exist -- a browser being the case that matters.
 *
 * The sink is the seam. Handlers write to it and stay ignorant of whether the
 * bytes end up framed on a pipe or appended to a buffer the caller is about to
 * hand to JavaScript.
 *
 * Two flavours:
 *
 *   fd  -- Content-Length framed write(2), exactly what `tur lsp` has always
 *          emitted. Byte-for-byte unchanged.
 *   buf -- appends each message as an element of a JSON array. NOT framed:
 *          the in-process caller is going to parse the result immediately, so
 *          writing a Content-Length header only to strip it back off is work
 *          that buys nothing. lsp_sink_buf_open/close bracket the array.
 *
 * The struct is public so callers can stack-allocate one; use the
 * initializers rather than filling it in by hand.
 * --------------------------------------------------------------------- */

typedef struct LspSink {
    int   fd;      /* destination fd when `buf` is NULL */
    Buf  *buf;     /* JSON-array destination when non-NULL */
    int   count;   /* messages appended so far (buffer sink only) */
} LspSink;

/* A sink that writes Content-Length framed messages to `fd`. */
LspSink lsp_sink_fd(int fd);

/* A sink that appends messages as elements of a JSON array in `out`. */
LspSink lsp_sink_buf(Buf *out);

/* Write the array's opening/closing bracket. No-ops on an fd sink, so a
 * caller that does not know which kind it holds can call them unconditionally.
 * lsp_sink_buf_close() also NUL-terminates the buffer so `out->data` is usable
 * as a C string. */
void lsp_sink_buf_open(LspSink *sink);
void lsp_sink_buf_close(LspSink *sink);

/* Emit one complete JSON-RPC message. `len` is the body length in bytes. */
void lsp_sink_send(LspSink *sink, const char *json, size_t len);

#endif
