#ifndef TUR_LSP_SESSION_H
#define TUR_LSP_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
#include "lsp_sink.h"

/* -------------------------------------------------------------------------
 * Transport-free LSP session.
 *
 * `tur lsp` is a blocking read(2)/write(2) loop, and a browser has neither
 * descriptor. Everything the server actually does -- analysis, completion,
 * hover, diagnostics -- is independent of that; only the loop around it is
 * not. This is the loop-free entry point: one message in, the messages the
 * server produced out.
 *
 * The replies come back as a JSON array (`[{...response...},{...notif...}]`),
 * not Content-Length framed bytes. A caller that is going to parse them
 * in-process has no use for framing it would immediately discard.
 * --------------------------------------------------------------------- */

/* Feed one JSON-RPC message to the server. `out` is initialised by the callee
 * and receives a JSON array of every message the handler produced -- the
 * response, plus any publishDiagnostics notifications a deferred analysis
 * flushed along the way. `out->data` is NUL-terminated.
 *
 * Returns false if the client sent `exit`, in which case the session has been
 * torn down and every open document freed. Unlike the stdio loop this never
 * calls _exit(): the caller's process is the browser tab, and killing it is
 * not this function's decision to make.
 */
bool lsp_session_handle(const char *msg, size_t len, Buf *out);

/* Analyze every document with pending edits and collect the resulting
 * publishDiagnostics notifications into `out` (same JSON-array shape).
 *
 * The stdio server runs this off a poll() timeout on the input descriptor.
 * A client that owns its own debounce -- which is every non-stdio client --
 * calls this instead when its quiet window expires. Without it, diagnostics
 * for an edited buffer would not appear until something else asked a question
 * that happened to need symbols.
 */
void lsp_session_flush(Buf *out);

/* Drop all session state: open documents, the cancel queue, the stdlib symbol
 * cache, and the initialize/shutdown handshake flags. The next call starts a
 * fresh server. */
void lsp_session_reset(void);

#endif
