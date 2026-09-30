/* fuzz_image_header -- the application-image header and payload check (M-1).
 *
 * tur_image_read_header + tur_image_verify_payload over an in-memory FILE,
 * exactly as `tur image-info` / `tur image-verify` drive them.  Most random
 * inputs die on the magic or the header CRC, so the harness also runs a
 * second pass with a valid magic and a recomputed header CRC -- that is what
 * reaches the version / flags / globals-offset / payload checks. */
#define _GNU_SOURCE 1
#include "fuzz_common.h"
#include "runtime/image.h"

#include <stdio.h>

static uint32_t crc32_le(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static void run(uint8_t *buf, size_t size) {
    FILE *f = fmemopen(buf, size, "rb");
    if (!f) return;
    TurImageHeader h;
    if (tur_image_read_header(f, &h) == IMAGE_OK) (void)tur_image_verify_payload(f, &h);
    fclose(f);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    uint8_t *buf = (uint8_t *)malloc(size);
    memcpy(buf, data, size);
    run(buf, size);
    if (size >= TUR_IMAGE_HEADER_SIZE) {
        buf[0] = 0x49; buf[1] = 0x52; buf[2] = 0x55; buf[3] = 0x54;   /* "TURI" LE */
        uint32_t crc = crc32_le(buf, 68);
        for (int i = 0; i < 4; i++) buf[68 + i] = (uint8_t)(crc >> (8 * i));
        run(buf, size);
    }
    free(buf);
    return 0;
}
