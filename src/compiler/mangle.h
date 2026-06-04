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
 * Scheme (stable, reversible for sigils):
 *   - [A-Za-z0-9_]      -> itself
 *   - '-' and '/'       -> '_'        (structural separators: kebab + namespace
 *                                      convention; kept as legacy `_` so the
 *                                      hundreds of `foo-bar` / `mod/fn` names
 *                                      keep their existing C spelling)
 *   - other sigils      -> '_' + a fixed two-letter mnemonic
 *                          (e.g. '>' -> "_gt", '<' -> "_lt", '?' -> "_qu")
 *   - any other byte    -> "_x" + two uppercase hex digits (escape hatch)
 *
 * The mnemonic + hex-escape forms make distinct operators map to distinct C
 * identifiers, so symmetric operator pairs (`>>>` / `<<<`, `+++` / `***`, ...)
 * can coexist in one module.
 *
 * No general demangler is provided. Because '-', '/', and a literal '_' all
 * map to '_' (legacy folding, kept so the hundreds of existing kebab/namespaced
 * names keep their C spelling), the encoding is not self-delimiting: a run like
 * `foo_bar` is indistinguishable from `foo` + sigil + `r`, so any inverse would
 * mis-decode ordinary identifiers. A sound inverse would require re-encoding
 * '-'/'/'/'_' too, which the project deliberately avoids (see plan A3). It is
 * also unnecessary: Turmeric diagnostics report the original *source* symbol
 * name (from the AST/span), never the mangled C identifier, so users always see
 * `>>>` rather than `_gt_gt_gt` without any demangling step.
 */
#ifndef TUR_COMPILER_MANGLE_H
#define TUR_COMPILER_MANGLE_H

#include <stddef.h>

/* Upper bound on the mangled length (excluding NUL) of a `src_len`-byte source
 * name component. The worst case per byte is the hex escape "_xHH" = 4 chars. */
size_t tur_mangle_bound(size_t src_len);

/* Append the mangled form of name[0..len) into dst starting at *pk, advancing
 * *pk past the written bytes. Does NOT write a NUL terminator. The caller must
 * ensure dst has at least *pk + tur_mangle_bound(len) bytes of capacity. */
void tur_mangle_append(char *dst, size_t *pk, const char *name, size_t len);

#endif /* TUR_COMPILER_MANGLE_H */
