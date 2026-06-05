/* mangle.c -- shared C-identifier mangling for Turmeric binding names.
 * See mangle.h for the scheme and rationale (plan section A3 + the
 * reversible-name-mangling follow-up). */
#include "mangle.h"

#include <stdlib.h>
#include <string.h>

/* Two-letter mnemonic for a sigil/separator character, or NULL if `c` is not an
 * encoded byte. The three structural separators get mnemonics too:
 *   '_' -> "un", '-' -> "hy", '/' -> "sl"
 * so the encoding is injective and self-delimiting (a lone '_' in the output
 * always introduces an escape; "__" is reserved as a structural separator that
 * data can never produce).
 *
 * Mnemonics are exactly two lowercase letters and none begin with 'x' (which is
 * reserved for the "_xHH" hex escape), so the demangler can disambiguate. */
static const char *sigil_mnemonic(unsigned char c) {
    switch (c) {
        case '_':  return "un"; /* underscore (literal data underscore) */
        case '-':  return "hy"; /* hyphen     */
        case '/':  return "sl"; /* slash      */
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

/* Inverse of sigil_mnemonic: map a two-letter mnemonic back to its source byte,
 * or -1 if the pair is not a known mnemonic. */
static int mnemonic_byte(char a, char b) {
    switch ((unsigned char)a << 8 | (unsigned char)b) {
        case 'u' << 8 | 'n': return '_';
        case 'h' << 8 | 'y': return '-';
        case 's' << 8 | 'l': return '/';
        case 'e' << 8 | 'x': return '!';
        case 'q' << 8 | 'u': return '?';
        case 'l' << 8 | 't': return '<';
        case 'g' << 8 | 't': return '>';
        case 'e' << 8 | 'q': return '=';
        case 'p' << 8 | 'l': return '+';
        case 's' << 8 | 't': return '*';
        case 'p' << 8 | 'c': return '%';
        case 'a' << 8 | 'm': return '&';
        case 'b' << 8 | 'a': return '|';
        case 'c' << 8 | 'r': return '^';
        case 't' << 8 | 'd': return '~';
        case 'd' << 8 | 'l': return '$';
        case 'a' << 8 | 't': return '@';
        case 'd' << 8 | 'o': return '.';
        case 'c' << 8 | 'l': return ':';
        case 'q' << 8 | 't': return '\'';
        case 'h' << 8 | 's': return '#';
        case 'c' << 8 | 'm': return ',';
        case 's' << 8 | 'c': return ';';
        default:             return -1;
    }
}

static int hex_digit(int v) { return v < 10 ? '0' + v : 'A' + (v - 10); }

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int tur_name_is_c_identifier(const char *name, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

size_t tur_mangle_bound(size_t src_len) {
    /* Worst case per source byte is the "_xHH" hex escape == 4 chars. */
    return src_len * 4 + 1;
}

void tur_mangle_append(char *dst, size_t *pk, const char *name, size_t len) {
    size_t k = *pk;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            /* Pure alphanumerics pass through unchanged. A literal '_' does NOT
             * pass through: it is encoded as "_un" so a lone '_' in the output
             * always introduces an escape (see mangle.h). */
            dst[k++] = (char)c;
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

void tur_mangle_legacy_append(char *dst, size_t *pk, const char *name,
                              size_t len) {
    size_t k = *pk;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            dst[k++] = (char)c;
        } else if (c == '-' || c == '/') {
            dst[k++] = '_';
        } else {
            const char *m = sigil_mnemonic(c);
            dst[k++] = '_';
            if (m && c != '_' && c != '-' && c != '/') {
                dst[k++] = m[0];
                dst[k++] = m[1];
            } else {
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

size_t tur_demangle(const char *mangled, char *out, size_t cap) {
    /* Scan left to right; copy alnum; on "__" emit the structural separator
     * '/'; on a lone '_' read the next byte: 'x' -> two hex digits -> byte,
     * otherwise two letters -> reverse-mnemonic lookup. Output never grows, so a
     * `cap` >= strlen(mangled)+1 always suffices. Returns the number of bytes
     * written (excluding the NUL), or 0 on a malformed escape. */
    if (cap == 0) return 0;
    size_t k = 0;
    const char *p = mangled;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c != '_') {
            if (k + 1 >= cap) break;
            out[k++] = (char)c;
            p++;
            continue;
        }
        /* c == '_': an escape, a structural separator, or unterminated. */
        if (p[1] == '_') {
            /* Structural separator "__" -> '/'. */
            if (k + 1 >= cap) break;
            out[k++] = '/';
            p += 2;
            continue;
        }
        if (p[1] == 'x') {
            int hi = hex_value((unsigned char)p[2]);
            int lo = hex_value((unsigned char)p[3]);
            if (hi < 0 || lo < 0) { out[k] = '\0'; return 0; }
            if (k + 1 >= cap) break;
            out[k++] = (char)((hi << 4) | lo);
            p += 4;
            continue;
        }
        if (p[1] && p[2]) {
            int b = mnemonic_byte(p[1], p[2]);
            if (b < 0) { out[k] = '\0'; return 0; }
            if (k + 1 >= cap) break;
            out[k++] = (char)b;
            p += 3;
            continue;
        }
        /* Lone trailing '_' with no escape body: malformed. */
        out[k] = '\0';
        return 0;
    }
    out[k] = '\0';
    return k;
}
