/*
 * src/runtime/image.c -- Application image header codec (plan AI1).
 *
 * Frames a Phase 21 TSER payload with a self-validating 72-byte header. See
 * image.h for the on-disk layout. All multi-byte fields are written/read in
 * little-endian byte order so the format is host-portable.
 */

#include "image.h"

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
 * SHA-256 of a file (for `tur image-verify <image> <binary>`). Self-contained;
 * mirrors the digest used by stdlib/image.tur's build stamp.
 * --------------------------------------------------------------------------- */
static void sha256_block(uint32_t st[8], const uint8_t *blk)
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ((w[i-15]>>7)|(w[i-15]<<25))^((w[i-15]>>18)|(w[i-15]<<14))^(w[i-15]>>3);
        uint32_t s1 = ((w[i-2]>>17)|(w[i-2]<<15))^((w[i-2]>>19)|(w[i-2]<<13))^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1=((e>>6)|(e<<26))^((e>>11)|(e<<21))^((e>>25)|(e<<7));
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+K[i]+w[i];
        uint32_t S0=((a>>2)|(a<<30))^((a>>13)|(a<<19))^((a>>22)|(a<<10));
        uint32_t mj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

bool tur_image_sha256_file(const char *path, uint8_t out[TUR_IMAGE_STAMP_LEN])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    rewind(f);
    size_t n = (size_t)sz;
    size_t total = (((n + 8) / 64) + 1) * 64;
    uint8_t *P = (uint8_t *)calloc(total, 1);
    if (!P) { fclose(f); return false; }
    size_t got = fread(P, 1, n, f);
    fclose(f);
    if (got != n) { free(P); return false; }
    P[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) P[total - 1 - j] = (uint8_t)(bits >> (8 * j));
    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (size_t off = 0; off < total; off += 64) sha256_block(st, P + off);
    free(P);
    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(st[j] >> 24);
        out[j*4+1] = (uint8_t)(st[j] >> 16);
        out[j*4+2] = (uint8_t)(st[j] >> 8);
        out[j*4+3] = (uint8_t)(st[j]);
    }
    return true;
}
