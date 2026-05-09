#ifndef TUR_ELAB_H
#define TUR_ELAB_H

#include "arena.h"
#include "expr.h"
#include "forms.h"
#include "symbols.h"

/* Elaborate a sequence of top-level Forms into a typed Expr program. The
 * result is an Expr of kind EX_PROGRAM, or NULL if any error was diagnosed.
 *
 * `arena` and `st` must outlive the returned Expr (it points into them). */
Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms);

#endif
