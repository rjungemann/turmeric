/* ffi_thunk.h -- RP4 of docs/spice-repl-plan.md.
 *
 * Bridge between the REPL's eval layer (TuriValue / TuriEnv) and the
 * shape-keyed dispatcher in src/runtime/ffi_dispatch_thunk.c. For each
 * export in a loaded TurSpiceImage, tur_ffi_install_spice_bindings()
 * registers a TuriNativeFn shim that:
 *
 *   1. Validates arg count against the recorded signature.
 *   2. Marshals each TuriValue arg to int64_t (':int' class) or
 *      double (':float' class) per the export's arg_classes.
 *   3. Calls tur_ffi_thunk_call() to pick the right typed dispatcher
 *      from src/runtime/ffi_dispatch.h.
 *   4. Wraps the result back into a TuriValue ('i' -> turi_int,
 *      'f' -> turi_float, 'v' -> turi_nil).
 *
 * Each export is bound under TWO names so the user can call it either
 * way at the REPL prompt:
 *   - bare       (e.g. `add42`)
 *   - qualified  (e.g. `smokelib/add42`)
 *
 * The bare form makes the smoke test `(add42 100)` work without an
 * explicit `(import ...)` (which the REPL still rejects at the top
 * level today). The qualified form avoids collisions when two spices
 * happen to export the same defn name.
 */
#ifndef TUR_TURI_FFI_THUNK_H
#define TUR_TURI_FFI_THUNK_H

#include "turi/env.h"
#include "turi/spice_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install one TuriNativeFn binding per export in `img`. Returns the
 * number of bindings installed (= image's export count). Safe to call
 * with NULL img -- returns 0. */
uint32_t tur_ffi_install_spice_bindings(TuriEnv *env, TurSpiceImage *img);

/* jit-ffi-c2mir-plan F2: bind `name` to a thunk-backed native that calls
 * the resolved C function `fn` with the signature described by `ret_class`
 * + `arg_classes[n]` (the 'i'/'f'/'F'/'v' vocabulary of turi/jit_ffi.h).
 * The call goes through the JIT provider's per-signature thunk at each
 * invocation, so registration is cheap and a provider that appears later
 * (or a compile failure) surfaces per-call, as a clean error value.  The
 * classes buffer is copied; `name` must outlive the env (interned symbol
 * name).  Calls are gated on TURI_CAP_FFI.  Returns 0 on success, -1 on
 * OOM. */
int tur_ffi_register_extern_thunk(TuriEnv *env, const char *name, void *fn,
                                  char ret_class, const char *arg_classes,
                                  uint32_t n);

/* ------------------------------------------------------------------ */
/* jit-ffi-c2mir-plan F5: callbacks (C calling back into Turmeric)     */
/* ------------------------------------------------------------------ */

/* The context a generated callback carries.  Its ADDRESS is baked into the
 * generated C as a literal, so it must outlive every C library that holds
 * the function pointer -- i.e. the process.  Allocated by
 * tur_ffi_cb_ctx_new and never freed, matching turi's closure policy. */
typedef struct TurFfiCbCtx {
    TuriEnv  *env;
    TuriValue fn;
    char      ret_class;              /* 'i' / 'f' / 'F' / 'v' */
    char     *arg_classes;            /* n entries, owned */
    uint32_t  n_args;
} TurFfiCbCtx;

/* Build a process-lifetime callback context.  Returns NULL on OOM. */
TurFfiCbCtx *tur_ffi_cb_ctx_new(TuriEnv *env, TuriValue fn, char ret_class,
                                const char *arg_classes, uint32_t n);

/* The fixed entry point every generated callback calls.  Deliberately NOT
 * static and spelled with a stable name: the generated C declares it
 * `extern` and resolves it against this process (the `tur` executable links
 * with ENABLE_EXPORTS), so renaming it breaks every compiled callback.
 *
 * Unpacks the position-indexed `iv`/`fv` buffers into TuriValues per the
 * context's classes, calls the Turmeric function, and writes the result back
 * through *out_i / *out_f by the return class.  A Turmeric-side error is
 * reported on stderr and yields a zero result -- there is no error channel
 * back through a C callback slot, and unwinding through foreign frames is
 * not something we can do safely. */
void tur_ffi_cb_dispatch(void *ctx, const long long *iv, const double *fv,
                         long long *out_i, double *out_f);

/* RP5: register the `reload` native so the user can type `(reload)`
 * at the prompt. Available even when no spice is currently loaded
 * (the call surfaces a clean "no spice loaded" error in that case).
 * Called once during REPL start, before the first eval. */
void tur_ffi_register_reload_native(TuriEnv *env);

/* RP5: re-discover + rebuild + reinstall bindings for the spice in
 * env->spice_image. The current image is pushed onto
 * env->retired_spice_images so any in-flight bindings keep their
 * borrowed name strings valid. Returns:
 *   :nil    -- success (a TuriValue tagged TURI_NIL)
 *   :error  -- failure (env->spice_image left unchanged; a status
 *              message has been printed)
 * The function prints one line summarising the outcome ("no changes",
 * "rebuilt N exports", or an error explanation). */
TuriValue tur_ffi_reload_spice(TuriEnv *env);

#ifdef __cplusplus
}
#endif

#endif /* TUR_TURI_FFI_THUNK_H */
