/*
 * tests/image_unit.c -- Application image header codec unit test (plan AI1).
 *
 * Covers:
 *   AI1.1  sizeof(TurImageHeader) == 72 (compile-time + runtime check)
 *   AI1.2  write -> read-header round-trips faithfully; bad magic / bad CRC /
 *          version mismatch are rejected with the named error codes.
 */

#include "runtime/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Local CRC-32 mirror of image.c's, used only to forge a valid-CRC header
 * with a bogus version so the version check can be isolated from the CRC check. */
static uint32_t test_crc32(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }            \
    else         { printf("ok:   %s\n", (msg)); }                        \
} while (0)

/* Write `payload` framed as an image into a temp file, return its path bytes
 * via a caller buffer. Returns 0 on success. */
static int write_image(const char *path,
                       const uint8_t *payload, size_t len,
                       const uint8_t stamp[32])
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    TurImageError e = tur_image_write(f, payload, len, stamp);
    fclose(f);
    return e == IMAGE_OK ? 0 : -1;
}

int main(void)
{
    /* AI1.1 -- size is fixed at 72 bytes. */
    CHECK(sizeof(TurImageHeader) == 72, "sizeof(TurImageHeader) == 72");
    CHECK(TUR_IMAGE_HEADER_SIZE == 72, "TUR_IMAGE_HEADER_SIZE == 72");

    const char *path = "image_unit.tmp.img";
    uint8_t stamp[32];
    for (int i = 0; i < 32; i++) stamp[i] = (uint8_t)(0xA0 + i);
    const uint8_t payload[] = { 'T','S','E','R', 1, 2, 3, 4, 5, 6, 7, 8 };
    const size_t plen = sizeof payload;

    /* AI1.2 -- faithful round trip of the header. */
    CHECK(write_image(path, payload, plen, stamp) == 0, "tur_image_write succeeds");

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "reopen image for reading");
    TurImageHeader h;
    TurImageError e = tur_image_read_header(f, &h);
    CHECK(e == IMAGE_OK, "tur_image_read_header == IMAGE_OK");
    CHECK(h.magic == TUR_IMAGE_MAGIC, "magic round-trips");
    CHECK(h.version == TUR_IMAGE_VERSION, "version round-trips");
    CHECK(h.payload_len == plen, "payload_len round-trips");
    CHECK(memcmp(h.build_stamp, stamp, 32) == 0, "build_stamp round-trips");
    CHECK(h.flags == 0, "flags == 0");
    CHECK(h.globals_offset == 0, "globals_offset == 0");
    CHECK(h.created_unix_ns != 0, "created_unix_ns stamped");

    /* File must be positioned at the payload; read it back and compare. */
    uint8_t rb[sizeof payload];
    size_t got = fread(rb, 1, plen, f);
    CHECK(got == plen && memcmp(rb, payload, plen) == 0, "payload follows header");
    fclose(f);

    /* AI1.2 -- bad magic is rejected. */
    {
        FILE *w = fopen("image_bad_magic.tmp.img", "wb");
        write_image("image_bad_magic.tmp.img", payload, plen, stamp);
        (void)w;
        /* Corrupt the first byte of magic. */
        FILE *rw = fopen("image_bad_magic.tmp.img", "r+b");
        fseek(rw, 0, SEEK_SET);
        uint8_t bad = 0x00;
        fwrite(&bad, 1, 1, rw);
        fclose(rw);
        FILE *r = fopen("image_bad_magic.tmp.img", "rb");
        e = tur_image_read_header(r, &h);
        fclose(r);
        CHECK(e == IMAGE_BAD_MAGIC, "bad magic -> IMAGE_BAD_MAGIC");
    }

    /* AI1.2 -- header CRC corruption is rejected. Flip a build_stamp byte
     * (inside the CRC-covered region) without touching magic. */
    {
        write_image("image_bad_crc.tmp.img", payload, plen, stamp);
        FILE *rw = fopen("image_bad_crc.tmp.img", "r+b");
        fseek(rw, 8, SEEK_SET);  /* OFF_BUILD_STAMP */
        uint8_t flipped = (uint8_t)(stamp[0] ^ 0xFF);
        fwrite(&flipped, 1, 1, rw);
        fclose(rw);
        FILE *r = fopen("image_bad_crc.tmp.img", "rb");
        e = tur_image_read_header(r, &h);
        fclose(r);
        CHECK(e == IMAGE_BAD_CRC, "flipped header byte -> IMAGE_BAD_CRC");
    }

    /* AI1.2 -- version mismatch is rejected. Forge a header with a bogus
     * version and a *valid* CRC so the version check is isolated from the
     * CRC check. */
    {
        write_image("image_bad_ver.tmp.img", payload, plen, stamp);
        uint8_t hdr[72];
        FILE *r = fopen("image_bad_ver.tmp.img", "rb");
        if (fread(hdr, 1, 72, r) != 72) { failures++; }
        fclose(r);
        /* Patch version (offset 4) to 999 little-endian, recompute CRC[0,68). */
        hdr[4] = (uint8_t)(999 & 0xff);
        hdr[5] = (uint8_t)((999 >> 8) & 0xff);
        hdr[6] = hdr[7] = 0;
        uint32_t crc = test_crc32(0, hdr, 68);
        hdr[68] = (uint8_t)(crc);
        hdr[69] = (uint8_t)(crc >> 8);
        hdr[70] = (uint8_t)(crc >> 16);
        hdr[71] = (uint8_t)(crc >> 24);
        FILE *rw = fopen("image_bad_ver.tmp.img", "r+b");
        fseek(rw, 0, SEEK_SET);
        fwrite(hdr, 1, 72, rw);
        fclose(rw);
        FILE *r2 = fopen("image_bad_ver.tmp.img", "rb");
        e = tur_image_read_header(r2, &h);
        fclose(r2);
        CHECK(e == IMAGE_BAD_VERSION, "bogus version (valid CRC) -> IMAGE_BAD_VERSION");
    }

    /* Truncated file -> IMAGE_TRUNCATED. */
    {
        FILE *w = fopen("image_trunc.tmp.img", "wb");
        fwrite("TUR", 1, 3, w);
        fclose(w);
        FILE *r = fopen("image_trunc.tmp.img", "rb");
        e = tur_image_read_header(r, &h);
        fclose(r);
        CHECK(e == IMAGE_TRUNCATED, "short file -> IMAGE_TRUNCATED");
    }

    remove(path);
    remove("image_bad_magic.tmp.img");
    remove("image_bad_crc.tmp.img");
    remove("image_bad_ver.tmp.img");
    remove("image_trunc.tmp.img");

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall image_unit checks passed\n");
    return 0;
}
