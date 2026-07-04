#ifndef TUR_MONO_SPECS_H
#define TUR_MONO_SPECS_H

#include <stddef.h>
#include <stdio.h>

/* VBM1 (docs/upcoming/van-laarhoven-monomorphization-plan.md): the by-value HKT
 * monomorphization spec registry.
 *
 * elab_poly_call registers one specialization key per van Laarhoven lens call
 * site whose resolved constraint pins the functor `f` to a WIDE by-value
 * aggregate (a `:copy` struct / flat-product ADT wider than the one-int64
 * carrier).  Each key is `(enclosing_fn, callee, functor_name, focus_ty,
 * whole_ty)` -- the shape Path B (VBM2) will emit at most one specialized lens
 * body per.  VBM1 only POPULATES the registry; codegen still takes the Path A
 * carrier-box path.  `--dump-mono-specs` prints the keys so the keying can be
 * reviewed by eye before emit is wired.
 *
 * Registration is guarded on `g_opt_vl_wide_mono`; the dump on
 * `g_dump_mono_specs`. */

/* Register (dedup by content hash) one lens spec key.  NULL fields are recorded
 * as "?".  Strings are snapshotted, so callers may pass transient buffers. */
void   mono_spec_register(const char *enclosing_fn, const char *callee,
                          const char *functor_name, const char *focus_ty,
                          const char *whole_ty);

/* Number of distinct keys registered this compile. */
size_t mono_spec_count(void);

/* Print one `mono-spec <hash> fn=.. callee=.. f=.. focus=.. whole=..` line per
 * key (preceded by a `;`-comment count header). */
void   mono_specs_dump(FILE *out);

/* Clear the registry (between compiles in a long-lived process). */
void   mono_specs_reset(void);

#endif /* TUR_MONO_SPECS_H */
