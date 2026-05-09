#ifndef TUR_EMIT_H
#define TUR_EMIT_H

#include "buf.h"
#include "expr.h"

/* Emit a complete C99 program from a typed Expr program (EX_PROGRAM). Returns
 * 0 on success, -1 on error (diagnostics already emitted). */
int emit_program(Buf *out, const Expr *program);

#endif
