/* source_literal.h - embed a filesystem path in generated Turmeric source.
 *
 * Several entry points build a form by string concatenation and hand it to
 * turi_eval:
 *
 *     snprintf(buf, sizeof buf, "(load \"%s\")", path);
 *
 * That is generated SOURCE, so `path` lands inside a string literal and the
 * Turmeric lexer reads it with escape processing on.  A POSIX path survives
 * because it has no backslashes.  A Windows path does not:
 *
 *     <eval>:1:12: error: unknown string escape '\U'
 *     1 | (load "C:\Users\roger\AppData\Local\Temp\turdist/stdlib/macros.tur")
 *
 * Every escape in the path errors, the form never evaluates, and the whole
 * stdlib prelude silently fails to load -- so `when` is an unknown name and the
 * REPL is unusable.  This is why an INSTALLED tur.exe could not run its own
 * prelude while the in-tree build appeared fine: in the dev tree the stdlib root
 * resolves to the literal "stdlib" (resolve_stdlib_root's last-resort fallback),
 * which has no backslashes to trip over.
 *
 * Escaping is the fix rather than rewriting separators to '/'.  Windows accepts
 * '/' in its file APIs, so a rewrite would also work today, but it is a fix for
 * the character that happens to be common rather than for the bug: any path
 * containing a backslash or a double quote is mis-parsed, on every platform.
 * A '"' in a directory name is legal on POSIX and would inject a string
 * terminator into the generated form.
 *
 * Not Windows-specific and not conditionally compiled: the hazard is the same
 * everywhere, and a POSIX path simply has nothing to escape.
 */

#ifndef TUR_SOURCE_LITERAL_H
#define TUR_SOURCE_LITERAL_H

#include <stddef.h>

/* Write `in` into `out` with '\' and '"' escaped, so the result can be pasted
 * between the quotes of a generated string literal.  Returns false (leaving
 * `out` NUL-terminated but truncated) when the escaped form does not fit --
 * escaping can double the length, so a caller sizing a buffer from strlen(in)
 * must allow for that.  Truncation is reported rather than silently accepted:
 * a truncated path would produce a valid-looking form naming the wrong file. */
static inline int tur_source_literal_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return 0;
    for (const char *p = in; *p; p++) {
        if (*p == '\\' || *p == '"') {
            if (o + 2 >= cap) { out[o] = '\0'; return 0; }
            out[o++] = '\\';
        } else if (o + 1 >= cap) {
            out[o] = '\0';
            return 0;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return 1;
}

#endif /* TUR_SOURCE_LITERAL_H */
