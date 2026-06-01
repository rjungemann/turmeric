/* symbols.h -- runtime interning for first-class symbols (:Sym, -Xsymbols).
 *
 * SYM5 (runtime-symbols-plan): a process-global, mutex-guarded table that backs
 * the optional dynamic constructor `str->sym`.  Literal `:foo` records are
 * static (emitted by codegen) and self-register at startup via a constructor;
 * `tur_sym_intern` returns the pre-registered static record for a known name
 * (so `(eq? :foo (str->sym "foo"))` holds) and allocates a fresh,
 * process-lifetime record for any name with no static literal. */
#ifndef TUR_RUNTIME_SYMBOLS_H
#define TUR_RUNTIME_SYMBOLS_H

#include <stdint.h>

/* Layout MUST match the records emitted by sym_codegen_emit (emit_core.c). */
#ifndef TUR_SYM_DEFINED
#define TUR_SYM_DEFINED 1
struct __tur_sym {
    uint64_t hash;   /* precomputed xxHash64 of name */
    uint32_t len;    /* byte length, excluding NUL */
    uint32_t _pad;
    char     name[]; /* NUL-terminated UTF-8 */
};
#endif

/* Register a static record under its name (first registration wins).  Called by
 * the codegen-emitted startup constructor for every literal `:foo` record so a
 * later tur_sym_intern("foo") returns the same pointer. */
void tur_sym_register(const struct __tur_sym *rec);

/* Return a stable, process-lifetime record for the given name.  If a record
 * with this name was already registered/interned, returns it; otherwise
 * allocates a new one.  Thread-safe. */
const struct __tur_sym *tur_sym_intern(const char *s, uint32_t len);

#endif
