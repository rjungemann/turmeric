#ifndef TUR_MONO_SPECS_H
#define TUR_MONO_SPECS_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

/* VBM1/VBM2 (docs/archive/history/van-laarhoven-monomorphization-plan.md): the by-value
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
 * concrete key.  `--dump-mono-specs` prints both the abstract keys and the
 * resolved concrete keys so the keying can be reviewed by eye (gated on
 * `g_dump_mono_specs`).
 *
 * CURRENT STATE -- the registry is NO LONGER registry-only.  Registration is
 * unconditional since the vl-wide-mono graduation (2026-07-05), and the by-value
 * body emit is LIVE for both lens shapes:
 *
 *   - SIMPLE lenses (direct `fmap` tail) redirect to a by-value
 *     `<lens>__mono_<hash>` body -- no carrier box, no dict dispatch.  VBM2b
 *     (docs/archive/history/vbm2-byvalue-lens-body-emit.md) and VBM3 landed
 *     2026-07-04.
 *   - COMPOSED lenses thread `(f a)` by value end to end too, since CB1-CB5
 *     (docs/archive/history/van-laarhoven-composed-byvalue-plan.md, 2026-07-05).
 *
 * Path A (the carrier box) is now a narrow BACKSTOP, not the default: it handles
 * only the shapes CB2/CB3 cannot lower -- runtime-selected nested lens, non-lens
 * tail, missing composition adapter, nesting past the depth cap.  The
 * `has_composed_lens` poison in mono_specs.c survives only to select that
 * backstop (see composed_lens_byvalueable); `g_opt_vl_wide_mono` is retired. */

/* VBM1: register (dedup by content hash) one ABSTRACT lens spec key.  NULL
 * fields are recorded as "?".  Strings are snapshotted, so callers may pass
 * transient buffers. */
void   mono_spec_register(const char *enclosing_fn, const char *callee,
                          const char *functor_name, const char *focus_ty,
                          const char *whole_ty, const char *tyvar,
                          const void *functor_ty, const void *lensparam_binding);

/* Number of distinct ABSTRACT keys registered this compile. */
size_t mono_spec_count(void);

/* CM2: the i-th abstract spec's lens-param `Binding *` and enclosing consumer fn
 * name (NULL when out of range).  Drive the consumer-clone emit by iterating
 * `mono_spec_count()` and, for each binding whose `mono_spec_lens_set_count` is
 * >= 2, emitting one clone per resolved lens. */
const void *mono_spec_abstract_binding(size_t i);
const char *mono_spec_abstract_enclosing(size_t i);

/* CM2: the i-th abstract spec's HKT tyvar name (`f`) and concrete functor ctor
 * `const Type *` (NULL if unresolved), so the consumer-clone emit can bind
 * `f := <functor>` and resolve the shared `g` twin's `(f A)` result by value. */
const char *mono_spec_abstract_tyvar(size_t i, const void **functor_ty);

/* CM3: if a call to `callee_name` targets a consumer whose lens param has
 * consumer-mono clones (|set| >= 2), return that lens param's `Binding *` and set
 * `*lens_idx_out` to the lens arg's positional slot; NULL otherwise.  The call
 * emit reads the concrete lens from arg `lens_idx`, then `mono_spec_lens_clone_hash`
 * gives the `<lens>__mono_<hash>` hash that names the `<consumer>__lens_<hash>`
 * clone; a non-concrete (runtime-selected) lens has no clone and stays on Path A. */
const void *mono_spec_consumer_call_lookup(const char *callee_name,
                                           int *lens_idx_out);

/* CM3: the mono hash for concrete lens `lens_name` in the set resolved for
 * consumer lens param `lensparam_binding` (0 if the lens is not in the set). */
unsigned long long mono_spec_lens_clone_hash(const void *lensparam_binding,
                                             const char *lens_name);

/* CM4: true when the consumer owning `lensparam_binding` is GENERIC (its lens
 * focus/whole pin to type variables).  The by-value redirect must be suppressed
 * in such a consumer's generic carrier BASE body (emitted with no active ABI
 * spec) -- there the lens-result consumer (run-id / get-const) is the carrier
 * form; the concrete ABI-spec body still redirects. */
bool mono_spec_consumer_is_generic(const void *lensparam_binding);

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

/* CB2 (van-laarhoven-composed-byvalue-plan): the `<lens>__mono_<hash>` hash for
 * the concrete key matching lens defn `lens_name` (and functor ctor name
 * `functor_name`, when non-NULL).  Used by the direct-lens redirect inside a
 * composed lens's by-value mono body to target a NESTED lens's mono body
 * (`(point-x g p)` -> `point_x__mono`).  0 when no such concrete key exists. */
unsigned long long mono_spec_mono_hash_for_lens(const char *lens_name,
                                                const char *functor_name);

/* VBM3: if `lensparam_binding` is the abstract lens param `l` of a consumer
 * whose lens UNIQUELY resolves (|set| == 1) to a concrete `<lens>__mono_<hash>`
 * body, return true and hand back the lens name + hash so the poly-call emit can
 * redirect `(l g s)` to the by-value mono body.  False for the unresolved
 * (|set| == 0) and ambiguous (|set| >= 2) cases -- the latter is CM2/CM3's
 * consumer-clone case, not an in-place redirect. */
bool   mono_spec_redirect_for_binding(const void *lensparam_binding,
                                      const char **lens_name,
                                      unsigned long long *mono_hash);

/* CM1 (van-laarhoven-consumer-mono-plan): the FULL resolved concrete-lens set for
 * a consumer lens param.  `mono_specs_resolve_program` grows this set to a
 * fixpoint over the call graph (direct named-lens args + transitively forwarded
 * lens params).  |set| == 1 is the VBM3 unique-redirect case; |set| >= 2 is the
 * ambiguous case CM2 emits one consumer clone per element for; |set| == 0 is a
 * runtime-selected lens with no static concrete lens (Path A / CM4 residual).
 * These accessors expose the set to the CM2 clone emit and CM3 call rewrite. */

/* Number of distinct concrete lenses resolved for consumer lens param
 * `lensparam_binding` (0 for an unknown binding or an unresolved param). */
size_t mono_spec_lens_set_count(const void *lensparam_binding);

/* Read the i-th resolved lens for `lensparam_binding`: its defn name, resolved
 * `const FnDef *` (may be NULL if unfound), and `<lens>__mono_<hash>` emit key.
 * Any out-ptr may be NULL.  Returns false if `i` is out of range. */
bool   mono_spec_lens_set_get(const void *lensparam_binding, size_t i,
                              const char **lens_name, const void **lens_fn,
                              unsigned long long *mono_hash);

/* Copy up to `sites_cap` of the i-th resolved lens's call-site `const Expr *`
 * pointers into `sites_out` (the sites CM3 rewrites).  Returns the TOTAL site
 * count (which may exceed `sites_cap`; 0 for an out-of-range `i`). */
size_t mono_spec_lens_set_sites(const void *lensparam_binding, size_t i,
                                const void **sites_out, size_t sites_cap);

/* Print the registry: one `mono-spec-abstract <hash> fn=.. lens-param=.. f=..
 * focus=.. whole=..` line per abstract key, then one `mono-spec <hash>
 * lens=.. f=.. focus=.. whole=..` line per resolved concrete key (each section
 * preceded by a `;`-comment count header). */
void   mono_specs_dump(FILE *out);

/* Clear the registry (between compiles in a long-lived process). */
void   mono_specs_reset(void);

#endif /* TUR_MONO_SPECS_H */
