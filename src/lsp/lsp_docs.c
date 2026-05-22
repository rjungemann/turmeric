#include "lsp_docs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Open-addressing hash map: URI string -> LspDoc* */
#define DOCS_INIT_CAP 16

typedef struct DocsSlot {
    LspDoc *doc;  /* NULL = empty */
} DocsSlot;

static DocsSlot *slots_   = NULL;
static size_t    capacity_ = 0;
static size_t    count_    = 0;

/* FNV-1a 32-bit */
static uint32_t fnv1a(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void rehash(void) {
    size_t new_cap = capacity_ ? capacity_ * 2 : DOCS_INIT_CAP;
    DocsSlot *new_slots = calloc(new_cap, sizeof(DocsSlot));
    for (size_t i = 0; i < capacity_; i++) {
        if (!slots_[i].doc) continue;
        LspDoc *d = slots_[i].doc;
        uint32_t h = fnv1a(d->uri, strlen(d->uri)) % (uint32_t)new_cap;
        while (new_slots[h].doc) h = (h + 1) % (uint32_t)new_cap;
        new_slots[h].doc = d;
    }
    free(slots_);
    slots_ = new_slots;
    capacity_ = new_cap;
}

static size_t find_slot(const char *uri, size_t uri_len) {
    if (!capacity_) return (size_t)-1;
    uint32_t h = fnv1a(uri, uri_len) % (uint32_t)capacity_;
    for (size_t probe = 0; probe < capacity_; probe++) {
        size_t idx = (h + probe) % capacity_;
        if (!slots_[idx].doc) return (size_t)-1;
        if (strncmp(slots_[idx].doc->uri, uri, uri_len) == 0 &&
            slots_[idx].doc->uri[uri_len] == '\0')
            return idx;
    }
    return (size_t)-1;
}

void lsp_docs_init(void) {
    slots_    = NULL;
    capacity_ = 0;
    count_    = 0;
}

void lsp_docs_free(void) {
    for (size_t i = 0; i < capacity_; i++) {
        if (!slots_[i].doc) continue;
        free(slots_[i].doc->uri);
        free(slots_[i].doc->path);
        free(slots_[i].doc->text);
        free(slots_[i].doc);
    }
    free(slots_);
    slots_ = NULL;
    capacity_ = count_ = 0;
}

char *lsp_uri_to_path(const char *uri, char *dest, size_t dest_cap) {
    /* Strip "file://" prefix */
    const char *src = uri;
    if (strncmp(src, "file://", 7) == 0) src += 7;
    /* URL-decode %XX sequences */
    size_t di = 0;
    for (; *src && di < dest_cap - 1; src++) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dest[di++] = (char)strtol(hex, NULL, 16);
            src += 2;
        } else {
            dest[di++] = *src;
        }
    }
    dest[di] = '\0';
    return dest;
}

static LspDoc *make_doc(const char *uri, size_t uri_len,
                        const char *text, size_t text_len) {
    LspDoc *d = calloc(1, sizeof(LspDoc));
    d->uri  = malloc(uri_len + 1);
    memcpy(d->uri, uri, uri_len);
    d->uri[uri_len] = '\0';

    d->path = malloc(uri_len + 1);
    lsp_uri_to_path(d->uri, d->path, uri_len + 1);

    d->text = malloc(text_len + 1);
    memcpy(d->text, text, text_len);
    d->text[text_len] = '\0';
    d->text_len = text_len;
    return d;
}

LspDoc *lsp_doc_open(const char *uri, size_t uri_len,
                     const char *text, size_t text_len) {
    /* Replace if already open */
    size_t existing = find_slot(uri, uri_len);
    if (existing != (size_t)-1) {
        LspDoc *d = slots_[existing].doc;
        free(d->text);
        d->text = malloc(text_len + 1);
        memcpy(d->text, text, text_len);
        d->text[text_len] = '\0';
        d->text_len = text_len;
        return d;
    }

    if (!capacity_ || (count_ + 1) * 4 >= capacity_ * 3)
        rehash();

    LspDoc *d = make_doc(uri, uri_len, text, text_len);
    uint32_t h = fnv1a(uri, uri_len) % (uint32_t)capacity_;
    while (slots_[h].doc) h = (h + 1) % (uint32_t)capacity_;
    slots_[h].doc = d;
    count_++;
    return d;
}

void lsp_doc_change(const char *uri, size_t uri_len,
                    const char *text, size_t text_len) {
    size_t idx = find_slot(uri, uri_len);
    if (idx == (size_t)-1) {
        lsp_doc_open(uri, uri_len, text, text_len);
        return;
    }
    LspDoc *d = slots_[idx].doc;
    free(d->text);
    d->text = malloc(text_len + 1);
    memcpy(d->text, text, text_len);
    d->text[text_len] = '\0';
    d->text_len = text_len;
}

void lsp_doc_close(const char *uri, size_t uri_len) {
    size_t idx = find_slot(uri, uri_len);
    if (idx == (size_t)-1) return;
    LspDoc *d = slots_[idx].doc;
    free(d->uri); free(d->path); free(d->text); free(d);
    slots_[idx].doc = NULL;
    count_--;
}

LspDoc *lsp_doc_get(const char *uri, size_t uri_len) {
    size_t idx = find_slot(uri, uri_len);
    return idx != (size_t)-1 ? slots_[idx].doc : NULL;
}
