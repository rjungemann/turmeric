/* fuzz_httpd_head -- stdlib/httpd.tur's request parsing (M-4), no socket.
 * httpd_entry.c is `tur emit-c tests/fuzz/httpd_entry.tur`; fuzz_hyhttp
 * parses the input as a request head plus body with httpd-parse-head, then
 * runs the header, cookie, form, multipart and JSON accessors over it. */
#define main tur_fuzz_entry_main
#include "httpd_entry.c"
#undef main

#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The server hands the parser a NUL-terminated buffer; do the same. */
    char *buf = fuzz_cstr(data, size);
    (void)fuzz_hyhttp((void *)buf, (int64_t)size);
    free(buf);
    return 0;
}
