#ifndef TUR_STDLIB_AUTOLOAD_H
#define TUR_STDLIB_AUTOLOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "forms.h"
#include "symbols.h"

/* -------------------------------------------------------------------------
 * Stdlib autoload
 *
 * Turmeric source sees `Cons`, `Option`, `when`, `assert!` and the rest
 * without importing anything, because every compilation unit gets the stdlib's
 * forms prepended to it. That prepend is what this file owns.
 *
 * It used to live in main.c alongside the CLI. That was fine while `tur` was
 * the only front end; it stopped being fine the moment a second one appeared
 * (the WASM playground's in-process analyzer, src/web/wasm_lsp.c), because a
 * second front end that does not prepend the same list is a second front end
 * that disagrees with the compiler about what is in scope -- and the way that
 * surfaces is a browser reporting "unknown function `cons`" on code the
 * compiler accepts.
 *
 * The stdlib *directory* is a parameter rather than something resolved here:
 * the CLI finds it by walking up from the executable, and the browser has it
 * mounted at a fixed path in its virtual filesystem. Neither resolution belongs
 * to the prepend.
 * --------------------------------------------------------------------- */

/* NULL-terminated list of stdlib basenames, in load order. Order is
 * load-bearing -- macros.tur first, and the typeclass files before the
 * containers that instantiate them. */
const char *const *tur_stdlib_autoload_files(void);

/* Read every autoload file out of `stdlib_dir`, parse it, and prepend the
 * resulting forms onto *forms_in_out (reallocating into `arena`). Updates
 * *nforms_in_out and the running *file_id_in_out counter.
 *
 * Returns the number of prepended forms, which the caller passes to
 * elaborate_program as `stdlib_prefix` so the elab loop can bracket them with
 * `in_stdlib_load = true` -- that is what sets `is_from_stdlib` on the
 * resulting bindings, which is in turn what the separate-compilation emit path
 * keys off to static-ify stdlib defns per TU.
 *
 * `entry_path` is the user-visible input file. When `no_auto_stdlib` is set
 * and the entry file IS one of the autoload files, everything from that file
 * onward is skipped -- the file cannot be autoloaded ahead of itself.
 *
 * Files that cannot be read are skipped silently: a partial stdlib is the
 * caller's problem to diagnose, and every missing file would otherwise print
 * during ordinary `--no-auto-stdlib` builds.
 */
uint32_t tur_stdlib_prepend_forms(Arena *arena, SymbolTable *st,
                                  const char *stdlib_dir,
                                  const char *entry_path,
                                  bool no_auto_stdlib,
                                  Form ***forms_in_out,
                                  uint32_t *nforms_in_out,
                                  uint8_t *file_id_in_out);

#endif
