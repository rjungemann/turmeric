#ifndef TUR_EMIT_H
#define TUR_EMIT_H

#include "buf.h"
#include "expr.h"
#include "types.h"

/* Emit a complete C99 program from a typed Expr program (EX_PROGRAM). Returns
 * 0 on success, -1 on error (diagnostics already emitted). */
int emit_program(Buf *out, const Expr *program);

/* project-mode-rc-runtime-preamble-missing (owner-TU design): emit the shared
 * inline C runtime as an include-guarded header (`tur_runtime.h`).  Every module
 * TU of a separate-compilation (`--shared`) build includes this; the single TU
 * that #defines TUR_RT_OWNER before including it (the generated tur_runtime.c)
 * supplies the one definition of each runtime global, while the runtime
 * functions are file-local (static) replicas operating on that shared state. */
void emit_shared_runtime_header(Buf *out);

/* S2 (jit-engine-plan, findings 19.4): the feature-complete SINGLE-FILE
 * runtime preamble -- every program-gated block forced on, single-file
 * linkage -- ending at the preamble marker.  Consumed by the split-generation
 * tool (via `tur emit-rt-split`) to produce the committed runtime-TU +
 * declarations artifacts, and by cmd_jit to hash the current emitter's
 * preamble against the artifact's recorded hash.  Deliberately reflects
 * current process knob state (backtrack depth, archive mode, experiment
 * flags): knob drift must change the text and fail the hash compare. */
void emit_rt_split_source(Buf *out);

/* DEDUP-4b (docs/archive/gc-cycle-collection-plan.md): declare, rather than define, the
 * rc<T>/GC runtime in the emitted preamble, because the program will link
 * libturt_runtime.a (which carries src/runtime/{rc,gc,rc_free_queue}.c).
 * Without this a compiled program runs a hand-written replica of the collector
 * while the maintained implementation sits unused in the archive.
 *
 * Must be set BEFORE emission -- the generated text depends on it.  main.c
 * resolves it from the same archive probe the link step uses; the probe is a
 * pure filesystem check, so it is safe to run ahead of codegen. */
void emit_set_rcgc_from_archive(bool from_archive);

/* J5/J6: A forced ABI specialization -- a clone that a borrower module needs
 * emitted by the owner module (even when the owner has no local call sites).
 * Passed from cmd_build_multi / cmd_emit_c_to_dir to emit_header and
 * emit_implementation after the cross-module borrow-spec scan. */
typedef struct {
    const char *clone_name;          /* unique mangled clone name */
    const char *fn_symbol;           /* binding name to look up in owner module */
    TypeKind    result_kind;
    TypeKind    arg_kinds[MAX_FN_ARITY];
    uint8_t     n_args;
} ForcedAbiSpec;

/* J6: Borrow spec info output from emit_implementation.  Each entry describes
 * a clone this module borrowed (call-site rewrite, no body).  The owner is
 * identified by fn_binding->defining_module_name stored in owning_module.
 * All three string fields are malloc'd; caller must free them. */
typedef struct {
    char       *clone_name;          /* malloc'd */
    char       *owning_module;       /* malloc'd (strdup of binding->defining_module_name) */
    char       *fn_symbol;           /* malloc'd (strdup of binding->name->name) */
    TypeKind    result_kind;
    TypeKind    arg_kinds[MAX_FN_ARITY];
    uint8_t     n_args;
} BorrowSpecInfo;

/* Phase 2: Multi-file support - emit separate .h and .c for a module.
 * Returns 0 on success, -1 on error.
 * `separate_compilation` (Phase M3): when true, the header exports only
 * exported symbols and the implementation #includes imported modules' headers
 * rather than inlining their code.
 * J5/J6: `forced`/`n_forced` inject specialization clones that borrower
 * modules need emitted here; pass NULL/0 for the first-pass compilation.
 * `out_borrow_specs`/`out_n` (emit_implementation only): if non-NULL, receives
 * a malloc'd array of BorrowSpecInfo describing clones this module borrowed. */
int emit_header(Buf *out, const char *module_name, const Expr *program,
                bool separate_compilation,
                const ForcedAbiSpec *forced, uint32_t n_forced);
int emit_implementation(Buf *out, const char *module_name, const Expr *program,
                        bool separate_compilation,
                        const ForcedAbiSpec *forced, uint32_t n_forced,
                        BorrowSpecInfo **out_borrow_specs, uint32_t *out_n_borrow_specs);

/* RP1: append one manifest line per exported defn of the form
 *   <module>/<defn> -> <mangled-c-symbol> :: (:arg-tag ...) -> :ret-tag
 * to `out`. Tags use the Turmeric type DSL (:int, :cstr, :float, :bool,
 * :void, :ptr, :any). Variadic defns get a `& :rest-tag` after the fixed
 * args. Returns 0 on success, -1 on error. Used by `tur build --shared`
 * to produce exports.manifest for the dlopen host (REPL, FFI dispatcher). */
int emit_exports_manifest(Buf *out, const Expr *program);

#endif
