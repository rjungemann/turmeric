/* mangle.c -- shared C-identifier mangling for Turmeric binding names.
 * See mangle.h for the scheme and rationale (plan section A3). */
#include "mangle.h"

#include <stdlib.h>
#include <string.h>

/* Two-letter mnemonic for a sigil character, or NULL if `c` is not an encoded
 * sigil. '-' and '/' are deliberately absent: they fold to a plain '_' (see
 * tur_mangle_append) to preserve the legacy spelling of kebab/namespaced names.
 *
 * Mnemonics are exactly two lowercase letters and none begin with 'x' (which is
 * reserved for the "_xHH" hex escape), so the demangler can disambiguate. */
static const char *sigil_mnemonic(unsigned char c) {
    switch (c) {
        case '!':  return "ex"; /* exclamation */
        case '?':  return "qu"; /* question    */
        case '<':  return "lt";
        case '>':  return "gt";
        case '=':  return "eq";
        case '+':  return "pl"; /* plus    */
        case '*':  return "st"; /* star    */
        case '%':  return "pc"; /* percent */
        case '&':  return "am"; /* ampersand */
        case '|':  return "ba"; /* bar    */
        case '^':  return "cr"; /* caret  */
        case '~':  return "td"; /* tilde  */
        case '$':  return "dl"; /* dollar */
        case '@':  return "at";
        case '.':  return "do"; /* dot   */
        case ':':  return "cl"; /* colon */
        case '\'': return "qt"; /* quote/prime */
        case '#':  return "hs"; /* hash  */
        case ',':  return "cm"; /* comma */
        case ';':  return "sc"; /* semicolon */
        default:   return NULL;
    }
}

static int hex_digit(int v) { return v < 10 ? '0' + v : 'A' + (v - 10); }

size_t tur_mangle_bound(size_t src_len) {
    /* Worst case per source byte is the "_xHH" hex escape == 4 chars. */
    return src_len * 4 + 1;
}

void tur_mangle_append(char *dst, size_t *pk, const char *name, size_t len) {
    size_t k = *pk;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            dst[k++] = (char)c;
        } else if (c == '-' || c == '/') {
            /* Structural separators keep the legacy single-underscore spelling. */
            dst[k++] = '_';
        } else {
            const char *m = sigil_mnemonic(c);
            dst[k++] = '_';
            if (m) {
                dst[k++] = m[0];
                dst[k++] = m[1];
            } else {
                /* Escape hatch: any other byte as "_xHH". */
                dst[k++] = 'x';
                dst[k++] = (char)hex_digit((c >> 4) & 0xF);
                dst[k++] = (char)hex_digit(c & 0xF);
            }
        }
    }
    *pk = k;
}

void tur_mangle_ident(const char *name, char *out, size_t cap) {
    if (cap == 0) return;
    size_t k = 0;
    /* Mangle one source byte at a time so we can stop cleanly before the fixed
     * buffer overflows. tur_mangle_append treats each byte independently, so
     * per-byte and bulk mangling yield identical output. */
    for (const char *p = name; *p; p++) {
        char tmp[4];          /* worst case per byte is the "_xHH" escape == 4 */
        size_t tk = 0;
        tur_mangle_append(tmp, &tk, p, 1);
        if (k + tk >= cap) break;   /* leave room for the NUL terminator */
        memcpy(out + k, tmp, tk);
        k += tk;
    }
    out[k] = '\0';
}
