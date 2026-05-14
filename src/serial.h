#ifndef TUR_SERIAL_H
#define TUR_SERIAL_H

/*
 * src/serial.h -- Phase 21: Serializable continuations
 *
 * Runtime infrastructure for serializing and deserializing continuation
 * frames to/from a self-describing binary wire format.
 *
 * Design overview (serializable-continuations-plan.md §3):
 *   - Each CPS-transformed function registers one or more SerialFrame
 *     descriptors at startup via serial_register().
 *   - A serial_cont value (opaque bytes blob) encodes the full frame chain
 *     captured by serial-shift, using the TSER wire format.
 *   - serial_cont_to_bytes() serialises; serial_cont_from_bytes() deserialises.
 *   - On resume, each frame's symbol_key is looked up via serial_lookup() to
 *     obtain the reconstruction function pointer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Type tags used in the wire format (§3.2)
 * --------------------------------------------------------------------------- */
typedef enum SerialTypeTag {
    STAG_INT64   = 0x01,
    STAG_FLOAT64 = 0x02,
    STAG_BOOL    = 0x03,
    STAG_CSTR    = 0x04,
    STAG_BYTES   = 0x05,
    STAG_PTR     = 0x06,  /* opaque pointer — serialised as 8 zero bytes (placeholder) */
    STAG_NIL     = 0x00,
} SerialTypeTag;

/* ---------------------------------------------------------------------------
 * A single captured field within a frame
 * --------------------------------------------------------------------------- */
typedef struct SerialField {
    const char    *name;       /* field name (stable, read-only data segment) */
    SerialTypeTag  tag;        /* wire type tag */
    union {
        int64_t    i;
        double     f;
        bool       b;
        const char *s;   /* STAG_CSTR — points into decoded wire data; caller owns copy */
        struct {
            uint8_t *data;
            size_t   len;
        } bytes;         /* STAG_BYTES */
    } value;
} SerialField;

/* ---------------------------------------------------------------------------
 * A single frame on the serialised continuation chain (§3.1)
 * --------------------------------------------------------------------------- */
typedef struct SerialFrame {
    const char         *symbol_key;  /* "module::fn::frame_N" — stable key */
    uint32_t            schema_ver;  /* CRC32 of field (name, tag) pairs */
    size_t              n_fields;
    SerialField        *fields;      /* heap-allocated array */
    struct SerialFrame *parent;      /* outer frame (NULL at outermost) */
} SerialFrame;

/* ---------------------------------------------------------------------------
 * Reconstruction callback registered by compiler-generated stubs
 * --------------------------------------------------------------------------- */
typedef SerialFrame *(*serial_reconstruct_fn)(const SerialFrame *encoded);

/* ---------------------------------------------------------------------------
 * Symbol registry (§3.3)
 *
 * Compiler-generated stubs call serial_register() at program startup for
 * every CPS-transformed function.  serial_lookup() retrieves the
 * reconstruction function given a stable symbol key.
 * --------------------------------------------------------------------------- */

/* Register a frame descriptor.
 *   symbol_key:   stable key string (lives in read-only data of the object file)
 *   frame_size:   sizeof the per-call env struct (for future use)
 *   reconstruct:  function that allocates a live frame from wire data
 * Returns false if the registry is full (hard limit: SERIAL_REGISTRY_MAX). */
bool serial_register(const char *symbol_key,
                     size_t      frame_size,
                     serial_reconstruct_fn reconstruct);

/* Look up a reconstruction function by symbol key.
 * Returns NULL if the key is not registered. */
serial_reconstruct_fn serial_lookup(const char *symbol_key);

/* Maximum number of registerable frames (across all compilation units). */
#define SERIAL_REGISTRY_MAX 4096

/* ---------------------------------------------------------------------------
 * Wire format codec (§3.2)
 *
 * The blob produced by serial_cont_to_bytes() is a self-contained buffer:
 *
 *   [magic: 4 bytes "TSER"]
 *   [format-version: uint16 LE]
 *   [frame-count: uint32 LE]
 *     [frame-0]
 *       [symbol-key: uint32 LE length + UTF-8 bytes]
 *       [schema-ver: uint32 LE]
 *       [field-count: uint32 LE]
 *         [field-0]
 *           [name: uint32 LE length + UTF-8 bytes]
 *           [type-tag: uint8]
 *           [payload: type-specific bytes]
 *         ...
 *     [frame-1]
 *     ...
 *   [checksum: crc32 LE of all preceding bytes]
 *
 * The returned buffer is heap-allocated; the caller must free() it.
 * --------------------------------------------------------------------------- */

#define SERIAL_WIRE_MAGIC   "TSER"
#define SERIAL_WIRE_VERSION 1

/* Serialise a frame chain to bytes.
 *   head:       innermost SerialFrame (outer frames follow via ->parent).
 *   out_len:    set to the number of bytes written.
 * Returns a malloc'd buffer, or NULL on error (check errno). */
uint8_t *serial_cont_to_bytes(const SerialFrame *head, size_t *out_len);

/* Deserialise a buffer back to a frame chain.
 *   data / len: the buffer produced by serial_cont_to_bytes().
 *   out_head:   set to the innermost SerialFrame on success.
 *   out_err:    if non-NULL and an error occurs, set to a static error string.
 * Returns true on success, false on failure. */
bool serial_cont_from_bytes(const uint8_t *data, size_t len,
                             SerialFrame **out_head, const char **out_err);

/* ---------------------------------------------------------------------------
 * Lifecycle helpers
 * --------------------------------------------------------------------------- */

/* Free all fields and the frame struct itself (does NOT follow ->parent). */
void serial_frame_free(SerialFrame *frame);

/* Free an entire frame chain (follows ->parent until NULL). */
void serial_frame_chain_free(SerialFrame *head);

/* Allocate a new SerialFrame with n_fields.
 * Caller fills in symbol_key, schema_ver, and fields[]. */
SerialFrame *serial_frame_alloc(size_t n_fields);

/* ---------------------------------------------------------------------------
 * CRC-32 (ISO 3309 / ITU-T V.42) — used for the wire format checksum and for
 * computing schema_ver from (name, type_tag) field pairs.
 * --------------------------------------------------------------------------- */
uint32_t serial_crc32(uint32_t crc, const void *buf, size_t len);

/* Compute a schema version CRC from an array of (name, tag) pairs.
 *   names / tags: parallel arrays of length n.
 * Returns a CRC32 of the concatenated name\0tag bytes. */
uint32_t serial_schema_ver(const char * const *names,
                            const SerialTypeTag *tags,
                            size_t n);

/* ---------------------------------------------------------------------------
 * Circular-reference detection (§4.3)
 *
 * A simple pointer → seen table used during serialisation to detect cycles.
 * --------------------------------------------------------------------------- */
typedef struct SerialPtrSet SerialPtrSet;

SerialPtrSet *serial_ptrset_alloc(void);
void          serial_ptrset_free(SerialPtrSet *set);
/* Returns true if ptr was already seen; inserts and returns false otherwise. */
bool          serial_ptrset_contains(SerialPtrSet *set, const void *ptr);

#endif /* TUR_SERIAL_H */
