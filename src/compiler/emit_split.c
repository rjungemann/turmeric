/* emit_split.c -- r7rs-programs-compile-slowly: the file-scope state transform
 * for a program built as a library unit and a client unit (emit_split.h).
 *
 * This reads C the emitter wrote, not arbitrary C: the runtime preamble,
 * hoisted inline-C and stdlib file-scope blocks.  So it is a scanner, not a
 * parser.  It skips comments, string and character literals and preprocessor
 * lines, tracks bracket depth, and cuts the file-scope text into
 * declarations (ending at a depth-0 `;`) and function definitions (a depth-0
 * `{` after a `)` with no `=` before it).  Each one is then classified by its
 * leading keywords and whether its declarator is a function.
 *
 * What it gets wrong fails loudly rather than silently.  A variable read as a
 * prototype is left alone and exists once per unit -- which
 * tests/check-r7rs-prelude-split.sh reports as a local data symbol both
 * objects define.  A prototype read as a variable becomes an `extern` of a
 * function in the client unit or loses its `static` in the library unit,
 * which `cc` rejects or `ld` reports as a duplicate. */

#include "emit_split.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- lexical skipping -------------------------------------------------- */

static bool tok_is(const char *s, size_t at, size_t len, const char *w) {
    return strlen(w) == len && memcmp(s + at, w, len) == 0;
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/* Past a comment, string or character literal starting at s[i]; i itself if
 * none starts there. */
static size_t skip_lexeme(const char *s, size_t n, size_t i) {
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
        return i + 1 < n ? i + 2 : n;
    }
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        while (i < n && s[i] != '\n') i++;
        return i;
    }
    if (s[i] == '"' || s[i] == '\'') {
        char q = s[i++];
        while (i < n && s[i] != q) {
            if (s[i] == '\\' && i + 1 < n) i++;
            if (s[i] == '\n') break;   /* unterminated: stop at the line end */
            i++;
        }
        return i < n ? i + 1 : n;
    }
    return i;
}

/* True when s[i] is the first non-blank character of its line. */
static bool at_line_start(const char *s, size_t i) {
    while (i > 0) {
        char c = s[i - 1];
        if (c == '\n') return true;
        if (c != ' ' && c != '\t') return false;
        i--;
    }
    return true;
}

/* Past a preprocessor line starting at s[i] ('#'), backslash continuations
 * included, and past its newline. */
static size_t skip_pp_line(const char *s, size_t n, size_t i) {
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') { i += 2; continue; }
        if (s[i] == '/' && i + 1 < n && (s[i + 1] == '*' || s[i + 1] == '/')) {
            size_t j = skip_lexeme(s, n, i);
            /* a block comment can run past the line end; a // one stops at it */
            i = j;
            continue;
        }
        if (s[i] == '\n') return i + 1;
        i++;
    }
    return n;
}

/* ---- cutting the file scope into declarations -------------------------- */

typedef enum { CHUNK_DECL, CHUNK_FUNC, CHUNK_OPEN } ChunkKind;

static size_t declarator_paren(const char *s, size_t a, size_t b, bool stop_at_eq);

/* From s[start], the end (exclusive) of the file-scope item that starts
 * there, and what it is.  `body` receives the offset of a function
 * definition's opening brace. */
static size_t chunk_end(const char *s, size_t n, size_t start,
                        ChunkKind *kind, size_t *body) {
    int paren = 0, brack = 0, brace = 0;
    bool saw_eq = false;
    size_t last_sig = (size_t)-1;   /* last significant char at depth 0 */
    size_t i = start;
    while (i < n) {
        size_t j = skip_lexeme(s, n, i);
        if (j != i) { i = j; continue; }
        char c = s[i];
        if (c == '#' && at_line_start(s, i)) { i = skip_pp_line(s, n, i); continue; }
        switch (c) {
        case '(': paren++; break;
        case ')': paren--; break;
        case '[': brack++; break;
        case ']': brack--; break;
        case '{':
            if (paren == 0 && brack == 0 && brace == 0 && !saw_eq &&
                last_sig != (size_t)-1 && s[last_sig] == ')' &&
                declarator_paren(s, start, i, false) != (size_t)-1) {
                /* A function definition: the body runs to the matching brace. */
                int d = 0;
                size_t k = i;
                while (k < n) {
                    size_t m = skip_lexeme(s, n, k);
                    if (m != k) { k = m; continue; }
                    if (s[k] == '#' && at_line_start(s, k)) { k = skip_pp_line(s, n, k); continue; }
                    if (s[k] == '{') d++;
                    else if (s[k] == '}' && --d == 0) break;
                    k++;
                }
                *kind = CHUNK_FUNC;
                *body = i;
                return k < n ? k + 1 : n;
            }
            brace++;
            break;
        case '}': brace--; break;
        case '=':
            if (paren == 0 && brack == 0 && brace == 0 &&
                !(i + 1 < n && s[i + 1] == '=') &&
                !(i > 0 && strchr("=<>!", s[i - 1])))
                saw_eq = true;
            break;
        case ';':
            if (paren == 0 && brack == 0 && brace == 0) {
                *kind = CHUNK_DECL;
                return i + 1;
            }
            break;
        default: break;
        }
        if (!isspace((unsigned char)c) && paren == 0 && brack == 0 && brace == 0)
            last_sig = i;
        else if (c == ')' && paren == 0 && brack == 0 && brace == 0)
            last_sig = i;
        i++;
    }
    *kind = CHUNK_OPEN;
    return n;
}

/* ---- reading a declaration's head -------------------------------------- */

/* The identifier-or-punctuation tokens of s[a..b) at bracket depth 0, with
 * `__attribute__((...))` groups, comments and preprocessor lines skipped.
 * Calls `fn(tok_start, tok_len, ud)` for each; stops when it returns false. */
typedef bool (*TokFn)(const char *s, size_t at, size_t len, void *ud);

static void each_head_token(const char *s, size_t a, size_t b, TokFn fn, void *ud) {
    size_t i = a;
    while (i < b) {
        size_t j = skip_lexeme(s, b, i);
        if (j != i) { i = j; continue; }
        if (s[i] == '#' && at_line_start(s, i)) { i = skip_pp_line(s, b, i); continue; }
        if (isspace((unsigned char)s[i])) { i++; continue; }
        if (is_ident_char(s[i])) {
            size_t k = i;
            while (k < b && is_ident_char(s[k])) k++;
            size_t len = k - i;
            if ((len == 13 && memcmp(s + i, "__attribute__", 13) == 0) ||
                (len == 10 && memcmp(s + i, "__declspec", 10) == 0)) {
                /* skip the parenthesized group that follows */
                while (k < b && isspace((unsigned char)s[k])) k++;
                if (k < b && s[k] == '(') {
                    int d = 0;
                    while (k < b) {
                        if (s[k] == '(') d++;
                        else if (s[k] == ')' && --d == 0) { k++; break; }
                        k++;
                    }
                }
                i = k;
                continue;
            }
            if (!fn(s, i, len, ud)) return;
            i = k;
            continue;
        }
        if (!fn(s, i, 1, ud)) return;
        i++;
    }
}

typedef struct {
    bool is_static, is_extern, is_typedef, is_inline, is_const, is_aggregate_kw;
    bool first;
} HeadKw;


static bool head_kw_tok(const char *s, size_t at, size_t len, void *ud) {
    HeadKw *h = (HeadKw *)ud;
    if (tok_is(s, at, len, "static")) h->is_static = true;
    else if (tok_is(s, at, len, "extern")) h->is_extern = true;
    else if (tok_is(s, at, len, "typedef")) h->is_typedef = true;
    else if (tok_is(s, at, len, "inline") || tok_is(s, at, len, "__inline__") ||
             tok_is(s, at, len, "__inline")) h->is_inline = true;
    else if (tok_is(s, at, len, "const")) h->is_const = true;
    else if (h->first && (tok_is(s, at, len, "struct") || tok_is(s, at, len, "union") ||
                          tok_is(s, at, len, "enum"))) h->is_aggregate_kw = true;
    if (!tok_is(s, at, len, "__extension__")) h->first = false;
    /* Stop at the first punctuation that can only follow the specifiers. */
    if (len == 1 && (s[at] == '=' || s[at] == '(' || s[at] == '[' || s[at] == '{'))
        return false;
    return true;
}

/* Is `s[k..e)` the name of a parenthesized group that is not a declarator:
 * an attribute, `sizeof`, `__typeof__`, `_Alignas` and the like? */
static bool is_group_word(const char *s, size_t k, size_t e) {
    static const char *const words[] = {
        "__attribute__", "__declspec", "sizeof", "__typeof__", "typeof",
        "_Alignas", "_Atomic", "__alignof__", "_Alignof", NULL
    };
    for (int w = 0; words[w]; w++)
        if (tok_is(s, k, e - k, words[w])) return true;
    return false;
}

/* The first `(` at bracket depth 0 in s[a..b) that opens a declarator's
 * parameter list or a parenthesized declarator -- one not following a group
 * word -- or (size_t)-1.  With `stop_at_eq`, a depth-0 `=` ends the search:
 * past it is an initializer. */
static size_t declarator_paren(const char *s, size_t a, size_t b, bool stop_at_eq) {
    int brack = 0, brace = 0;
    size_t i = a;
    while (i < b) {
        size_t j = skip_lexeme(s, b, i);
        if (j != i) { i = j; continue; }
        if (s[i] == '#' && at_line_start(s, i)) { i = skip_pp_line(s, b, i); continue; }
        char c = s[i];
        if (c == '[') brack++;
        else if (c == ']') brack--;
        else if (c == '{') brace++;
        else if (c == '}') brace--;
        else if (c == '=' && brack == 0 && brace == 0 && stop_at_eq) return (size_t)-1;
        else if (c == '(' && brack == 0 && brace == 0) {
            size_t k = i;
            while (k > a && isspace((unsigned char)s[k - 1])) k--;
            size_t e = k;
            while (k > a && is_ident_char(s[k - 1])) k--;
            if (!(e > k && is_group_word(s, k, e))) return i;
            int d = 0;   /* skip the group */
            while (i < b) {
                if (s[i] == '(') d++;
                else if (s[i] == ')' && --d == 0) break;
                i++;
            }
        }
        i++;
    }
    return (size_t)-1;
}

/* Does the declaration s[a..b) declare a function (a prototype), as opposed
 * to a variable?  Its first declarator `(` before any `=` is a parameter list
 * unless a `*` follows it -- `(*name)(...)` is a function-pointer variable. */
static bool decl_is_function(const char *s, size_t a, size_t b) {
    size_t i = declarator_paren(s, a, b, true);
    if (i == (size_t)-1) return false;
    size_t m = i + 1;
    while (m < b && isspace((unsigned char)s[m])) m++;
    return !(m < b && s[m] == '*');
}

/* Is the static variable declaration s[a..b) read-only data -- a `const`
 * object rather than a pointer to const?  Read-only data needs no sharing: a
 * copy per unit reads the same. */
static bool decl_is_readonly(const char *s, size_t a, size_t b, const HeadKw *h) {
    if (!h->is_const) return false;
    /* the head, up to the initializer */
    size_t end = b;
    int brack = 0;
    for (size_t i = a; i < b; i++) {
        size_t j = skip_lexeme(s, b, i);
        if (j != i) { i = j - 1; continue; }
        if (s[i] == '[') brack++;
        else if (s[i] == ']') brack--;
        else if (s[i] == '=' && brack == 0) { end = i; break; }
    }
    size_t star = (size_t)-1;
    for (size_t i = a; i < end; i++) {
        size_t j = skip_lexeme(s, end, i);
        if (j != i) { i = j - 1; continue; }
        if (s[i] == '*') star = i;
    }
    if (star == (size_t)-1) return true;   /* `static const T x` */
    size_t k = star + 1;
    while (k < end && isspace((unsigned char)s[k])) k++;
    return k + 5 <= end && memcmp(s + k, "const", 5) == 0 &&
           (k + 5 == end || !is_ident_char(s[k + 5]));
}

/* ---- rewriting ---------------------------------------------------------- */

/* Copy s[a..b) to out without its first standalone `static` keyword. */
static void put_without_static(Buf *out, const char *s, size_t a, size_t b) {
    size_t i = a;
    while (i < b) {
        size_t j = skip_lexeme(s, b, i);
        if (j != i) { buf_write(out, s + i, j - i); i = j; continue; }
        if (is_ident_char(s[i])) {
            size_t k = i;
            while (k < b && is_ident_char(s[k])) k++;
            if (k - i == 6 && memcmp(s + i, "static", 6) == 0 &&
                (i == a || !is_ident_char(s[i - 1]))) {
                while (k < b && (s[k] == ' ' || s[k] == '\t')) k++;
                buf_write(out, s + k, b - k);
                return;
            }
            buf_write(out, s + i, k - i);
            i = k;
            continue;
        }
        buf_putc(out, s[i]);
        i++;
    }
}

/* Write s[a..b) (a declaration ending in `;`) as an `extern` declaration:
 * `static` dropped and every declarator's initializer removed. */
static void put_as_extern(Buf *out, const char *s, size_t a, size_t b) {
    Buf tmp;
    buf_init(&tmp);
    put_without_static(&tmp, s, a, b);
    buf_puts(out, "extern ");
    const char *t = tmp.data;
    size_t n = tmp.len;
    int paren = 0, brack = 0, brace = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = skip_lexeme(t, n, i);
        if (j != i) { buf_write(out, t + i, j - i); i = j; continue; }
        char c = t[i];
        if (c == '#' && at_line_start(t, i)) {
            size_t e = skip_pp_line(t, n, i);
            buf_write(out, t + i, e - i);
            i = e;
            continue;
        }
        if (c == '=' && paren == 0 && brack == 0 && brace == 0) {
            /* drop through the next depth-0 `,` or `;` (kept) */
            while (out->len > 0 && (out->data[out->len - 1] == ' ' ||
                                    out->data[out->len - 1] == '\t'))
                out->len--;
            i++;
            int p = 0, k = 0, r = 0;
            while (i < n) {
                size_t m = skip_lexeme(t, n, i);
                if (m != i) { i = m; continue; }
                if (t[i] == '#' && at_line_start(t, i)) { i = skip_pp_line(t, n, i); continue; }
                char d = t[i];
                if (d == '(') p++;
                else if (d == ')') p--;
                else if (d == '[') k++;
                else if (d == ']') k--;
                else if (d == '{') r++;
                else if (d == '}') r--;
                else if ((d == ',' || d == ';') && p == 0 && k == 0 && r == 0) break;
                i++;
            }
            continue;
        }
        if (c == '(') paren++;
        else if (c == ')') paren--;
        else if (c == '[') brack++;
        else if (c == ']') brack--;
        else if (c == '{') brace++;
        else if (c == '}') brace--;
        buf_putc(out, c);
        i++;
    }
    buf_free(&tmp);
}

/* Write a function definition's header s[a..body) with every `constructor` or
 * `destructor` attribute renamed `unused`, then its body s[body..b). */
static void put_without_ctor(Buf *out, const char *s, size_t a, size_t body, size_t b) {
    size_t i = a;
    while (i < body) {
        size_t j = skip_lexeme(s, body, i);
        if (j != i) { buf_write(out, s + i, j - i); i = j; continue; }
        if (is_ident_char(s[i])) {
            size_t k = i;
            while (k < body && is_ident_char(s[k])) k++;
            if (tok_is(s, i, k - i, "constructor") || tok_is(s, i, k - i, "destructor") ||
                tok_is(s, i, k - i, "__constructor__") || tok_is(s, i, k - i, "__destructor__")) {
                buf_puts(out, "unused");
                /* a priority argument: `constructor(101)` */
                if (k < body && s[k] == '(') {
                    while (k < body && s[k] != ')') k++;
                    if (k < body) k++;
                }
            } else {
                buf_write(out, s + i, k - i);
            }
            i = k;
            continue;
        }
        buf_putc(out, s[i]);
        i++;
    }
    buf_write(out, s + body, b - body);
}

static bool head_has_ctor(const char *s, size_t a, size_t body) {
    for (size_t i = a; i < body; i++) {
        size_t j = skip_lexeme(s, body, i);
        if (j != i) { i = j - 1; continue; }
        if (is_ident_char(s[i]) && (i == a || !is_ident_char(s[i - 1]))) {
            size_t k = i;
            while (k < body && is_ident_char(s[k])) k++;
            if (tok_is(s, i, k - i, "constructor") || tok_is(s, i, k - i, "destructor") ||
                tok_is(s, i, k - i, "__constructor__") || tok_is(s, i, k - i, "__destructor__"))
                return true;
            i = k - 1;
        }
    }
    return false;
}

/* ---- the library unit's exported names --------------------------------- */

/* Every name the library unit gives external linkage, so emit_split_rename
 * can move all of them to a private prefix.  A hash set of owned strings. */
static char   **g_exp;
static uint32_t g_exp_cap, g_exp_n;

static uint32_t exp_hash(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    return h;
}

static bool exp_has(const char *p, size_t n) {
    if (!g_exp_cap) return false;
    for (uint32_t i = exp_hash(p, n) & (g_exp_cap - 1);; i = (i + 1) & (g_exp_cap - 1)) {
        const char *e = g_exp[i];
        if (!e) return false;
        if (strlen(e) == n && memcmp(e, p, n) == 0) return true;
    }
}

static void exp_add(const char *p, size_t n) {
    if (n == 0 || exp_has(p, n)) return;
    if ((g_exp_n + 1) * 2 > g_exp_cap) {
        uint32_t oc = g_exp_cap, nc = oc ? oc * 2 : 1024;
        char **old = g_exp;
        g_exp = (char **)calloc(nc, sizeof(char *));
        if (!g_exp) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_exp_cap = nc;
        g_exp_n = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i]) { exp_add(old[i], strlen(old[i])); free(old[i]); }
        free(old);
    }
    char *c = (char *)malloc(n + 1);
    if (!c) { fprintf(stderr, "tur: oom\n"); abort(); }
    memcpy(c, p, n);
    c[n] = '\0';
    uint32_t i = exp_hash(p, n) & (g_exp_cap - 1);
    while (g_exp[i]) i = (i + 1) & (g_exp_cap - 1);
    g_exp[i] = c;
    g_exp_n++;
}

void emit_split_note_export(const char *name) {
    if (name) exp_add(name, strlen(name));
}

void emit_split_exports_clear(void) {
    for (uint32_t i = 0; i < g_exp_cap; i++) free(g_exp[i]);
    free(g_exp);
    g_exp = NULL;
    g_exp_cap = g_exp_n = 0;
}

/* The names the variable declaration s[a..b) declares: one per declarator,
 * `(*name)` for a function pointer, else its last identifier outside any
 * brackets and attribute groups, before the initializer. */
static void note_declared_names(const char *s, size_t a, size_t b) {
    size_t i = a;
    while (i < b) {
        /* one declarator: up to a depth-0 `,` or `;`; its head ends at `=` */
        int paren = 0, brack = 0, brace = 0;
        size_t head_end = (size_t)-1, seg_end = b;
        size_t j = i;
        while (j < b) {
            size_t m = skip_lexeme(s, b, j);
            if (m != j) { j = m; continue; }
            char c = s[j];
            if (c == '(') paren++;
            else if (c == ')') paren--;
            else if (c == '[') brack++;
            else if (c == ']') brack--;
            else if (c == '{') brace++;
            else if (c == '}') brace--;
            else if (paren == 0 && brack == 0 && brace == 0) {
                if (c == '=' && head_end == (size_t)-1) head_end = j;
                else if (c == ',' || c == ';') { seg_end = j; break; }
            }
            j++;
        }
        if (head_end == (size_t)-1) head_end = seg_end;
        /* `(*name)` */
        size_t fp = declarator_paren(s, i, head_end, false);
        size_t name_s = 0, name_e = 0;
        if (fp != (size_t)-1) {
            size_t k = fp + 1;
            while (k < head_end && (isspace((unsigned char)s[k]) || s[k] == '*')) k++;
            size_t st = k;
            while (k < head_end && is_ident_char(s[k])) k++;
            /* `volatile` and friends may sit between the `*` and the name */
            while (k < head_end && (tok_is(s, st, k - st, "volatile") ||
                                    tok_is(s, st, k - st, "const"))) {
                while (k < head_end && isspace((unsigned char)s[k])) k++;
                st = k;
                while (k < head_end && is_ident_char(s[k])) k++;
            }
            name_s = st; name_e = k;
        } else {
            int bd = 0, bb = 0;
            for (size_t k = i; k < head_end;) {
                size_t m = skip_lexeme(s, head_end, k);
                if (m != k) { k = m; continue; }
                char c = s[k];
                if (c == '[') { bb++; k++; continue; }
                if (c == ']') { bb--; k++; continue; }
                if (c == '{') { bd++; k++; continue; }
                if (c == '}') { bd--; k++; continue; }
                if (c == '(' ) {   /* an attribute group after the name */
                    int d = 0;
                    while (k < head_end) {
                        if (s[k] == '(') d++;
                        else if (s[k] == ')' && --d == 0) { k++; break; }
                        k++;
                    }
                    continue;
                }
                if (is_ident_char(c) && bd == 0 && bb == 0) {
                    size_t st = k;
                    while (k < head_end && is_ident_char(s[k])) k++;
                    if (!is_group_word(s, st, k) && !isdigit((unsigned char)s[st])) {
                        name_s = st; name_e = k;
                    }
                    continue;
                }
                k++;
            }
        }
        if (name_e > name_s) exp_add(s + name_s, name_e - name_s);
        if (seg_end >= b || s[seg_end] == ';') break;
        i = seg_end + 1;
    }
}

/* ---- the rename ----------------------------------------------------------- */

void emit_split_rename(const char *s, size_t n, Buf *out) {
    size_t i = 0;
    while (i < n) {
        /* String and character literals keep their text; comments too. */
        size_t j = skip_lexeme(s, n, i);
        if (j != i) { buf_write(out, s + i, j - i); i = j; continue; }
        if (is_ident_char(s[i]) && !isdigit((unsigned char)s[i]) &&
            (i == 0 || !is_ident_char(s[i - 1]))) {
            size_t k = i;
            while (k < n && is_ident_char(s[k])) k++;
            if (exp_has(s + i, k - i)) buf_puts(out, EMIT_SPLIT_PREFIX);
            buf_write(out, s + i, k - i);
            i = k;
            continue;
        }
        /* a number's digits are not an identifier's start */
        if (isdigit((unsigned char)s[i])) {
            size_t k = i;
            while (k < n && (is_ident_char(s[k]) || s[k] == '.')) k++;
            buf_write(out, s + i, k - i);
            i = k;
            continue;
        }
        buf_putc(out, s[i]);
        i++;
    }
}

void emit_split_state(const char *s, size_t n, EmitSplitMode mode, Buf *out) {
    if (!s || n == 0) return;
    if (mode == EMIT_SPLIT_NONE) { buf_write(out, s, n); return; }
    size_t i = 0;
    while (i < n) {
        /* between items: blanks, comments, preprocessor lines */
        if (isspace((unsigned char)s[i])) { buf_putc(out, s[i]); i++; continue; }
        size_t j = skip_lexeme(s, n, i);
        if (j != i) { buf_write(out, s + i, j - i); i = j; continue; }
        if (s[i] == '#' && at_line_start(s, i)) {
            size_t e = skip_pp_line(s, n, i);
            buf_write(out, s + i, e - i);
            i = e;
            continue;
        }
        if (s[i] == ';') { buf_putc(out, ';'); i++; continue; }   /* stray */

        ChunkKind kind;
        size_t body = 0;
        size_t e = chunk_end(s, n, i, &kind, &body);
        HeadKw h = { .first = true };
        each_head_token(s, i, kind == CHUNK_FUNC ? body : e, head_kw_tok, &h);

        if (kind == CHUNK_FUNC) {
            if (mode == EMIT_SPLIT_CLIENT && !h.is_static && !h.is_inline) {
                /* One definition, in the library unit; the client declares it. */
                size_t k = body;
                while (k > i && isspace((unsigned char)s[k - 1])) k--;
                buf_write(out, s + i, k - i);
                buf_puts(out, ";");
            } else if (mode == EMIT_SPLIT_CLIENT && head_has_ctor(s, i, body)) {
                put_without_ctor(out, s, i, body, e);
            } else {
                buf_write(out, s + i, e - i);
            }
            i = e;
            continue;
        }
        if (kind == CHUNK_OPEN) { buf_write(out, s + i, e - i); i = e; continue; }

        /* A declaration. */
        bool pass = h.is_typedef || h.is_extern || decl_is_function(s, i, e);
        if (!pass && h.is_aggregate_kw && !h.is_static) {
            /* `struct X { ... };` -- a type, no declarator after the body */
            size_t k = e - 1;   /* the `;` */
            while (k > i && isspace((unsigned char)s[k - 1])) k--;
            if (k > i && s[k - 1] == '}') pass = true;
            /* `struct X;` */
            size_t toks = 0;
            for (size_t m = i; m < e; m++)
                if (is_ident_char(s[m]) && (m == i || !is_ident_char(s[m - 1]))) toks++;
            if (toks <= 2) pass = true;
        }
        if (!pass && h.is_static && decl_is_readonly(s, i, e, &h)) pass = true;
        if (pass) { buf_write(out, s + i, e - i); i = e; continue; }

        if (mode == EMIT_SPLIT_LIB) {
            /* Only what the split made external moves to the private prefix:
             * a name that was already external in one unit keeps it, and
             * with it whatever the runtime archives bind it to there. */
            if (h.is_static) {
                note_declared_names(s, i, e);
                put_without_static(out, s, i, e);
            } else {
                buf_write(out, s + i, e - i);
            }
        } else {
            put_as_extern(out, s, i, e);
        }
        i = e;
    }
}
