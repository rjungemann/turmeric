/* fuzz_lsp_frame -- Content-Length framing for `tur lsp` / `tur dap` (M-3).
 *
 * The peer is the user's editor, but the framing must survive anything: the
 * input is parsed once as a bare header block and once as a raw byte stream
 * through lsp_read_message over a pipe, reading messages until it reports
 * EOF or a framing error. */
#include "fuzz_common.h"
#include "lsp/lsp_io.h"

#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *hdr = fuzz_cstr(data, size);
    size_t len = 0;
    (void)lsp_parse_content_length(hdr, &len);
    free(hdr);

    /* A pipe holds 64 KiB on Linux; keep the write non-blocking in practice
     * by staying under it (libFuzzer's default -max_len is 4096). */
    if (size > 60000) return 0;
    int fds[2];
    if (pipe(fds) != 0) return 0;
    if (size && write(fds[1], data, size) != (ssize_t)size) abort();
    close(fds[1]);
    for (int i = 0; i < 64; i++) {
        char *msg = lsp_read_message(fds[0]);
        if (!msg) break;
        free(msg);
    }
    close(fds[0]);
    return 0;
}
