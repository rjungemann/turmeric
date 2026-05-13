#ifndef TUR_ELAB_H
#define TUR_ELAB_H

#include "arena.h"
#include "expr.h"
#include "forms.h"
#include "symbols.h"

/* Elaborate a sequence of top-level Forms into a typed Expr program. The
 * result is an Expr of kind EX_PROGRAM, or NULL if any error was diagnosed.
 *
 * `arena` and `st` must outlive the returned Expr (it points into them).
 * `module_base_dir` is the directory to search for imported module files
 * (Phase M2); pass NULL or "." to use the current working directory.
 * `separate_compilation` (Phase M3): when true, imported modules are not
 * inlined; callers emit separate .h/.c per module and #include them. */
Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation);

#endif
