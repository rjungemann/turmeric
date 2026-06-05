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
 * See docs/upcoming/stdlib-type-erasure-cleanup-plan.md (section A3).
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

#endif /* TUR_COMPILER_MANGLE_H */
