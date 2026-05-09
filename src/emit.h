#ifndef TUR_EMIT_H
#define TUR_EMIT_H

#include "buf.h"
#include "expr.h"

/* Emit a complete C99 program from a typed Expr program (EX_PROGRAM). Returns
 * 0 on success, -1 on error (diagnostics already emitted). */
int emit_program(Buf *out, const Expr *program);

/* Phase 2: Multi-file support - emit separate .h and .c for a module.
 * Returns 0 on success, -1 on error. */
int emit_header(Buf *out, const char *module_name, const Expr *program);
int emit_implementation(Buf *out, const char *module_name, const Expr *program);

#endif
