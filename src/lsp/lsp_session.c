#include "lsp_session.h"

#include "lsp_docs.h"

/* Implemented in lsp.c, which owns the handlers and the dispatch chain. Not
 * in a public header: the split exists so one dispatch can serve two
 * transports, not to invite a third caller into the server's internals. */
bool lsp_dispatch_message(const char *msg, LspSink *sink, int fd_in);
void lsp_dispatch_flush(LspSink *sink);
void lsp_dispatch_init(void);
void lsp_dispatch_teardown(void);

bool lsp_session_handle(const char *msg, size_t len, Buf *out) {
    (void)len;   /* the body is NUL-terminated; the JSON scanners want a string */

    buf_init(out);
    LspSink sink = lsp_sink_buf(out);
    lsp_sink_buf_open(&sink);

    bool alive = true;
    if (msg) {
        alive = lsp_dispatch_message(msg, &sink, /*fd_in=*/-1);

        /* Flush any document the message just dirtied.
         *
         * The stdio server can afford to defer this: it owns the input
         * descriptor and notices the client going quiet. Here the client owns
         * the debounce, so by the time a didChange arrives it already
         * represents a quiet window -- deferring again would mean diagnostics
         * never publish until some unrelated request happened to need
         * symbols. A request that already flushed leaves nothing dirty, so
         * this is a no-op on that path rather than a second compile.
         */
        if (alive) lsp_dispatch_flush(&sink);
    }

    lsp_sink_buf_close(&sink);

    if (!alive) lsp_dispatch_teardown();
    return alive;
}

void lsp_session_flush(Buf *out) {
    buf_init(out);
    LspSink sink = lsp_sink_buf(out);
    lsp_sink_buf_open(&sink);
    lsp_dispatch_init();
    lsp_dispatch_flush(&sink);
    lsp_sink_buf_close(&sink);
}

void lsp_session_reset(void) {
    lsp_dispatch_teardown();
}
