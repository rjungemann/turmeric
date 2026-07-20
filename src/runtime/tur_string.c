/* tur_string.c -- owned, immutable, refcounted String runtime.
 * See tur_string.h for the contract and lifetime discipline. */
#include "tur_string.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Heap payload: [rc | len | bytes[len+1]].  The public handle points at the
 * header; the NUL-terminated bytes follow immediately after it. */
typedef struct {
    int64_t rc;
    int64_t len;
    /* char bytes[len + 1] follows */
} tur_string_hdr;

static inline char *str_bytes(tur_string_hdr *h) { return (char *)(h + 1); }

/* ---- construction / conversion ---------------------------------------- */

void *tur_string_from_bytes(const char *src, int64_t n) {
    if (n < 0) n = 0;
    tur_string_hdr *h =
        (tur_string_hdr *)malloc(sizeof(tur_string_hdr) + (size_t)n + 1);
    h->rc = 1;
    h->len = n;
    char *b = str_bytes(h);
    if (src && n) memcpy(b, src, (size_t)n);
    b[n] = '\0';
    return h;
}

void *tur_string_from_cstr(const char *s) {
    return tur_string_from_bytes(s, s ? (int64_t)strlen(s) : 0);
}

void *tur_string_adopt_cstr(const char *s) {
    void *r = tur_string_from_bytes(s, s ? (int64_t)strlen(s) : 0);
    free((void *)s);
    return r;
}

void *tur_string_from_int(int64_t v) {
    char buf[24]; /* -9223372036854775808 + NUL fits in 21 */
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return tur_string_from_bytes(buf, n < 0 ? 0 : (int64_t)n);
}

const char *tur_string_cstr(void *s) {
    if (!s) return "";
    return str_bytes((tur_string_hdr *)s);
}

/* ---- refcount --------------------------------------------------------- */

void *tur_string_retain(void *s) {
    if (s) ((tur_string_hdr *)s)->rc++;
    return s;
}

void tur_string_release(void *s) {
    if (!s) return;
    tur_string_hdr *h = (tur_string_hdr *)s;
    if (--h->rc == 0) free(h);
}

/* ---- queries ---------------------------------------------------------- */

int64_t tur_string_len(void *s) {
    return s ? ((tur_string_hdr *)s)->len : 0;
}

int tur_string_empty(void *s) {
    return tur_string_len(s) == 0 ? 1 : 0;
}

int64_t tur_string_byte_at(void *s, int64_t i) {
    if (!s) return -1;
    tur_string_hdr *h = (tur_string_hdr *)s;
    if (i < 0 || i >= h->len) return -1;
    return (int64_t)(unsigned char)str_bytes(h)[i];
}

int tur_string_eq(void *a, void *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    tur_string_hdr *ha = (tur_string_hdr *)a, *hb = (tur_string_hdr *)b;
    if (ha->len != hb->len) return 0;
    return memcmp(str_bytes(ha), str_bytes(hb), (size_t)ha->len) == 0 ? 1 : 0;
}

int64_t tur_string_cmp(void *a, void *b) {
    if (a == b) return 0;
    tur_string_hdr *ha = (tur_string_hdr *)a, *hb = (tur_string_hdr *)b;
    int64_t la = ha ? ha->len : 0, lb = hb ? hb->len : 0;
    int64_t n = la < lb ? la : lb;
    if (n) {
        int c = memcmp(str_bytes(ha), str_bytes(hb), (size_t)n);
        if (c < 0) return -1;
        if (c > 0) return 1;
    }
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

int64_t tur_string_hash(void *s) {
    /* FNV-1a over the exact byte range, so embedded NULs and length both
     * participate (unlike a NUL-terminated cstr hash). */
    tur_string_hdr *h = (tur_string_hdr *)s;
    uint64_t hash = 1469598103934665603ULL; /* FNV offset basis */
    if (h) {
        const unsigned char *p = (const unsigned char *)str_bytes(h);
        for (int64_t i = 0; i < h->len; i++) {
            hash ^= p[i];
            hash *= 1099511628211ULL; /* FNV prime */
        }
    }
    return (int64_t)hash;
}

int tur_string_starts_with(void *s, void *prefix) {
    tur_string_hdr *hs = (tur_string_hdr *)s, *hp = (tur_string_hdr *)prefix;
    int64_t ls = hs ? hs->len : 0, lp = hp ? hp->len : 0;
    if (lp > ls) return 0;
    if (lp == 0) return 1;
    return memcmp(str_bytes(hs), str_bytes(hp), (size_t)lp) == 0 ? 1 : 0;
}

int tur_string_ends_with(void *s, void *suffix) {
    tur_string_hdr *hs = (tur_string_hdr *)s, *hf = (tur_string_hdr *)suffix;
    int64_t ls = hs ? hs->len : 0, lf = hf ? hf->len : 0;
    if (lf > ls) return 0;
    if (lf == 0) return 1;
    return memcmp(str_bytes(hs) + (ls - lf), str_bytes(hf), (size_t)lf) == 0 ? 1
                                                                             : 0;
}

int tur_string_contains(void *s, void *needle) {
    tur_string_hdr *hs = (tur_string_hdr *)s, *hn = (tur_string_hdr *)needle;
    int64_t ls = hs ? hs->len : 0, ln = hn ? hn->len : 0;
    if (ln == 0) return 1;
    if (ln > ls) return 0;
    const char *bs = str_bytes(hs), *bn = str_bytes(hn);
    for (int64_t i = 0; i + ln <= ls; i++) {
        if (memcmp(bs + i, bn, (size_t)ln) == 0) return 1;
    }
    return 0;
}

/* ---- transforms ------------------------------------------------------- */

void *tur_string_concat(void *a, void *b) {
    tur_string_hdr *ha = (tur_string_hdr *)a, *hb = (tur_string_hdr *)b;
    int64_t la = ha ? ha->len : 0, lb = hb ? hb->len : 0;
    tur_string_hdr *h =
        (tur_string_hdr *)malloc(sizeof(tur_string_hdr) + (size_t)(la + lb) + 1);
    h->rc = 1;
    h->len = la + lb;
    char *dst = str_bytes(h);
    if (la) memcpy(dst, str_bytes(ha), (size_t)la);
    if (lb) memcpy(dst + la, str_bytes(hb), (size_t)lb);
    dst[la + lb] = '\0';
    return h;
}

void *tur_string_substring(void *s, int64_t start, int64_t len) {
    tur_string_hdr *h = (tur_string_hdr *)s;
    int64_t n = h ? h->len : 0;
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len < 0) len = 0;
    if (start + len > n) len = n - start;
    return tur_string_from_bytes(h ? str_bytes(h) + start : NULL, len);
}

void *tur_string_to_upper(void *s) {
    tur_string_hdr *h = (tur_string_hdr *)s;
    int64_t n = h ? h->len : 0;
    void *r = tur_string_from_bytes(h ? str_bytes(h) : NULL, n);
    char *b = str_bytes((tur_string_hdr *)r);
    for (int64_t i = 0; i < n; i++)
        if (b[i] >= 'a' && b[i] <= 'z') b[i] = (char)(b[i] - 'a' + 'A');
    return r;
}

void *tur_string_to_lower(void *s) {
    tur_string_hdr *h = (tur_string_hdr *)s;
    int64_t n = h ? h->len : 0;
    void *r = tur_string_from_bytes(h ? str_bytes(h) : NULL, n);
    char *b = str_bytes((tur_string_hdr *)r);
    for (int64_t i = 0; i < n; i++)
        if (b[i] >= 'A' && b[i] <= 'Z') b[i] = (char)(b[i] - 'A' + 'a');
    return r;
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

void *tur_string_trim(void *s) {
    tur_string_hdr *h = (tur_string_hdr *)s;
    int64_t n = h ? h->len : 0;
    const char *b = h ? str_bytes(h) : "";
    int64_t lo = 0, hi = n;
    while (lo < hi && is_ws(b[lo])) lo++;
    while (hi > lo && is_ws(b[hi - 1])) hi--;
    return tur_string_from_bytes(b + lo, hi - lo);
}

/* ---- StringBuilder ---------------------------------------------------- */

typedef struct {
    int64_t len;
    int64_t cap;
    char *data;
} tur_sb;

void *tur_sb_new(void) {
    tur_sb *b = (tur_sb *)malloc(sizeof(tur_sb));
    b->len = 0;
    b->cap = 16;
    b->data = (char *)malloc((size_t)b->cap);
    return b;
}

static void sb_reserve(tur_sb *b, int64_t extra) {
    int64_t need = b->len + extra;
    if (need <= b->cap) return;
    while (b->cap < need) b->cap *= 2;
    b->data = (char *)realloc(b->data, (size_t)b->cap);
}

void tur_sb_push_cstr(void *bp, const char *s) {
    if (!bp || !s) return;
    tur_sb *b = (tur_sb *)bp;
    int64_t n = (int64_t)strlen(s);
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
}

void tur_sb_push_string(void *bp, void *s) {
    if (!bp || !s) return;
    tur_sb *b = (tur_sb *)bp;
    int64_t n = tur_string_len(s);
    sb_reserve(b, n);
    memcpy(b->data + b->len, tur_string_cstr(s), (size_t)n);
    b->len += n;
}

void tur_sb_push_byte(void *bp, int64_t c) {
    if (!bp) return;
    tur_sb *b = (tur_sb *)bp;
    sb_reserve(b, 1);
    b->data[b->len++] = (char)(unsigned char)c;
}

int64_t tur_sb_len(void *bp) { return bp ? ((tur_sb *)bp)->len : 0; }

void *tur_sb_finish(void *bp) {
    if (!bp) return tur_string_from_bytes(NULL, 0);
    tur_sb *b = (tur_sb *)bp;
    void *s = tur_string_from_bytes(b->data, b->len);
    free(b->data);
    free(b);
    return s;
}

/* ---- owned-map-key bridge --------------------------------------------- */

/* Forward-declare only the two hamt entry points we need, so this TU stays
 * decoupled from hamt.h.  Both are linked whenever a String is used as a map
 * key (which requires a Map, which pulls in hamt.c). */
void *tur_hamt_box_key(const void *src, size_t n);
bool tur_hamt_box_key_eq(int64_t a, int64_t b);

void *tur_string_box_key(void *s) {
    tur_string_hdr *h = (tur_string_hdr *)s;
    return tur_hamt_box_key(h ? str_bytes(h) : NULL,
                            (size_t)(h ? h->len : 0));
}

int64_t tur_string_key_eq_addr(void) {
    return (int64_t)(intptr_t)&tur_hamt_box_key_eq;
}
