#ifndef TUR_EMIT_INTERNAL_H
#define TUR_EMIT_INTERNAL_H

/* Shared internals for the emit_*.c translation units (emit_core, emit_expr,
 * emit_stmt, emit_fns, emit_module). This header is not installed; the public
 * codegen API lives in emit.h. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"

#include "builtins.h"
#include "cps.h"
#include "rc.h"
#include "rc_elision.h"
#include "types.h"

/* Phase R5: Global panic strategy flag (set by main.c --panic-abort) */
extern bool g_panic_abort;

/* Phase R6: Result/panic linting flags (set by main.c) */
extern bool g_warn_unused_result;
extern bool g_lint_panic;
extern bool g_panic_trace;
/* Phase P3: HAMT lowering - track if HAMT is needed */
extern bool g_needs_hamt;
/* stdlib/re.tur: track if any inline-C body references <regex.h>. */
extern bool g_needs_regex_h;
/* AR8: Variadic rest parameters - track if any variadic defn is compiled */
extern bool g_has_variadics;
/* SS0a: Session types enabled (-Xsessions) */
extern bool g_sessions_enabled;

/* Phase B5: backtrack depth cap (set by main.c --backtrack-depth N) */
extern int64_t g_backtrack_depth;
/* Phase B5: dump cloneable capture plan (set by main.c --dump-clone-plan) */
extern bool g_dump_clone_plan;

/* Forward declarations */
struct DeferThunk;

/* GS5/CS3: AbiTypeBinding is defined in expr.h and shared with elab_call.c so
 * the emit phase consumes the substitution that elab already computed. */
typedef struct EmitAbiSpecialization {
    const Expr *call_expr;
    const Expr *fn_expr;
    FnDef *fn;
    Binding *binding;
    AbiTypeBinding bindings[ABI_TYPE_BINDINGS_MAX];
    uint8_t n_bindings;
    Type arg_types[MAX_FN_ARITY];
    uint8_t n_args;
    Type result_type;
    char *clone_name;
} EmitAbiSpecialization;

typedef struct EmitCtx {
    Buf  *file;     /* file-scope decls (statics, includes) */
    Buf  *main_;    /* main() body */
    Buf  *thunk_typedefs; /* TS1: shared thunk typedef prelude */
    int   indent;
    int   tmp_n;
    /* Phase 2: when emitting a function body, these are the parameter bindings
     * that should use raw names (without ID suffix) when referenced. */
    Binding **fn_params;
    uint8_t   n_fn_params;
    /* Phase 3: Track emitted env struct names to avoid duplicates */
    const Symbol **env_struct_names;
    uint8_t   n_env_struct_names;
    uint8_t   cap_env_struct_names;
    /* TS1: per-signature thunk typedef tracking */
    char    **thunk_typedef_names;
    uint32_t  n_thunk_typedef_names;
    uint32_t  cap_thunk_typedef_names;
    /* Phase 3: For closure thunk emission, track the current closure */
    struct Closure *closure;
    const char *env_var_name;  /* Name of the casted env variable (e.g., "__env_4") */
    /* Phase 4 v1: Frame tracking for unified defer model */
    const char *frame_var;    /* Name of current tur_frame variable (e.g., "__frame_3") */
    bool in_scope_with_defers; /* Track if current scope has defers */
    struct DeferThunk *pending_defer_thunks; /* Thunks to emit at file scope */
    /* Phase 4 v1: For defer thunk emission with captures */
    Binding **defer_captures;    /* Current defer thunk's captures (NULL if none) */
    uint8_t n_defer_captures;
    /* Phase 3/4: Track if a return has been emitted in the current scope */
    bool return_emitted;
    /* Phase 19: Buffer for effect handler functions that must be emitted just
     * before the enclosing function definition (never inside a function body). */
    Buf *pending_handler_fns;
    /* Phase R5: true when currently emitting a #[no-unwind] function body;
     * causes EX_PANIC/EX_PANIC_WITH to emit tur_panic_abort instead of
     * tur_panic so that no setjmp/longjmp unwinding is attempted. */
    bool no_unwind;
    /* Phase M3: when true, each module is compiled to its own .c/.h pair;
     * exported functions are not static and headers are export-filtered. */
    bool separate_compilation;
    /* Phase 19D: Handle body fiber - outer variable captures */
    Binding **handle_captures;
    uint32_t n_handle_captures;
    const char *handle_env_name;  /* e.g., "__henv_5" */
    /* GF1: Generator emission context */
    Binding    **gen_struct_bindings;    /* struct fields when inside _next fn (NULL outside) */
    uint32_t     n_gen_struct_bindings;
    const char  *gen_var_name;           /* "__g" when inside _next function (NULL outside) */
    const char  *gen_struct_type;        /* struct type name when inside _next (NULL outside) */
    bool         gen_hdr_emitted;        /* true once __tur_gen_hdr_t typedef is in ctx->file */
    EmitAbiSpecialization *abi_specializations;
    uint32_t     n_abi_specializations;
    uint32_t     cap_abi_specializations;
    const Expr **specialized_call_exprs;
    const char **specialized_call_names;
    uint32_t     n_specialized_calls;
    uint32_t     cap_specialized_calls;
    /* KB-022: function bindings that are the target of a direct (non-specialized)
     * call.  A generic-unsafe function with such a callsite must still have its
     * carrier definition emitted, since no specialization clone will stand in. */
    const Binding **carrier_call_bindings;
    uint32_t        n_carrier_call_bindings;
    uint32_t        cap_carrier_call_bindings;
    const EmitAbiSpecialization *current_abi_specialization;
    const char  *fn_name_override;
    /* Phase D: pass-by-ptr param bindings for the current function.
     * Populated by emit_fn_def before body emission; cleared after.
     * expr_is_pbp_param checks this to decide whether a receiver uses -> . */
    Binding     *pbp_param_ptrs[16]; /* MAX_FN_ARITY */
    uint8_t      n_pbp_params;
} EmitCtx;

/* Phase 4 v1: Defer thunk tracking */
typedef struct DeferThunk {
    char *name;              /* Thunk function name (e.g., "__defer_1") */
    Expr *body;              /* The defer body expression */
    Binding **captures;      /* Captured bindings (NULL if none) */
    uint8_t n_captures;
    char *env_name;          /* Env struct name for captured defers (NULL if no captures) */
    struct DeferThunk *next; /* Linked list */
} DeferThunk;

/* ------------ emit_core.c: carrier ABI bridge ------------ */

/* Two-state tag: which ABI is a value in?
 * CK_CARRIER  -- int64_t slot (heap pointer cast to intptr_t, or inline
 *                8-byte payload reinterpreted from the scalar value)
 * CK_CONCRETE -- concrete C type (struct by value, or scalar at its native
 *                width) */
typedef enum { CK_CARRIER = 0, CK_CONCRETE = 1 } CarrierKind;

/* Emit a bridge expression so that src_str (already emitted, malloc'd) is
 * delivered with the ABI expected by sink_ck.
 *
 * Returns a fresh malloc'd C expression string.  When src_ck == sink_ck the
 * input string is returned unchanged (ownership transferred).  When the
 * crossing requires spilling to a local (CK_CONCRETE -> CK_CARRIER for
 * aggregate types), the spill statement is appended to body at the current
 * indent level and the returned expression refers to the spill variable.
 *
 * concrete_ty must describe the concrete type at the crossing (the struct,
 * ADT, or scalar type -- never TY_INT / TY_TYVAR).
 *
 * Ownership: consumes src_str (frees it when wrapping). */
char *emit_carrier_bridge(EmitCtx *ctx, Buf *body,
                          char *src_str,
                          CarrierKind src_ck, CarrierKind sink_ck,
                          Type concrete_ty);

/* ------------ emit_core.c: helpers, naming, atoms, builtins ------------ */
/* SS2: Perform __TUR_CAP_N__ / __TUR_VAL_N__ substitution on an InlineC node.
 * Evaluates val_exprs[N] into temp vars; returns a malloc'd substituted string. */
char *inline_c_substitute(EmitCtx *ctx, Buf *body, InlineC *ic);
/* Phase G: true when the inline-C body contains a __TUR_TY_<NAME>__ template
 * marker. emit_abi_register_call uses this to gate ABI specialization on
 * inline-C bodies (only the opt-in template form is safe to specialize). */
bool inline_c_has_ty_template(const InlineC *ic);
const Expr **flatten_program_items(const Expr *program, uint32_t *out_n);
Type emit_type_from_kind(TypeKind k);
Type emit_resolve_type(EmitCtx *ctx, Type t);
const char *emit_type_c_name(EmitCtx *ctx, Type t);
/* KB-021: arbiter of which struct-valued types may use the int64_t carrier ABI
 * (see emit_core.c). */
bool type_uses_carrier_abi(Type t);
void indent_buf(Buf *b, int n);
bool expr_is_divergent(const Expr *e);
bool expr_contains_return_or_throw(const Expr *e);
bool expr_tail_diverges(const Expr *e);
bool expr_has_multishot_handler(const Expr *e);
char *fresh_tmp(EmitCtx *ctx);
char *fresh_frame(EmitCtx *ctx);
char *fresh_defer_thunk(EmitCtx *ctx);
char *fresh_defer_env(EmitCtx *ctx);
void register_defer_thunk(EmitCtx *ctx, const char *name, const Expr *body,
                          Binding **captures, uint8_t n_captures,
                          const char *env_name);
void emit_pending_defer_thunks(EmitCtx *ctx, Buf *out);
char *mangle_dynvar_name(const char *name);
char *mangle_field_name(const char *name);
char *raw_name_for_binding(const Binding *b);
char *emit_call_name(EmitCtx *ctx, const Expr *call, const Binding *b);
char *name_for_binding(EmitCtx *ctx, const Binding *b);
void emit_c_string(Buf *out, StrSlice s);
char *atom_nil(void);
char *atom_bool(bool b);
char *atom_int_typed(int64_t i, TypeKind k);
char *atom_float32(double f);
char *atom_float(double f);
char *atom_var(EmitCtx *ctx, const Binding *b);
char *atom_cstr(StrSlice s);
char *emit_builtin(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_dict_name(char *buf, size_t buflen, const TypeClassInstance *inst);
void collect_defined(const Expr *e, Binding ***defs, uint32_t *ndefs, uint32_t *cdefs);
Binding **collect_handle_captures(const Expr *body, uint32_t *n_out);
bool use_typed_thunk_abi(Type result_type, Type *param_types, uint8_t n_params);
char *ensure_typed_thunk_typedef(EmitCtx *ctx, Buf *out,
                                 Type result_type, Type *param_types, uint8_t n_params);

/* ------------ emit_effects.c: effects/CPS expression emission ------------ */
/* Region C -- algebraic effects (EX_DEFECT, EX_PERFORM, EX_HANDLE, EX_RESUME,
 * EX_DISCONTINUE).  Expression-position emission only; emit_program runtime
 * fragments remain in emit_module.c. */
char *emit_effects_defect(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_perform(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_handle(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_resume(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_discontinue(EmitCtx *ctx, Buf *body, const Expr *e);
/* Region A -- delimited / cloneable / serial continuations */
char *emit_effects_reset(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_cloneable_reset(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_shift(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_shift0(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_cloneable_shift(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_serial_reset(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_serial_shift(EmitCtx *ctx, Buf *body, const Expr *e);
/* Region B -- continuation predicate */
char *emit_effects_cont_pred(EmitCtx *ctx, Buf *body, const Expr *e);

/* ------------ emit_expr.c: expression-position emission ------------ */
char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e);

/* ------------ emit_stmt.c: statement-position emission ------------ */
void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_while_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_set_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_set_deref_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_set_field_stmt(EmitCtx *ctx, Buf *body, const Expr *e);

/* ------------ emit_fns.c: function-definition emission ------------ */
void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e);

#endif
