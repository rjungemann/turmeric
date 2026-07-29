#include "lsp_sink.h"
#include "lsp_io.h"

LspSink lsp_sink_fd(int fd) {
    LspSink s;
    s.fd    = fd;
    s.buf   = NULL;
    s.count = 0;
    return s;
}

LspSink lsp_sink_buf(Buf *out) {
    LspSink s;
    s.fd    = -1;
    s.buf   = out;
    s.count = 0;
    return s;
}

void lsp_sink_buf_open(LspSink *sink) {
    if (!sink || !sink->buf) return;
    buf_putc(sink->buf, '[');
    sink->count = 0;
}

void lsp_sink_buf_close(LspSink *sink) {
    if (!sink || !sink->buf) return;
    buf_putc(sink->buf, ']');
    /* NUL-terminate without counting the terminator in `len`, so the buffer
     * doubles as a C string for the WASM bridge and the test harness while
     * `len` still describes just the JSON. */
    buf_putc(sink->buf, '\0');
    sink->buf->len--;
}

void lsp_sink_send(LspSink *sink, const char *json, size_t len) {
    if (!sink || !json) return;
    if (sink->buf) {
        if (sink->count > 0) buf_putc(sink->buf, ',');
        buf_write(sink->buf, json, len);
        sink->count++;
        return;
    }
    if (sink->fd >= 0) lsp_write_message(sink->fd, json, len);
}
