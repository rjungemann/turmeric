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

/* Curated denylist of libc / POSIX function symbols that a modern C toolchain's
 * system headers (unistd.h, stdio.h, stdlib.h, string.h, time.h, sys/socket.h,
 * ...) declare in a translation unit -- so a user top-level `defn` lowered to a
 * bare `static int64_t <name>(...)` of the SAME spelling is a redeclaration
 * conflict ("static declaration of 'X' follows non-static declaration").  The
 * defect is toolchain-dependent (e.g. macOS SDKs added `pipe2` to <unistd.h>,
 * turning a long-compiling fixture red) and platform-independent in principle,
 * so `raw_name_for_binding` mangles ONLY a bare (non-module-prefixed) global
 * whose spelling lands here.  Kept sorted for bsearch; extend it as real
 * collisions surface rather than pre-emptively (over-broad entries would mangle
 * a user's function for no reason).  See
 * docs/archive/codegen-user-defn-collides-with-libc-pipe2.md. */
static int sorted_strcmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

int tur_name_collides_libc(const char *name, size_t len) {
    if (!name || len == 0) return 0;
    /* Callers pass a NUL-terminated Symbol whose text is exactly `len` bytes;
     * a slice with an interior NUL or extra trailing bytes is not a real
     * identifier and cannot match a libc symbol -- reject it so the strcmp
     * below compares the whole spelling. */
    if (strlen(name) != len) return 0;
    static const char *const libc_names[] = {
        "abort", "abs", "accept", "alarm", "atoi", "atol", "atoll",
        "bcopy", "bind", "bsearch", "bzero",
        "calloc", "chdir", "chmod", "clock", "close", "connect", "ctime",
        "difftime", "dup", "dup2",
        "exit",
        "fclose", "fcntl", "fdopen", "fflush", "fgets", "fopen", "fork",
        "fprintf", "fputs", "fread", "free", "fscanf", "fseek", "fstat",
        "ftell", "fwrite",
        "getcwd", "getenv", "getgid", "getline", "getpid", "getppid",
        "getsockopt", "getuid", "gmtime",
        "index", "kill",
        "labs", "link", "listen", "localtime", "lockf", "longjmp", "lseek",
        "lstat",
        "malloc", "memccpy", "memchr", "memcmp", "memcpy", "memmove", "memset",
        "mkdir", "mktime",
        "nanosleep",
        "open",
        "pause", "pclose", "pipe", "pipe2", "poll", "popen", "printf",
        "putenv", "puts",
        "qsort",
        "raise", "rand", "random", "read", "realloc", "recv", "recvfrom",
        "remove", "rename", "rewind", "rindex", "rmdir",
        "scanf", "select", "send", "sendto", "setenv", "setgid", "setjmp",
        "setsockopt", "setuid", "shutdown", "signal", "sleep", "snprintf",
        "socket", "socketpair", "sprintf", "srand", "srandom", "sscanf",
        "stat", "strcat", "strchr", "strcmp", "strcpy", "strdup", "strlen",
        "strncat", "strncmp", "strncpy", "strrchr", "strstr", "strtod",
        "strtok", "strtol", "strtoul", "symlink", "system",
        "time", "times", "tmpfile",
        "unlink", "unsetenv", "usleep",
        "wait", "waitpid", "write",
    };
    return bsearch(name, libc_names,
                   sizeof(libc_names) / sizeof(libc_names[0]),
                   sizeof(libc_names[0]), sorted_strcmp) != NULL;
}

int tur_name_is_c_keyword(const char *name, size_t len) {
    if (!name || len == 0) return 0;
    /* Same slice guard as tur_name_collides_libc: an interior NUL or trailing
     * bytes mean this is not a whole identifier, so it cannot be a keyword. */
    if (strlen(name) != len) return 0;
    /* Every C reserved word, C89 through C23, plus `asm` / `typeof` (which
     * every toolchain we target accepts).  Sorted for bsearch under strcmp,
     * where the leading '_' (0x5F) of the C99/C11 spellings sorts before every
     * lowercase letter, so the `_X` block comes first.  Unlike the libc list
     * this one is complete by construction rather than grown on demand: the
     * keyword set is fixed by the standard, and a missing entry is the same
     * unreadable cc cascade the guard exists to prevent. */
    static const char *const c_keywords[] = {
        "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex",
        "_Decimal128", "_Decimal32", "_Decimal64", "_Generic", "_Imaginary",
        "_Noreturn", "_Static_assert", "_Thread_local",
        "alignas", "alignof", "asm", "auto",
        "bool", "break",
        "case", "char", "const", "constexpr", "continue",
        "default", "do", "double",
        "else", "enum", "extern",
        "false", "float", "for",
        "goto",
        "if", "inline", "int",
        "long",
        "nullptr",
        "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "static_assert", "struct",
        "switch",
        "thread_local", "true", "typedef", "typeof", "typeof_unqual",
        "union", "unsigned",
        "void", "volatile",
        "while",
    };
    return bsearch(name, c_keywords,
                   sizeof(c_keywords) / sizeof(c_keywords[0]),
                   sizeof(c_keywords[0]), sorted_strcmp) != NULL;
}
