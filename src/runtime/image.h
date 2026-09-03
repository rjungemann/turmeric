#ifndef TUR_IMAGE_H
#define TUR_IMAGE_H

/*
 * src/runtime/image.h -- Application image dumps (plan AI1).
 *
 * An "image" file is a fixed-layout TurImageHeader followed by the Phase 21
 * TSER payload bytes produced by save-cont! (stdlib/workflow.tur). No new wire
 * format: this layer only frames the TSER payload with a self-validating
 * header so a cold restart can reject a stale/foreign image cleanly instead of
 * resuming garbage.
 *
 * On-disk layout is little-endian regardless of host byte order; the codec in
 * image.c serialises field by field rather than dumping the struct raw, so the
 * format is host-portable.
 *
 *   [magic           u32]  "TURI" little-endian (0x54555249)
 *   [version         u32]  header format version (TUR_IMAGE_VERSION)
 *   [build_stamp     32B]  binary identity (AI4); foreign image -> clean Err
 *   [payload_len     u64]  bytes of TSER payload following the header
 *   [created_unix_ns u64]  informational; not validated
 *   [globals_offset  u64]  AI3: file offset of the image-globals section
 *                          (72 + continuation bytes), 0 == no section.  The
 *                          section is [i64 len][len bytes] and sits INSIDE
 *                          payload_len, after the continuation bytes; its
 *                          bytes are the defimage-global registry snapshot
 *                          (stdlib/image.tur image/global-registry): [i64 n]
 *                          then per global [i64 name_len][name][i64 blen]
 *                          [Serializable bytes].
 *   [flags           u32]  reserved, must be 0
 *   [header_crc32    u32]  CRC32 of header bytes [0, header_crc32) -- catches
 *                          truncation/corruption of the header itself
 *
 * sizeof(TurImageHeader) == TUR_IMAGE_HEADER_SIZE == 72.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TUR_IMAGE_MAGIC       0x54555249u  /* "TURI" little-endian */
#define TUR_IMAGE_VERSION     1u
#define TUR_IMAGE_HEADER_SIZE 72u          /* fixed on-disk header size in bytes */
#define TUR_IMAGE_STAMP_LEN   32u          /* SHA-256 digest length */

typedef struct TurImageHeader {
    uint32_t magic;                          /* TUR_IMAGE_MAGIC */
    uint32_t version;                        /* TUR_IMAGE_VERSION */
    uint8_t  build_stamp[TUR_IMAGE_STAMP_LEN]; /* SHA-256 of the binary (AI4) */
    uint64_t payload_len;                    /* bytes of TSER data after header */
    uint64_t created_unix_ns;                /* informational; not validated */
    uint64_t globals_offset;                 /* AI3 globals section offset; 0 == none */
    uint32_t flags;                          /* reserved, must be 0 */
    uint32_t header_crc32;                   /* CRC32 of bytes [0, header_crc32) */
} TurImageHeader;

/* ---------------------------------------------------------------------------
 * Error codes (AI1.3). Surfaced in stdlib/image.tur as named Result errs.
 * --------------------------------------------------------------------------- */
typedef enum TurImageError {
    IMAGE_OK              =  0,
    IMAGE_BAD_MAGIC       = -1,  /* magic != "TURI" */
    IMAGE_BAD_VERSION     = -2,  /* version != TUR_IMAGE_VERSION */
    IMAGE_BAD_BUILD_STAMP = -3,  /* build_stamp mismatch (checked by caller, AI4) */
    IMAGE_BAD_CRC         = -4,  /* header_crc32 mismatch */
    IMAGE_TRUNCATED       = -5,  /* short read: fewer than 72 header bytes */
    IMAGE_IO_ERROR        = -6,  /* underlying read/write failed */
} TurImageError;

/* Human-readable name for an error code (static string, never NULL). */
const char *tur_image_strerror(TurImageError err);

/* ---------------------------------------------------------------------------
 * Codec (AI1.2)
 * --------------------------------------------------------------------------- */

/* Write a complete image: header (with build_stamp + payload_len + CRC filled
 * in) followed by payload. created_unix_ns is stamped from the wall clock.
 * Returns IMAGE_OK on success, IMAGE_IO_ERROR on a write failure. */
TurImageError tur_image_write(FILE *f,
                              const uint8_t *payload,
                              size_t payload_len,
                              const uint8_t build_stamp[TUR_IMAGE_STAMP_LEN]);

/* Read and validate the 72-byte header from the current file position. On
 * success, *out is populated (host byte order) and the file is positioned at
 * the first payload byte. Validates magic, version, and header CRC; does NOT
 * validate the build stamp (that is the caller's job, AI4). */
TurImageError tur_image_read_header(FILE *f, TurImageHeader *out);

/* Compute the SHA-256 of an entire file (used by `tur image-verify` to hash a
 * candidate loader binary and compare it against an image's build stamp).
 * Writes 32 bytes into out. Returns true on success, false if the file could
 * not be read. */
bool tur_image_sha256_file(const char *path, uint8_t out[TUR_IMAGE_STAMP_LEN]);

#endif /* TUR_IMAGE_H */
