/* spice_loader.h -- RP3 of docs/spice-repl-plan.md.
 *
 * Discovers an enclosing spice project (walking up from the cwd to find
 * a build.tur), invokes `tur build --shared` if the cached library is
 * stale, dlopens the resulting .so, parses its exports.manifest, and
 * primes a symbol map that the REPL's binding layer (RP4) can consult
 * when the user evaluates `(import ...)`.
 *
 * Lifetime: a TurSpiceImage owns the dlopen handle and the parsed
 * export list. Free it with tur_spice_image_free; the REPL does this
 * implicitly by freeing the TuriEnv that holds the image.
 */
#ifndef TUR_SPICE_LOADER_H
#define TUR_SPICE_LOADER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum positional arity tracked per export. Mirrors MAX_FN_ARITY in
 * the compiler; defns with more positional args are rejected by the
 * elaborator long before this loader sees them. */
#define TUR_SPICE_MAX_ARITY 16

/* One row from exports.manifest, post-parse. The dispatcher class chars
 * use the encoding shared with src/runtime/ffi_dispatch.h:
 *   'i' -- int64_t (covers :int / :cstr / :bool / :ptr / sized ints)
 *   'f' -- double  (covers :float / :float64)
 *   'v' -- void return only (never appears in arg_classes)
 *   '?' -- unrepresentable in v1 (e.g. :struct return); the binding
 *          layer must surface a clear error when the user calls it. */
typedef struct TurSpiceExport {
    char    *module;      /* defmodule name, e.g. "smokelib" */
    char    *name;        /* defn name, e.g. "add42" */
    char    *mangled;     /* C symbol name, e.g. "smokelib__add42" */
    void    *fn_ptr;      /* dlsym'd address */
    char     ret_class;
    char     arg_classes[TUR_SPICE_MAX_ARITY];
    uint8_t  n_args;
    bool     is_variadic; /* true when the manifest line had `& :tag` */
} TurSpiceExport;

/* Opaque image handle. */
typedef struct TurSpiceImage TurSpiceImage;

/* Discover + build + dlopen + parse, all in one call.
 *
 * `start_dir`  -- walk-up origin (typically the cwd). Required.
 * `tur_bin`    -- path to the `tur` executable used for the rebuild
 *                 subprocess; pass NULL to default to "tur" (PATH lookup).
 * `out_image`  -- on success, receives a heap-allocated image. NULL
 *                 on the "no project here" path (return 1).
 *
 * Return codes:
 *    0 -- success; *out_image is populated.
 *    1 -- no build.tur found by walking up from start_dir; *out_image
 *         is NULL. This is the expected pure-Turmeric REPL path.
 *   -1 -- hard error (build failed, dlopen failed, manifest malformed).
 *         A descriptive message has already been printed to stderr;
 *         *out_image is NULL. */
int tur_spice_image_load(const char *start_dir, const char *tur_bin,
                         TurSpiceImage **out_image);

void tur_spice_image_free(TurSpiceImage *img);

/* Accessors used by RP4's binding layer (and by tests / status output). */
const char     *tur_spice_image_root  (const TurSpiceImage *img);
const char     *tur_spice_image_lib   (const TurSpiceImage *img);
uint32_t        tur_spice_image_count (const TurSpiceImage *img);
const TurSpiceExport *tur_spice_image_at(const TurSpiceImage *img,
                                          uint32_t index);

/* Look up an export by (module, name). Returns NULL if not present. */
const TurSpiceExport *tur_spice_image_find(const TurSpiceImage *img,
                                            const char *module,
                                            const char *name);

/* RP5: true when every .tur file under the image's build directory has
 * mtime <= the cached library's. False (rebuild needed) when any source
 * is newer, the library is missing, or img is NULL. Used by (reload) to
 * short-circuit when nothing changed. */
bool tur_spice_image_is_fresh(const TurSpiceImage *img);

#ifdef __cplusplus
}
#endif

#endif /* TUR_SPICE_LOADER_H */
