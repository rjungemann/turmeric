/* docstrings.c -- read stdlib/docstrings.tur's doc table directly from C.
 * See docstrings.h.
 *
 * The file's shape is a contract we own on both sides: tools/gendocs.py's
 * emit_docstrings_tur writes
 *
 *     static const struct { const char *key; const char *val; } entries[] = {
 *       {"name", "docstring with \n escapes"},
 *       ...
 *     };
 *
 * so the scanner below only has to understand C string literals -- it does not
 * care about line layout, ordering, or how many entries there are.  It stops at
 * the end of that array, which keeps the separate doc-verified? table (a flat
 * list of bare names) from being mistaken for entries.
 *
 * The generated file is ~500 KB, so it is parsed once into a sorted table
 * rather than rescanned per lookup: the playground doc panel looks a name up
 * on every hover.
 */
#include "turi/docstrings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    char *val;
} DocEntry;

static char     *g_path    = NULL;   /* the file the cache below was read from */
static DocEntry *g_entries = NULL;
static size_t    g_n       = 0;

/* Read one C string literal starting at `p` (which must point at the opening
 * quote), unescaping into `out`.  Returns a pointer just past the closing
 * quote, or NULL if the literal is unterminated.  `out` may be NULL to scan
 * without copying; `cap` then does not matter. */
static const char *scan_c_string(const char *p, char *out, size_t cap) {
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '0':  c = '\0'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                default:   c = e;    break;
            }
        }
        if (out && i + 1 < cap) out[i++] = c;
    }
    if (*p != '"') return NULL;
    if (out && cap) out[i] = '\0';
    return p + 1;
}

/* Copy the literal at `p` (ending just before `end`) into a fresh string. */
static char *dup_c_string(const char *p, const char *end) {
    size_t room = (size_t)(end - p) + 1;   /* unescaping only shrinks */
    char *s = (char *)malloc(room);
    if (s) scan_c_string(p, s, room);
    return s;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return NULL; }
    buf[size] = '\0';
    return buf;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const DocEntry *)a)->key, ((const DocEntry *)b)->key);
}

static void cache_clear(void) {
    for (size_t i = 0; i < g_n; i++) {
        free(g_entries[i].key);
        free(g_entries[i].val);
    }
    free(g_entries);
    free(g_path);
    g_entries = NULL;
    g_n       = 0;
    g_path    = NULL;
}

/* Parse `path` into the cache.  A missing or malformed file leaves an EMPTY
 * cache for that path, so a lookup against an absent stdlib is a cheap miss
 * rather than a re-read per call. */
static void cache_load(const char *path) {
    cache_clear();
    g_path = strdup(path);

    char *text = read_file(path);
    if (!text) return;

    const char *p = strstr(text, "entries[] = {");
    if (!p) { free(text); return; }
    p += strlen("entries[] = {");

    size_t cap = 0;

#define SKIP_WS_COMMA(q) \
    while (*(q) == ' ' || *(q) == '\t' || *(q) == '\n' || \
           *(q) == '\r' || *(q) == ',') (q)++

    for (;;) {
        SKIP_WS_COMMA(p);
        /* The array's own closing brace ends the table.  Each entry also ends
         * in `}`, which is consumed at the bottom of the loop -- leaving it
         * there would stop the scan after the first entry. */
        if (*p != '{') break;
        p++;
        SKIP_WS_COMMA(p);

        const char *key_start = p;
        const char *after_key = scan_c_string(p, NULL, 0);
        if (!after_key) break;
        p = after_key;
        SKIP_WS_COMMA(p);
        if (*p != '"') break;                 /* {NULL, NULL} sentinel, or malformed */

        const char *val_start = p;
        const char *after_val = scan_c_string(p, NULL, 0);
        if (!after_val) break;

        if (g_n == cap) {
            size_t ncap = cap ? cap * 2 : 1024;
            DocEntry *grown = (DocEntry *)realloc(g_entries, ncap * sizeof(DocEntry));
            if (!grown) break;
            g_entries = grown;
            cap = ncap;
        }
        g_entries[g_n].key = dup_c_string(key_start, after_key);
        g_entries[g_n].val = dup_c_string(val_start, after_val);
        if (!g_entries[g_n].key || !g_entries[g_n].val) {
            free(g_entries[g_n].key);
            free(g_entries[g_n].val);
            break;
        }
        g_n++;

        p = after_val;
        SKIP_WS_COMMA(p);
        if (*p == '}') p++;                   /* close this entry */
    }
#undef SKIP_WS_COMMA

    free(text);
    /* gendocs dedups keys (first definition wins), so a plain sort is enough
     * for bsearch to find the one entry per key. */
    if (g_n > 1) qsort(g_entries, g_n, sizeof(DocEntry), entry_cmp);
}

const char *tur_docstring_lookup_in(const char *path, const char *name) {
    if (!path || !name) return NULL;
    if (!g_path || strcmp(g_path, path) != 0) cache_load(path);
    if (g_n == 0) return NULL;
    DocEntry probe = { (char *)name, NULL };
    DocEntry *hit = (DocEntry *)bsearch(&probe, g_entries, g_n,
                                        sizeof(DocEntry), entry_cmp);
    return hit ? hit->val : NULL;
}

const char *tur_docstring_lookup(const char *name) {
    const char *sdir = getenv("TUR_STDLIB_DIR");
    if (!sdir || !*sdir) sdir = "stdlib";
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/docstrings.tur", sdir);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    return tur_docstring_lookup_in(path, name);
}
