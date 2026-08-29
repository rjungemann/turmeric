#include "serial.h"

#include "trail.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * CRC-32 (ISO 3309 polynomial 0xEDB88320, reflected form)
 * --------------------------------------------------------------------------- */

static uint32_t s_crc32_table[256];
static int      s_crc32_init = 0;

static void crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
        s_crc32_table[i] = crc;
    }
    s_crc32_init = 1;
}

uint32_t serial_crc32(uint32_t crc, const void *buf, size_t len) {
    if (!s_crc32_init) crc32_build_table();
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) {
        crc = (crc >> 8) ^ s_crc32_table[(crc ^ *p++) & 0xFF];
    }
    return ~crc;
}

uint32_t serial_schema_ver(const char * const *names,
                            const SerialTypeTag *tags,
                            size_t n) {
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc = serial_crc32(crc, names[i], strlen(names[i]) + 1); /* include NUL */
        uint8_t tag = (uint8_t)tags[i];
        crc = serial_crc32(crc, &tag, 1);
    }
    return crc;
}

/* ---------------------------------------------------------------------------
 * Symbol registry
 * --------------------------------------------------------------------------- */

typedef struct RegistryEntry {
    const char           *symbol_key;
    size_t                frame_size;
    serial_reconstruct_fn reconstruct;
} RegistryEntry;

static RegistryEntry s_registry[SERIAL_REGISTRY_MAX];
static size_t        s_registry_n = 0;

bool serial_register(const char *symbol_key,
                     size_t frame_size,
                     serial_reconstruct_fn reconstruct) {
    if (s_registry_n >= SERIAL_REGISTRY_MAX) {
        fprintf(stderr, "serial_register: registry full (limit %d)\n",
                SERIAL_REGISTRY_MAX);
        return false;
    }
    s_registry[s_registry_n].symbol_key = symbol_key;
    s_registry[s_registry_n].frame_size = frame_size;
    s_registry[s_registry_n].reconstruct = reconstruct;
    s_registry_n++;
    return true;
}

serial_reconstruct_fn serial_lookup(const char *symbol_key) {
    for (size_t i = 0; i < s_registry_n; i++) {
        if (strcmp(s_registry[i].symbol_key, symbol_key) == 0) {
            return s_registry[i].reconstruct;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Lifecycle helpers
 * --------------------------------------------------------------------------- */

SerialFrame *serial_frame_alloc(size_t n_fields) {
    SerialFrame *f = (SerialFrame *)calloc(1, sizeof(SerialFrame));
    if (!f) return NULL;
    if (n_fields > 0) {
        f->fields = (SerialField *)calloc(n_fields, sizeof(SerialField));
        if (!f->fields) { free(f); return NULL; }
    }
    f->n_fields = n_fields;
    return f;
}

void serial_frame_free(SerialFrame *frame) {
    if (!frame) return;
    /* Free any heap-allocated string/bytes values in fields. */
    for (size_t i = 0; i < frame->n_fields; i++) {
        SerialField *fld = &frame->fields[i];
        if (fld->tag == STAG_CSTR && fld->value.s) {
            free((void *)fld->value.s);
            fld->value.s = NULL;
        } else if (fld->tag == STAG_BYTES && fld->value.bytes.data) {
            free(fld->value.bytes.data);
            fld->value.bytes.data = NULL;
        }
    }
    free(frame->fields);
    free(frame);
}

void serial_frame_chain_free(SerialFrame *head) {
    while (head) {
        SerialFrame *next = head->parent;
        serial_frame_free(head);
        head = next;
    }
}

/* ---------------------------------------------------------------------------
 * Circular-reference detection
 * --------------------------------------------------------------------------- */

#define PTRSET_INITIAL 64

struct SerialPtrSet {
    const void **ptrs;
    size_t       n;
    size_t       cap;
};

SerialPtrSet *serial_ptrset_alloc(void) {
    SerialPtrSet *s = (SerialPtrSet *)calloc(1, sizeof(SerialPtrSet));
    if (!s) return NULL;
    s->ptrs = (const void **)calloc(PTRSET_INITIAL, sizeof(void *));
    if (!s->ptrs) { free(s); return NULL; }
    s->cap = PTRSET_INITIAL;
    return s;
}

void serial_ptrset_free(SerialPtrSet *set) {
    if (!set) return;
    free(set->ptrs);
    free(set);
}

bool serial_ptrset_contains(SerialPtrSet *set, const void *ptr) {
    for (size_t i = 0; i < set->n; i++) {
        if (set->ptrs[i] == ptr) return true;
    }
    /* Not found — insert. */
    if (set->n >= set->cap) {
        size_t new_cap = set->cap * 2;
        const void **np = (const void **)realloc(set->ptrs,
                                                  new_cap * sizeof(void *));
        if (!np) return false; /* treat as not-seen on OOM */
        set->ptrs = np;
        set->cap  = new_cap;
    }
    set->ptrs[set->n++] = ptr;
    return false;
}

/* ---------------------------------------------------------------------------
 * Wire format helpers — write to a growing buffer
 * --------------------------------------------------------------------------- */

typedef struct WireBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} WireBuf;

static bool wire_reserve(WireBuf *wb, size_t extra) {
    if (wb->len + extra <= wb->cap) return true;
    size_t new_cap = wb->cap ? wb->cap * 2 : 256;
    while (new_cap < wb->len + extra) new_cap *= 2;
    uint8_t *np = (uint8_t *)realloc(wb->data, new_cap);
    if (!np) return false;
    wb->data = np;
    wb->cap  = new_cap;
    return true;
}

static bool wire_u8(WireBuf *wb, uint8_t v) {
    if (!wire_reserve(wb, 1)) return false;
    wb->data[wb->len++] = v;
    return true;
}

static bool wire_u16le(WireBuf *wb, uint16_t v) {
    if (!wire_reserve(wb, 2)) return false;
    wb->data[wb->len++] = (uint8_t)(v & 0xff);
    wb->data[wb->len++] = (uint8_t)((v >> 8) & 0xff);
    return true;
}

static bool wire_u32le(WireBuf *wb, uint32_t v) {
    if (!wire_reserve(wb, 4)) return false;
    wb->data[wb->len++] = (uint8_t)(v & 0xff);
    wb->data[wb->len++] = (uint8_t)((v >>  8) & 0xff);
    wb->data[wb->len++] = (uint8_t)((v >> 16) & 0xff);
    wb->data[wb->len++] = (uint8_t)((v >> 24) & 0xff);
    return true;
}

static bool wire_u64le(WireBuf *wb, uint64_t v) {
    if (!wire_reserve(wb, 8)) return false;
    for (int i = 0; i < 8; i++) {
        wb->data[wb->len++] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    return true;
}

/* Length-prefixed UTF-8 string (4-byte LE length). */
static bool wire_lstr(WireBuf *wb, const char *s) {
    uint32_t slen = s ? (uint32_t)strlen(s) : 0;
    if (!wire_u32le(wb, slen)) return false;
    if (slen > 0) {
        if (!wire_reserve(wb, slen)) return false;
        memcpy(wb->data + wb->len, s, slen);
        wb->len += slen;
    }
    return true;
}

/* Encode one field's payload according to its tag. */
static bool wire_field_payload(WireBuf *wb, const SerialField *fld) {
    switch (fld->tag) {
    case STAG_INT64:
        return wire_u64le(wb, (uint64_t)fld->value.i);
    case STAG_FLOAT64: {
        uint64_t bits;
        double dv = fld->value.f;
        memcpy(&bits, &dv, 8);
        return wire_u64le(wb, bits);
    }
    case STAG_BOOL:
        return wire_u8(wb, fld->value.b ? 1 : 0);
    case STAG_CSTR:
        return wire_lstr(wb, fld->value.s);
    case STAG_BYTES: {
        uint32_t dlen = (uint32_t)fld->value.bytes.len;
        if (!wire_u32le(wb, dlen)) return false;
        if (dlen > 0) {
            if (!wire_reserve(wb, dlen)) return false;
            memcpy(wb->data + wb->len, fld->value.bytes.data, dlen);
            wb->len += dlen;
        }
        return true;
    }
    case STAG_PTR:
        /* Opaque pointers carry host-process addresses and cannot be serialized. */
        fprintf(stderr, "serial error: attempt to serialize an opaque pointer (STAG_PTR)\n");
        abort();
        return false;
    case STAG_NIL:
        return true; /* nothing to emit */
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Wire format codec — serialisation
 * --------------------------------------------------------------------------- */

uint8_t *serial_cont_to_bytes(const SerialFrame *head, size_t *out_len) {
    WireBuf wb = {0};
    *out_len = 0;

    /* SX1 (solver-extension-plan 3.5): refuse inside a trail scope.
     *
     * A serialized continuation captures CONTROL.  The trailed writes made under
     * an open `bt-scope` are not part of it, and the undo information that would
     * put them back lives in this process's trail -- which does not travel.  So a
     * blob taken here would deserialize into a world where the writes either
     * never happened (fresh process) or are still live with no way to unwind
     * them (same process, scope since exited).  Both are silent wrong answers.
     *
     * Refusing is the SX1 answer, per 3.5: serializing the live trail segment
     * alongside the continuation is the alternative, and it is declined for the
     * same reason (b) is -- a snapshot restores symmetrically, which is not what
     * the state under a search scope wants. */
    uint32_t lvl = tur_trail_level();
    if (lvl != 0) {
        fprintf(stderr,
                "serial error: cannot serialize a continuation inside a trail "
                "scope (bt-scope depth %u, %u trailed write%s outstanding)\n"
                "  the trail's undo information is process-local and does not "
                "travel with the blob\n"
                "  serialize outside the enclosing bt-scope, or commit the "
                "level first with bt-commit-to!\n",
                lvl, tur_trail_depth(), tur_trail_depth() == 1 ? "" : "s");
        return NULL;
    }

    /* Count frames. */
    uint32_t n_frames = 0;
    for (const SerialFrame *f = head; f; f = f->parent) n_frames++;

    /* Circular-reference guard. */
    SerialPtrSet *seen = serial_ptrset_alloc();
    if (!seen) return NULL;

    /* ---- Header ---- */
    if (!wire_reserve(&wb, 4)) goto err;
    memcpy(wb.data + wb.len, SERIAL_WIRE_MAGIC, 4);
    wb.len += 4;
    if (!wire_u16le(&wb, SERIAL_WIRE_VERSION)) goto err;
    if (!wire_u32le(&wb, n_frames)) goto err;

    /* ---- Frames ---- */
    for (const SerialFrame *f = head; f; f = f->parent) {
        if (serial_ptrset_contains(seen, f)) {
            fprintf(stderr, "serial_cont_to_bytes: circular frame reference\n");
            goto err;
        }
        if (!wire_lstr(&wb, f->symbol_key)) goto err;
        if (!wire_u32le(&wb, f->schema_ver)) goto err;
        if (!wire_u32le(&wb, (uint32_t)f->n_fields)) goto err;
        for (size_t fi = 0; fi < f->n_fields; fi++) {
            const SerialField *fld = &f->fields[fi];
            if (!wire_lstr(&wb, fld->name)) goto err;
            if (!wire_u8(&wb, (uint8_t)fld->tag)) goto err;
            if (!wire_field_payload(&wb, fld)) goto err;
        }
    }

    /* ---- Checksum ---- */
    uint32_t crc = serial_crc32(0, wb.data, wb.len);
    if (!wire_u32le(&wb, crc)) goto err;

    serial_ptrset_free(seen);
    *out_len = wb.len;
    return wb.data;

err:
    serial_ptrset_free(seen);
    free(wb.data);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Wire format codec — deserialisation
 * --------------------------------------------------------------------------- */

/* Cursor for reading the wire buffer. */
typedef struct WireCursor {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} WireCursor;

static bool cur_read(WireCursor *c, void *dst, size_t n) {
    if (c->pos + n > c->len) return false;
    memcpy(dst, c->data + c->pos, n);
    c->pos += n;
    return true;
}

static bool cur_u8(WireCursor *c, uint8_t *out) {
    return cur_read(c, out, 1);
}

static bool cur_u16le(WireCursor *c, uint16_t *out) {
    uint8_t b[2];
    if (!cur_read(c, b, 2)) return false;
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

static bool cur_u32le(WireCursor *c, uint32_t *out) {
    uint8_t b[4];
    if (!cur_read(c, b, 4)) return false;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] <<  8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return true;
}

static bool cur_u64le(WireCursor *c, uint64_t *out) {
    uint8_t b[8];
    if (!cur_read(c, b, 8)) return false;
    *out = 0;
    for (int i = 0; i < 8; i++) *out |= ((uint64_t)b[i] << (8 * i));
    return true;
}

/* Read a length-prefixed string (allocates a NUL-terminated copy). */
static bool cur_lstr(WireCursor *c, char **out) {
    uint32_t slen;
    if (!cur_u32le(c, &slen)) return false;
    char *s = (char *)malloc(slen + 1);
    if (!s) return false;
    if (slen > 0 && !cur_read(c, s, slen)) { free(s); return false; }
    s[slen] = '\0';
    *out = s;
    return true;
}

bool serial_cont_from_bytes(const uint8_t *data, size_t len,
                             SerialFrame **out_head, const char **out_err) {
    static const char *err_short    = "wire: buffer too short";
    static const char *err_magic    = "wire: bad magic (expected TSER)";
    static const char *err_version  = "wire: unsupported format version";
    static const char *err_crc      = "wire: CRC mismatch";
    static const char *err_oom      = "wire: out of memory";
    static const char *err_unknown  = "wire: unknown symbol key";
    static const char *err_schema   = "wire: schema version mismatch";
    static const char *err_tag      = "wire: unknown type tag";

    if (out_err) *out_err = NULL;
    *out_head = NULL;

    if (len < 4 + 2 + 4 + 4) { /* magic + ver + n_frames + crc minimum */
        if (out_err) *out_err = err_short;
        return false;
    }

    /* Verify CRC (covers everything except the trailing 4-byte crc itself). */
    uint32_t stored_crc = 0;
    {
        const uint8_t *p = data + len - 4;
        stored_crc = (uint32_t)p[0]
                   | ((uint32_t)p[1] <<  8)
                   | ((uint32_t)p[2] << 16)
                   | ((uint32_t)p[3] << 24);
    }
    uint32_t computed_crc = serial_crc32(0, data, len - 4);
    if (stored_crc != computed_crc) {
        if (out_err) *out_err = err_crc;
        return false;
    }

    WireCursor c = { data, len - 4 /* exclude CRC */, 0 };

    /* Magic. */
    char magic[4];
    if (!cur_read(&c, magic, 4)) { if (out_err) *out_err = err_short; return false; }
    if (memcmp(magic, SERIAL_WIRE_MAGIC, 4) != 0) {
        if (out_err) *out_err = err_magic;
        return false;
    }

    /* Version. */
    uint16_t ver;
    if (!cur_u16le(&c, &ver)) { if (out_err) *out_err = err_short; return false; }
    if (ver != SERIAL_WIRE_VERSION) {
        if (out_err) *out_err = err_version;
        return false;
    }

    /* Frame count. */
    uint32_t n_frames;
    if (!cur_u32le(&c, &n_frames)) { if (out_err) *out_err = err_short; return false; }

    /* Decode frames; build chain with innermost first. */
    SerialFrame *chain_head = NULL;
    SerialFrame *chain_tail = NULL;

    for (uint32_t fi = 0; fi < n_frames; fi++) {
        /* symbol_key */
        char *sym_key = NULL;
        if (!cur_lstr(&c, &sym_key)) {
            serial_frame_chain_free(chain_head);
            if (out_err) *out_err = err_short;
            return false;
        }

        /* schema_ver */
        uint32_t schema_ver;
        if (!cur_u32le(&c, &schema_ver)) {
            free(sym_key);
            serial_frame_chain_free(chain_head);
            if (out_err) *out_err = err_short;
            return false;
        }

        /* Look up reconstruction function. */
        serial_reconstruct_fn reconstruct = serial_lookup(sym_key);
        if (!reconstruct) {
            fprintf(stderr, "serial_cont_from_bytes: unknown frame key '%s'\n", sym_key);
            free(sym_key);
            serial_frame_chain_free(chain_head);
            if (out_err) *out_err = err_unknown;
            return false;
        }

        /* field count */
        uint32_t n_fields;
        if (!cur_u32le(&c, &n_fields)) {
            free(sym_key);
            serial_frame_chain_free(chain_head);
            if (out_err) *out_err = err_short;
            return false;
        }

        /* Allocate a temporary frame to hold decoded wire data. */
        SerialFrame *wf = serial_frame_alloc((size_t)n_fields);
        if (!wf) {
            free(sym_key);
            serial_frame_chain_free(chain_head);
            if (out_err) *out_err = err_oom;
            return false;
        }
        wf->symbol_key = sym_key; /* owned by wf; free'd in serial_frame_free */
        wf->schema_ver = schema_ver;

        /* Decode fields. */
        bool field_ok = true;
        for (uint32_t fldidx = 0; fldidx < n_fields && field_ok; fldidx++) {
            SerialField *fld = &wf->fields[fldidx];

            char *fname = NULL;
            if (!cur_lstr(&c, &fname)) { field_ok = false; break; }
            fld->name = fname; /* owned */

            uint8_t tag_byte;
            if (!cur_u8(&c, &tag_byte)) { field_ok = false; break; }
            fld->tag = (SerialTypeTag)tag_byte;

            switch (fld->tag) {
            case STAG_INT64: {
                uint64_t v;
                if (!cur_u64le(&c, &v)) { field_ok = false; break; }
                fld->value.i = (int64_t)v;
                break;
            }
            case STAG_FLOAT64: {
                uint64_t bits;
                if (!cur_u64le(&c, &bits)) { field_ok = false; break; }
                memcpy(&fld->value.f, &bits, 8);
                break;
            }
            case STAG_BOOL: {
                uint8_t bv;
                if (!cur_u8(&c, &bv)) { field_ok = false; break; }
                fld->value.b = (bv != 0);
                break;
            }
            case STAG_CSTR: {
                char *sv = NULL;
                if (!cur_lstr(&c, &sv)) { field_ok = false; break; }
                fld->value.s = sv;
                break;
            }
            case STAG_BYTES: {
                uint32_t dlen;
                if (!cur_u32le(&c, &dlen)) { field_ok = false; break; }
                fld->value.bytes.len = dlen;
                fld->value.bytes.data = NULL;
                if (dlen > 0) {
                    fld->value.bytes.data = (uint8_t *)malloc(dlen);
                    if (!fld->value.bytes.data) { field_ok = false; break; }
                    if (!cur_read(&c, fld->value.bytes.data, dlen)) {
                        free(fld->value.bytes.data);
                        fld->value.bytes.data = NULL;
                        field_ok = false;
                    }
                }
                break;
            }
            case STAG_PTR: {
                /* Opaque pointers cannot be deserialized; abort. */
                fprintf(stderr, "serial error: attempt to deserialize an opaque pointer (STAG_PTR)\n");
                if (out_err) *out_err = err_tag;
                field_ok = false;
                break;
            }
            case STAG_NIL:
                break; /* nothing to read */
            default:
                fprintf(stderr, "serial_cont_from_bytes: unknown tag %d\n", (int)fld->tag);
                if (out_err) *out_err = err_tag;
                field_ok = false;
                break;
            }
        }

        if (!field_ok) {
            serial_frame_free(wf);
            free(sym_key);
            serial_frame_chain_free(chain_head);
            if (out_err && !*out_err) *out_err = err_short;
            return false;
        }

        /* Validate schema version against the registered descriptor. */
        /* Reconstruction function can do further validation; for the base
         * path we check schema_ver after reconstructing so the registered
         * function has the compiled schema_ver available. */
        SerialFrame *live = reconstruct(wf);
        serial_frame_free(wf);
        free(sym_key);

        if (!live) {
            serial_frame_chain_free(chain_head);
            if (out_err) *out_err = err_schema;
            return false;
        }

        /* Append to chain (innermost first). */
        if (!chain_head) {
            chain_head = live;
            chain_tail = live;
        } else {
            chain_tail->parent = live;
            chain_tail = live;
        }
    }

    *out_head = chain_head;
    return true;
}
