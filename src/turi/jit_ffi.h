/* jit_ffi.h -- JIT-scoped dynamic FFI via c2mir (jit-ffi-c2mir-plan).
 *
 * The interpreter is the one consumer that genuinely needs call signatures
 * materialized at runtime; the JIT is exactly where it already lives.  This
 * header is the insulation layer between the two: tur_core TUs (ffi_thunk.c,
 * eval.c) consult a PROVIDER installed at startup by a JIT build, and stay
 * MIR-free themselves -- eighteen targets consume tur_core's objects
 * (including the WASM build) and none of them may grow a MIR link line.
 *
 * The provider synthesizes tiny C thunks with c2mir for signatures described
 * by a shape string (section 2.1 of the plan):
 *
 *     sig := <ret-class> ':' <arg-class>*
 *
 *     'i'  int64 register class (:int, :bool, :cstr, :ptr<T>, sized ints)
 *     'f'  double
 *     'F'  float32 (exact ABI class -- NOT widened at the call boundary)
 *     'v'  void (return position only)
 *
 * e.g. "i:ifi" = int64 f(int64, double, int64); "v:" = void f(void).
 * The bracketed "{...}" struct-by-value form is F4 and not accepted yet.
 *
 * In a non-JIT build no provider is ever installed and every consumer keeps
 * today's behavior (per-export __ffi shims, the generated shape table, the
 * extern-c nil stub, a clean "requires a JIT-enabled build" diagnostic for
 * call-ptr).  The provider interface is deliberately the swap point if
 * Windows/WASM ever need a non-MIR thunk source (dyncall / libffi). */
#ifndef TUR_TURI_JIT_FFI_H
#define TUR_TURI_JIT_FFI_H

#include <stddef.h>
#include <stdint.h>

#include "compiler/types.h"   /* TypeKind */

/* A compiled call thunk.  `fn` is the target function pointer; `iv`/`fv`
 * are the position-indexed marshalled argument buffers (arg k reads iv[k]
 * or fv[k] by its class -- the same convention the per-export __ffi shims
 * and the generated shape table already use); `sv`/`out_s` are reserved
 * for the F4 struct-by-value extension and are always NULL today.  The
 * return value is written to *out_i or *out_f by the return class ('v'
 * writes neither). */
typedef void (*TurJitFfiThunkFn)(void *fn, const int64_t *iv,
                                 const double *fv, void *sv,
                                 int64_t *out_i, double *out_f, void *out_s);

typedef struct TurJitFfiProvider {
    /* Return a cached or freshly compiled thunk for `sig`, or NULL with a
     * human-readable reason in errbuf (c2mir compile errors surface here as
     * diagnostics, never aborts). */
    TurJitFfiThunkFn (*thunk_for)(const char *sig, char *errbuf,
                                  size_t errcap);
    /* Resolve a C symbol against this process (dlsym(RTLD_DEFAULT)), for
     * extern-c registration.  NULL when the symbol is not visible; the
     * resolution order (process image incl. ENABLE_EXPORTS runtime, then
     * anything dlopened RTLD_GLOBAL, e.g. jit autolink libs) is dlsym's. */
    void *(*resolve)(const char *name);
} TurJitFfiProvider;

/* Install / read the process-wide provider.  Installed once at startup by a
 * JIT build (tur_jit_ffi_install in jit_ffi.c, called from main); NULL in
 * non-JIT builds and in embedders that never install one. */
void tur_jit_ffi_set_provider(const TurJitFfiProvider *p);
const TurJitFfiProvider *tur_jit_ffi_provider(void);

/* Classify a TypeKind for the signature vocabulary above.  Mirrors codegen's
 * ffi_shim_class_for_kind (emit_module.c) except that float32 reports its
 * exact 'F' class here -- the thunk implements the precise ABI, so it must
 * not widen.  '?' = not representable as a scalar (structs, ADTs, carriers);
 * the caller declines and falls back / errors cleanly. */
char tur_jit_ffi_class_for_kind(TypeKind k, int is_return);

/* Render `n` parameter kinds + a return kind into `buf` as a sig string.
 * Returns 0 on success, -1 (buf untouched) when any kind classifies '?' or
 * the buffer is too small.  bufcap must cover n + 3 bytes. */
int tur_jit_ffi_sig_render(TypeKind ret, const TypeKind *params, uint32_t n,
                           char *buf, size_t bufcap);

/* Install the c2mir-backed provider.  Defined in jit_ffi.c (tur_jit_obj,
 * JIT builds only); call from the host's startup under #ifdef TUR_HAVE_JIT.
 * Idempotent. */
void tur_jit_ffi_install(void);

#endif /* TUR_TURI_JIT_FFI_H */
