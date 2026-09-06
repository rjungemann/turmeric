/*
 * src/runtime/image.c -- Application image header codec (plan AI1).
 *
 * Frames a Phase 21 TSER payload with a self-validating 72-byte header. See
 * image.h for the on-disk layout. All multi-byte fields are written/read in
 * little-endian byte order so the format is host-portable.
 */

#include "image.h"

#include "sha256.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Compile-time guarantee that the header is exactly 72 bytes. */
_Static_assert(sizeof(TurImageHeader) == TUR_IMAGE_HEADER_SIZE,
               "TurImageHeader must be exactly 72 bytes");

/* ---------------------------------------------------------------------------
 * CRC-32 (ISO 3309 / ITU-T V.42), self-contained so image.c needs no other
 * runtime object. Matches serial_crc32's polynomial (0xEDB88320).
 * --------------------------------------------------------------------------- */
static uint32_t image_crc32(uint32_t crc, const void *buf, size_t len)
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

/* ---------------------------------------------------------------------------
 * Little-endian scalar encoders/decoders into a fixed 72-byte buffer.
 * --------------------------------------------------------------------------- */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Field offsets within the 72-byte on-disk header. */
#define OFF_MAGIC        0
#define OFF_VERSION      4
#define OFF_BUILD_STAMP  8
#define OFF_PAYLOAD_LEN  40
#define OFF_CREATED      48
#define OFF_GLOBALS_OFF  56
#define OFF_FLAGS        64
#define OFF_CRC          68

const char *tur_image_strerror(TurImageError err)
{
    switch (err) {
        case IMAGE_OK:              return "ok";
        case IMAGE_BAD_MAGIC:       return "bad magic (not a TURI image)";
        case IMAGE_BAD_VERSION:     return "unsupported image version";
        case IMAGE_BAD_BUILD_STAMP: return "build-stamp mismatch (foreign binary)";
        case IMAGE_BAD_CRC:         return "header CRC mismatch (corrupted)";
        case IMAGE_TRUNCATED:       return "truncated image (short header)";
        case IMAGE_IO_ERROR:        return "I/O error";
    }
    return "unknown image error";
}

/* Encode the header into a 72-byte little-endian on-disk buffer. The CRC is
 * computed over bytes [0, OFF_CRC) and written into the final 4 bytes. */
static void encode_header(uint8_t buf[TUR_IMAGE_HEADER_SIZE],
                          const TurImageHeader *h)
{
    memset(buf, 0, TUR_IMAGE_HEADER_SIZE);
    put_u32(buf + OFF_MAGIC,       h->magic);
    put_u32(buf + OFF_VERSION,     h->version);
    memcpy(buf + OFF_BUILD_STAMP,  h->build_stamp, TUR_IMAGE_STAMP_LEN);
    put_u64(buf + OFF_PAYLOAD_LEN, h->payload_len);
    put_u64(buf + OFF_CREATED,     h->created_unix_ns);
    put_u64(buf + OFF_GLOBALS_OFF, h->globals_offset);
    put_u32(buf + OFF_FLAGS,       h->flags);
    put_u32(buf + OFF_CRC,         image_crc32(0, buf, OFF_CRC));
}

TurImageError tur_image_write(FILE *f,
                              const uint8_t *payload,
                              size_t payload_len,
                              const uint8_t build_stamp[TUR_IMAGE_STAMP_LEN])
{
    TurImageHeader h;
    memset(&h, 0, sizeof h);
    h.magic           = TUR_IMAGE_MAGIC;
    h.version         = TUR_IMAGE_VERSION;
    if (build_stamp) memcpy(h.build_stamp, build_stamp, TUR_IMAGE_STAMP_LEN);
    h.payload_len     = (uint64_t)payload_len;
    h.globals_offset  = 0;
    h.flags           = 0;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        h.created_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;

    uint8_t hdr[TUR_IMAGE_HEADER_SIZE];
    encode_header(hdr, &h);

    if (fwrite(hdr, 1, TUR_IMAGE_HEADER_SIZE, f) != TUR_IMAGE_HEADER_SIZE)
        return IMAGE_IO_ERROR;
    if (payload_len > 0 &&
        fwrite(payload, 1, payload_len, f) != payload_len)
        return IMAGE_IO_ERROR;
    return IMAGE_OK;
}

TurImageError tur_image_read_header(FILE *f, TurImageHeader *out)
{
    uint8_t hdr[TUR_IMAGE_HEADER_SIZE];
    size_t got = fread(hdr, 1, TUR_IMAGE_HEADER_SIZE, f);
    if (got != TUR_IMAGE_HEADER_SIZE)
        return ferror(f) ? IMAGE_IO_ERROR : IMAGE_TRUNCATED;

    uint32_t magic = get_u32(hdr + OFF_MAGIC);
    if (magic != TUR_IMAGE_MAGIC)
        return IMAGE_BAD_MAGIC;

    uint32_t stored_crc = get_u32(hdr + OFF_CRC);
    if (stored_crc != image_crc32(0, hdr, OFF_CRC))
        return IMAGE_BAD_CRC;

    uint32_t version = get_u32(hdr + OFF_VERSION);
    if (version != TUR_IMAGE_VERSION)
        return IMAGE_BAD_VERSION;

    if (out) {
        memset(out, 0, sizeof *out);
        out->magic           = magic;
        out->version         = version;
        memcpy(out->build_stamp, hdr + OFF_BUILD_STAMP, TUR_IMAGE_STAMP_LEN);
        out->payload_len     = get_u64(hdr + OFF_PAYLOAD_LEN);
        out->created_unix_ns = get_u64(hdr + OFF_CREATED);
        out->globals_offset  = get_u64(hdr + OFF_GLOBALS_OFF);
        out->flags           = get_u32(hdr + OFF_FLAGS);
        out->header_crc32    = stored_crc;
    }
    return IMAGE_OK;
}

/* ---------------------------------------------------------------------------
 * SHA-256 of a file (for `tur image-verify <image> <binary>`). Mirrors the
 * digest used by stdlib/image.tur's build stamp. The algorithm lives in
 * runtime/sha256.c -- there used to be a second copy here, and a third shelled
 * out to sha256sum from pkg.c.
 * --------------------------------------------------------------------------- */
bool tur_image_sha256_file(const char *path, uint8_t out[TUR_IMAGE_STAMP_LEN])
{
    _Static_assert(TUR_IMAGE_STAMP_LEN == TUR_SHA256_DIGEST_LEN,
                   "the build stamp is a SHA-256 digest");
    return tur_sha256_file(path, out);
}
