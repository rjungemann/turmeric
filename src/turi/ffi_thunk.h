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

#include "env.h"
#include "spice_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install one TuriNativeFn binding per export in `img`. Returns the
 * number of bindings installed (= image's export count). Safe to call
 * with NULL img -- returns 0. */
uint32_t tur_ffi_install_spice_bindings(TuriEnv *env, TurSpiceImage *img);

#ifdef __cplusplus
}
#endif

#endif /* TUR_TURI_FFI_THUNK_H */
