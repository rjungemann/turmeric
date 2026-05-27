#ifndef TUR_EMIT_H
#define TUR_EMIT_H

#include "buf.h"
#include "expr.h"

/* Emit a complete C99 program from a typed Expr program (EX_PROGRAM). Returns
 * 0 on success, -1 on error (diagnostics already emitted). */
int emit_program(Buf *out, const Expr *program);

/* Phase 2: Multi-file support - emit separate .h and .c for a module.
 * Returns 0 on success, -1 on error.
 * `separate_compilation` (Phase M3): when true, the header exports only
 * exported symbols and the implementation #includes imported modules' headers
 * rather than inlining their code. */
int emit_header(Buf *out, const char *module_name, const Expr *program, bool separate_compilation);
int emit_implementation(Buf *out, const char *module_name, const Expr *program, bool separate_compilation);

/* RP1: append one manifest line per exported defn of the form
 *   <module>/<defn> -> <mangled-c-symbol> :: (:arg-tag ...) -> :ret-tag
 * to `out`. Tags use the Turmeric type DSL (:int, :cstr, :float, :bool,
 * :void, :ptr, :any). Variadic defns get a `& :rest-tag` after the fixed
 * args. Returns 0 on success, -1 on error. Used by `tur build --shared`
 * to produce exports.manifest for the dlopen host (REPL, FFI dispatcher). */
int emit_exports_manifest(Buf *out, const Expr *program);

#endif
