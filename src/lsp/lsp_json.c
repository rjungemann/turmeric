#include "lsp_json.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Skip a JSON value (string, number, object, array, bool, null).
 * Returns pointer to one past the value, or NULL on parse error. */
static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (!*p) return NULL;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') p++;
            p++;
        }
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        p++;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\') p++; p++; }
                if (*p) p++;
            } else if (*p == open) {
                depth++; p++;
            } else if (*p == close) {
                depth--; p++;
            } else {
                p++;
            }
        }
        return p;
    }
    /* number, bool, null -- scan to next delimiter */
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        p++;
    return p;
}

/* Find the raw value for `key` at the top level of a JSON object in `json`.
 * Returns pointer to the start of the value; sets *out_len.
 * Does not recurse into nested objects at depth > 1. */
const char *lsp_json_raw(const char *json, const char *key, size_t *out_len) {
    if (!json || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = skip_ws(json);
    if (*p != '{') return NULL;
    p++;

    while (1) {
        p = skip_ws(p);
        if (!*p || *p == '}') break;

        /* Expect a string key */
        if (*p != '"') break;
        p++;
        const char *kstart = p;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        size_t this_klen = (size_t)(p - kstart);
        if (*p) p++; /* skip closing '"' */

        p = skip_ws(p);
        if (*p != ':') break;
        p++;
        p = skip_ws(p);

        const char *vstart = p;
        const char *vend = skip_value(p);
        if (!vend) break;

        if (this_klen == klen && strncmp(kstart, key, klen) == 0) {
            if (out_len) *out_len = (size_t)(vend - vstart);
            return vstart;
        }

        p = vend;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    return NULL;
}

const char *lsp_json_str(const char *json, const char *key, size_t *out_len) {
    size_t vlen;
    const char *v = lsp_json_raw(json, key, &vlen);
    if (!v) return NULL;
    v = skip_ws(v);
    if (*v != '"') return NULL;
    v++;
    /* Find the closing quote (handle escapes) */
    const char *start = v;
    while (*v && *v != '"') { if (*v == '\\') v++; if (*v) v++; }
    if (out_len) *out_len = (size_t)(v - start);
    return start;
}

int64_t lsp_json_int(const char *json, const char *key) {
    size_t vlen;
    const char *v = lsp_json_raw(json, key, &vlen);
    if (!v) return -1;
    v = skip_ws(v);
    if (*v == '-' || (*v >= '0' && *v <= '9'))
        return (int64_t)atoll(v);
    return -1;
}

int lsp_json_str_copy(const char *json, const char *key,
                      char *dest, size_t dest_cap) {
    size_t vlen;
    const char *v = lsp_json_str(json, key, &vlen);
    if (!v) return -1;
    size_t copy = vlen < dest_cap - 1 ? vlen : dest_cap - 1;
    /* Unescape common sequences */
    size_t di = 0;
    for (size_t si = 0; si < copy && di < dest_cap - 1; si++) {
        if (v[si] == '\\' && si + 1 < vlen) {
            si++;
            switch (v[si]) {
                case '"':  dest[di++] = '"';  break;
                case '\\': dest[di++] = '\\'; break;
                case '/':  dest[di++] = '/';  break;
                case 'n':  dest[di++] = '\n'; break;
                case 'r':  dest[di++] = '\r'; break;
                case 't':  dest[di++] = '\t'; break;
                default:   dest[di++] = v[si]; break;
            }
        } else {
            dest[di++] = v[si];
        }
    }
    dest[di] = '\0';
    return 0;
}
