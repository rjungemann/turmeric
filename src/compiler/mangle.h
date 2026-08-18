/* mangle.h -- shared C-identifier mangling for Turmeric binding names.
 *
 * A single source of truth for turning a Turmeric binding name (which may
 * contain operator/sigil characters such as `>`, `<`, `?`, `!`, `=`) into a
 * valid, *injective* C identifier component.  Both the elaborator
 * (elab_mangle_binding_name) and the emitter (raw_name_for_binding /
 * c_name_for_binding) route their per-name character mangling through here so
 * a function's declaration, definition, and every call site agree on one
 * spelling.
 *
 * Why this exists: the old per-site loops collapsed *every* non-id char to a
 * single `_`, so `>>>` and `<<<` both mangled to `___` -- a C redefinition.
 * See docs/archive/history/stdlib-type-erasure-cleanup-plan.md (section A3).
 *
 * Scheme (injective, self-delimiting):
 *   - [A-Za-z0-9]       -> itself
 *   - '_' (literal)     -> "_un"      (so a lone '_' always introduces an escape)
 *   - '-' (hyphen)      -> "_hy"
 *   - '/' (slash)       -> "_sl"      (when a raw '/' reaches the mangler; the
 *                                      module layer pre-splits paths into the
 *                                      "__" structural separator instead)
 *   - other sigils      -> '_' + a fixed two-letter mnemonic
 *                          (e.g. '>' -> "_gt", '<' -> "_lt", '?' -> "_qu")
 *   - any other byte    -> "_x" + two uppercase hex digits (escape hatch)
 *
 * The mnemonic + hex-escape forms make distinct operators map to distinct C
 * identifiers, so symmetric operator pairs (`>>>` / `<<<`, `+++` / `***`, ...)
 * can coexist in one module.
 *
 * Why every separator is now encoded: the old scheme folded '-', '/', and a
 * literal '_' all to a single '_', so distinct Turmeric names collided in C
 * (`foo-bar` and `foo_bar` both became `foo_bar` -- a hard redefinition error).
 * Encoding '_' as "_un" closes that collision and makes the encoding injective.
 *
 * Self-delimiting + invertible. Two invariants make a demangler sound:
 *   1. A single '_' always introduces an escape (data underscores are "_un").
 *   2. "__" is exclusively structural -- because a literal '_' is "_un", two
 *      adjacent underscores can never arise from data, so the module layer is
 *      free to use "__" as the `geom/vector` -> `geom__vector__` boundary.
 * tur_demangle (below) implements the inverse: copy alnum; on "__" emit '/';
 * on a lone '_' read 'x'+two hex digits, or two mnemonic letters.
 */
#ifndef TUR_COMPILER_MANGLE_H
#define TUR_COMPILER_MANGLE_H

#include <stddef.h>
#include <stdint.h>

/* Upper bound on the mangled length (excluding NUL) of a `src_len`-byte source
 * name component. The worst case per byte is the hex escape "_xHH" = 4 chars. */
size_t tur_mangle_bound(size_t src_len);

/* True if name[0..len) is already a valid C identifier body (every byte in
 * [A-Za-z0-9_]). Such a name needs no mangling. The emitter uses this to leave
 * compiler-synthesized C identifiers (e.g. `__fn_5`, `__inst_*`) verbatim
 * instead of re-encoding their literal '_' as "_un". */
int tur_name_is_c_identifier(const char *name, size_t len);

/* Append the mangled form of name[0..len) into dst starting at *pk, advancing
 * *pk past the written bytes. Does NOT write a NUL terminator. The caller must
 * ensure dst has at least *pk + tur_mangle_bound(len) bytes of capacity. */
void tur_mangle_append(char *dst, size_t *pk, const char *name, size_t len);

/* Legacy (non-injective) fold, used only for function-LOCAL names (parameters
 * and locals) -- not linker-visible, so they cannot collide across the program,
 * and inline-C bodies reference them by this stable `-`/`/` -> `_`, underscore-
 * passthrough convention. Globals use the injective tur_mangle_append above.
 *   - [A-Za-z0-9_] -> itself
 *   - '-' '/'      -> '_'
 *   - other sigils -> '_' + two-letter mnemonic (so `>>>`/`<<<` still differ)
 *   - any other    -> "_xHH"
 * Same 4x worst-case bound as tur_mangle_append, so tur_mangle_bound applies. */
void tur_mangle_legacy_append(char *dst, size_t *pk, const char *name, size_t len);

/* Mangle a NUL-terminated `name` into the fixed buffer `out` (capacity `cap`,
 * including the NUL), writing a NUL terminator and truncating safely if the
 * mangled form would not fit. This is the convenience wrapper the typeclass
 * dict/instance emitters use so a method's instance-function name, its dict
 * struct field, and every dispatch site agree on one spelling -- letting sigil
 * method pairs like `>>>` / `<<<` coexist instead of colliding on `___`. */
void tur_mangle_ident(const char *name, char *out, size_t cap);

/* Inverse of tur_mangle_ident: decode a mangled C identifier back to its
 * original Turmeric source spelling, writing a NUL-terminated result into `out`
 * (capacity `cap`, including the NUL). "__" decodes to the structural separator
 * '/'. Returns the number of bytes written (excluding the NUL), or 0 if the
 * input contains a malformed escape (a lone '_' not followed by a valid
 * mnemonic or hex body). The decoded form is never longer than the input, so
 * `cap >= strlen(mangled) + 1` always suffices. */
size_t tur_demangle(const char *mangled, char *out, size_t cap);

/* True if `name[0..len)` is a libc/POSIX function symbol that a C toolchain's
 * system headers declare in the emitted translation unit, so a user top-level
 * `defn` lowered to a bare `static int64_t <name>(...)` would be a
 * redeclaration conflict.  The emitter (raw_name_for_binding) consults this to
 * mangle ONLY a bare, non-module-prefixed global whose spelling collides --
 * module-qualified names (`geom__read`) and extern-c bindings (which name the
 * real libc symbol on purpose) are unaffected.  See
 * docs/archive/codegen-user-defn-collides-with-libc-pipe2.md. */
int tur_name_collides_libc(const char *name, size_t len);

/* True if `name[0..len)` is a C reserved word (C89 through C23, plus the two
 * near-universal extensions `asm`/`typeof`).  Such a name survives every
 * mangling scheme unchanged -- it is pure `[A-Za-z0-9_]`, so both the injective
 * scheme and the legacy fold pass it through byte for byte -- and then lands in
 * the emitted C as `static int64_t double(int64_t);`, `f(int64_t double)`, or
 * `int64_t int;`.  Each is a syntax error, and one bad declaration derails the
 * rest of the translation unit, so the user sees hundreds of cc errors with no
 * relation to their source.  Callers prefix a hit with TUR_NAME_GUARD_PREFIX,
 * exactly as they do for tur_name_collides_libc.  See
 * docs/archive/history/c-keyword-function-names-not-mangled.md. */
int tur_name_is_c_keyword(const char *name, size_t len);

/* Prefix applied to a source name whose C spelling would collide with something
 * the C side already owns -- a libc/POSIX symbol the system headers declare, or
 * a C reserved word.  A user name that literally starts with `tur_u_` cannot
 * alias the guarded form under the injective scheme (a literal '_' encodes as
 * "_un", so `tur_u_double` mangles to `tur_unu_undouble`). */
#define TUR_NAME_GUARD_PREFIX     "tur_u_"
#define TUR_NAME_GUARD_PREFIX_LEN 6

/* Length of <name> in a `__TUR_CNAME_<name>__` splice.
 *
 * `buf`/`len` are the whole inline-C body; `name_start` indexes the byte just
 * after the 12-character `__TUR_CNAME_` prefix.  Returns the length of <name>,
 * or 0 when the splice is malformed (the caller then leaves the text alone).
 *
 * <name> may itself begin with `__`.  That is the established convention for a
 * private helper right across stdlib and the spices (`__cons`, `__gzbuf`,
 * `__db-prepare-raw`, `__functor_*`) -- which makes it exactly the name an
 * author reaches for when calling a sibling from inline C.  Scanning naively
 * for "the first __" terminates at offset zero for such a name, so the splice
 * is left unexpanded and the emitted C either calls a nonexistent
 * `__TUR_CNAME_...` function or, when the name contains a hyphen, is tokenized
 * as a subtraction -- producing a `cc` error that names a fragment appearing
 * nowhere in the source (`alloc__` for `__secret-alloc`).
 *
 * So a leading run of underscores is skipped when a real name character
 * follows it.  A run with no name character after it is left alone, keeping
 * the old no-match behaviour for a degenerate splice.  An interior `__` still
 * terminates the name; that ambiguity is inherent to the delimiter and
 * unchanged here. */
static inline uint32_t tur_cname_name_len(const char *buf, uint32_t len,
                                          uint32_t name_start) {
    uint32_t j = name_start;
    uint32_t lead = j;
    while (lead < len && buf[lead] == '_') lead++;
    if (lead < len && ((buf[lead] >= 'a' && buf[lead] <= 'z') ||
                       (buf[lead] >= 'A' && buf[lead] <= 'Z') ||
                       (buf[lead] >= '0' && buf[lead] <= '9') ||
                        buf[lead] == '-'))
        j = lead;
    while (j + 1 < len && !(buf[j] == '_' && buf[j + 1] == '_')) j++;
    if (j + 1 < len && buf[j] == '_' && buf[j + 1] == '_' && j > name_start)
        return j - name_start;
    return 0;
}

#endif /* TUR_COMPILER_MANGLE_H */
