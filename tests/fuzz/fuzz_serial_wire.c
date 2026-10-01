/* fuzz_serial_wire -- the TSER continuation wire codec (src/runtime/serial.c).
 *
 * serial_cont_from_bytes verifies a trailing CRC-32 over the whole buffer
 * before it parses anything, so the harness appends the right CRC to the
 * fuzz input -- otherwise nothing past the CRC check is ever reached.  Two
 * frame keys are registered so the "known key" paths run too. */
#include "fuzz_common.h"
#include "runtime/serial.h"

/* A fresh empty frame: enough for the decoder to hand one back and for the
 * chain to be freed.  The decoder, not the callback, is under test. */
static SerialFrame *reconstruct(const SerialFrame *enc) {
    (void)enc;
    SerialFrame *f = serial_frame_alloc(0);
    if (f) f->symbol_key = "k";
    return f;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    serial_register("k", 0, reconstruct);
    serial_register("main::f::frame_0", 0, reconstruct);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *buf = (uint8_t *)malloc(size + 4);
    if (size) memcpy(buf, data, size);
    uint32_t crc = serial_crc32(0, buf, size);
    for (int i = 0; i < 4; i++) buf[size + i] = (uint8_t)(crc >> (8 * i));
    SerialFrame *head = NULL;
    const char *err = NULL;
    if (serial_cont_from_bytes(buf, size + 4, &head, &err))
        serial_frame_chain_free(head);
    free(buf);
    return 0;
}
