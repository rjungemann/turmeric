#ifndef TUR_MONO_SPECS_H
#define TUR_MONO_SPECS_H

#include <stddef.h>
#include <stdio.h>

/* VBM1/VBM2 (docs/upcoming/van-laarhoven-monomorphization-plan.md): the by-value
 * HKT monomorphization spec registry.
 *
 * VBM1: elab_poly_call registers one ABSTRACT specialization key per van
 * Laarhoven lens call site whose resolved constraint pins the functor `f` to a
 * WIDE by-value aggregate (a `:copy` struct / flat-product ADT wider than the
 * one-int64 carrier).  Each abstract key is `(enclosing_fn, callee, functor_name,
 * focus_ty, whole_ty)`.  At that pin the callee is the ABSTRACT lens param `l`
 * (e.g. inside `set-px` the call is `(l g s)`), so the CONCRETE lens FnDef Path B
 * must specialize is not yet known -- that cross-procedural collapse is plan
 * OQ #1/#2, deferred by VBM1 to VBM2.
 *
 * VBM2 (this slice): `mono_specs_resolve_program` walks the elaborated program
 * and joins each abstract spec to the CONCRETE lens passed at every top-level
 * call of its enclosing fn (e.g. `(set-px point-x ...)` in `main` resolves the
 * abstract `l` to the concrete lens `point-x`), recording a CONCRETE spec keyed
 * `(lens_fn, functor_name, focus_ty, whole_ty)` -- at most one emitted body per
 * concrete key.  Still registry-only: codegen keeps the Path A carrier-box path;
 * the per-spec by-value body emit (VBM2b) is tracked separately (see the plan and
 * docs/reported/vbm2-byvalue-lens-body-emit.md -- it depends on closing the
 * documented "M7-by-value gap" the MB2.5 carve-out at emit_module.c leaves open).
 * `--dump-mono-specs` prints both the abstract keys and the resolved concrete
 * keys so the keying can be reviewed by eye.
 *
 * Registration is guarded on `g_opt_vl_wide_mono`; the dump on
 * `g_dump_mono_specs`. */

/* VBM1: register (dedup by content hash) one ABSTRACT lens spec key.  NULL
 * fields are recorded as "?".  Strings are snapshotted, so callers may pass
 * transient buffers. */
void   mono_spec_register(const char *enclosing_fn, const char *callee,
                          const char *functor_name, const char *focus_ty,
                          const char *whole_ty, const char *tyvar,
                          const void *functor_ty);

/* Number of distinct ABSTRACT keys registered this compile. */
size_t mono_spec_count(void);

/* VBM2: resolve every abstract spec to its concrete lens by scanning the
 * elaborated program `prog` (an `Expr *` program node) for calls of each
 * abstract spec's enclosing fn, and register a concrete `(lens_fn, functor,
 * focus, whole)` key for the concrete lens found in the abstract lens param's
 * arg slot.  Idempotent + dedup by content hash.  No-op when the registry is
 * empty. */
void   mono_specs_resolve_program(const void *prog);

/* Number of distinct CONCRETE (resolved) keys registered this compile. */
size_t mono_spec_concrete_count(void);

/* Read the i-th resolved concrete key's fields (any out-ptr may be NULL).
 * Returns the deterministic content hash (0 if `i` is out of range).  The
 * returned strings point into the registry and stay valid until
 * `mono_specs_reset`.  VBM2b's emit pass iterates these to emit one
 * `<lens>__mono_<hash>` by-value body per key. */
unsigned long long mono_spec_concrete_get(size_t i, const char **lens,
                                          const char **functor,
                                          const char **focus,
                                          const char **whole);

/* Read the i-th concrete key's emit handles: the resolved lens `const FnDef *`
 * (may be NULL if the concrete lens defn wasn't found), the HKT tyvar name
 * (`f`), and the concrete functor ctor `const Type *` (NULL if unresolved).
 * Returns the content hash (0 if out of range). */
unsigned long long mono_spec_concrete_emit_info(size_t i, const void **lens_fn,
                                                const char **tyvar,
                                                const void **functor_ty);

/* Print the registry: one `mono-spec-abstract <hash> fn=.. lens-param=.. f=..
 * focus=.. whole=..` line per abstract key, then one `mono-spec <hash>
 * lens=.. f=.. focus=.. whole=..` line per resolved concrete key (each section
 * preceded by a `;`-comment count header). */
void   mono_specs_dump(FILE *out);

/* Clear the registry (between compiles in a long-lived process). */
void   mono_specs_reset(void);

#endif /* TUR_MONO_SPECS_H */
