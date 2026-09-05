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

/* D3 (compiled-catch-unwind-stackless-plan): max int params a stackless
 * self-recursive catch-unwind function may carry (the tur_cont saved[] width).
 * >5 positional params is already a code smell (MAX_FN_ARITY is 64), so 8 is a
 * generous cap; functions above it fall back to the normal lowering. */
#define TUR_SC_MAXP 8

/* G3 (compiled-catch-unwind-general-lowering): the general segment splitter
 * saves every live scalar local (params + hoisted let-vars + suspension result
 * temps) at each descend, so its tur_cont saved[] is wider than the scaffold's
 * per-param cap.  A function whose live-scalar count exceeds this falls back to
 * the scaffold / normal lowering. */
#define TUR_SC_MAXN 32

#include "arena.h"
#include "builtins.h"
#include "cps.h"
#include "diag.h"
#include "rc.h"
#include "rc_elision.h"
#include "types.h"

/* Phase R5: Global panic strategy flag (set by main.c --panic-abort) */
extern bool g_panic_abort;

/* Phase R6: Result/panic linting flags (set by main.c) */
extern bool g_warn_unused_result;
extern bool g_lint_panic;
extern bool g_emit_panic_trace;
/* Phase C2: --no-contracts (controls the TUR_CONTRACTS_ENABLED preamble define) */
extern bool g_no_contracts;
/* Debugger Phase 4: --debug emits `#line N "file.tur"` source-map directives. */
extern bool g_emit_debug_lines;
/* CPS3: selective CPS lowering path */
extern bool g_cps_path;
/* Phase P3: HAMT lowering - track if HAMT is needed */
extern bool g_needs_hamt;
/* stdlib/re.tur: track if any inline-C body references <regex.h>. */
extern bool g_needs_regex_h;

/* inline-c-function-scope-include-guards fix: deduped #include set lifted
 * from inline-C bodies. See globals.c and emit_core.c. */
extern char    **g_hoisted_includes;
extern uint32_t  g_n_hoisted_includes;
extern uint32_t  g_cap_hoisted_includes;

/* Marker appended to a hoisted `#include` whose author wrote a
 * `tur:optional` comment on the line -- a header that is EXPECTED to be
 * absent on some target (a per-platform alternative such as <direct.h> on
 * POSIX).  Such an include skips silently; every other missing angle header
 * announces itself.  See tur_emit_hoisted_include. */
#define TUR_HOIST_OPTIONAL_TAG " /* tur:optional */"

/* Append `n` bytes of `line` (the full `#include ...` directive without a
 * trailing newline) to the deduped global set. */
void   tur_hoist_include_add(const char *line, size_t n);

/* As tur_hoist_include_add, but records whether the author marked the include
 * `tur:optional`.  Dedup is by the directive itself, and an optional marking
 * from any site wins (one deliberate per-platform use makes the header
 * optional everywhere in the TU). */
void   tur_hoist_include_add_ex(const char *line, size_t n, bool optional);

/* Emit one hoisted `#include` line; angle (system) headers are wrapped in
 * `#if __has_include(...)` so a platform-missing header is skipped rather than
 * hard-failing the build.  An unmarked header that is missing emits a
 * `#pragma message` naming it, so the skip is never silent.  Quoted (project)
 * headers are emitted bare. */
void   tur_emit_hoisted_include(Buf *out, const char *line);

/* Scan `body` (length `len`) for leading blank/comment/`#include` lines,
 * append each `#include` to the global set, and return the byte count
 * consumed at the start of `body`. The caller should `memmove` to drop the
 * consumed prefix. */
size_t tur_hoist_top_includes_scan(const char *body, size_t len);
/* emit-value-dispatch-unbounded-recursion: the emitter's expression walk is
 * plain structural recursion and is bounded by a depth counter kept in
 * emit_expr.c.  Reset at the start of each program; if the bound was hit, a
 * TUR-E0712 has been reported and emit_program must fail rather than hand back
 * a TU whose over-deep expressions were replaced by `0`. */
void emit_expr_depth_reset(void);
bool emit_expr_depth_exceeded(void);

/* AR8: Variadic rest parameters - track if any variadic defn is compiled */
extern bool g_has_variadics;
/* prelude-macros (Defect B / F3): set when the user-callable `cons` runtime
 * list constructor is referenced; gates the `cons` helper emission. */
extern bool g_uses_cons;
/* SS0a: Session types enabled (-Xsessions) */
extern bool g_sessions_enabled;

/* SYM1 (runtime-symbols-plan): per-TU interned-symbol codegen registry.
 *   sym_codegen_reset()    -- clear the registry (call once per compilation unit)
 *   sym_codegen_register() -- intern `sym` by name; returns the stable mangled
 *                             C identifier of its static record (e.g.
 *                             "__tur_sym_foo").  Idempotent per name.
 *   sym_codegen_count()    -- number of distinct records registered so far.
 *   sym_codegen_emit()     -- emit `struct __tur_sym` (once) + one record per
 *                             distinct keyword into `out`.  When external_weak
 *                             is true (multi-TU/separate compilation) the
 *                             records use external weak linkage so the linker
 *                             folds same-named records across TUs (SYM2);
 *                             otherwise they are `static` (single-file/emit-c). */
struct Symbol;
void        sym_codegen_reset(void);
const char *sym_codegen_register(const struct Symbol *sym);
uint32_t    sym_codegen_count(void);
void        sym_codegen_emit(Buf *out, bool external_weak);
/* SYM5: note that str->sym is defined in this TU (gates the seeding ctor). */
void        sym_codegen_note_intern_used(void);

/* S1b (jit-engine-plan): per-TU explicit static-initialization registry.
 * Replaces per-site `__attribute__((constructor))`, which c2mir silently
 * discards (findings 3.1).  Bands encode the ordering the toolchain used to
 * pick for us; within a band, registration order is preserved. */
typedef enum StaticInitBand {
    STATIC_INIT_KEYS     = 0,  /* pthread_key_create for dynamic variables */
    STATIC_INIT_REGISTRY = 1,  /* __sk_register / __tur_cps_register / tur_sym_register */
    STATIC_INIT_ATEXIT   = 2,  /* module-defer atexit() registration */
    STATIC_INIT_DEFS     = 3,  /* __tur_module_def_init -- runs user code, last */
} StaticInitBand;
void     static_init_reset(void);
void     static_init_register(const char *fn, StaticInitBand band);
uint32_t static_init_count(void);
void     static_init_emit(Buf *out);

/* Phase B5: backtrack depth cap (set by main.c --backtrack-depth N) */
extern int64_t g_backtrack_depth;
/* Phase B5: dump cloneable capture plan (set by main.c --dump-clone-plan) */
extern bool g_dump_clone_plan;

/* Forward declarations */
struct DeferThunk;

/* struct-of-closures monomorphization: upper bound on the number of ADDITIONAL
 * (beyond the first) inner closures a single struct-of-closures return can link
 * to one outer spec.  A generic fn returning `(make-struct S clo1 .. cloN)`
 * links clo1 via inner_closure_spec_idx and clo2..cloN here, so this caps N-1.
 * 15 (total 16 closures) matches the practical struct field ceiling. */
#define TUR_EXTRA_INNER_CLOSURE_MAX 15

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
    /* J3: when true the clone is emitted with external (not static) linkage
     * and its forward decl omits 'static'.  False in whole-program mode. */
    bool external_linkage;
    /* poly-closure-result-specialization: for an inner-closure-body spec
     * (the lifted (fn ...) a generic closure-returning defn returns), this is
     * the suffixed env-struct symbol the clone -- and the enclosing outer
     * spec's EX_CLOSURE construction -- must agree on, so a float (register-
     * class-changing) specialization gets its own `struct __env_N__spec__...`
     * instead of reusing the base int64-carrier layout.  NULL for ordinary
     * specs. */
    const Symbol *env_name_override;
    /* On an OUTER (closure-returning) spec: index into abi_specializations of
     * the linked inner-closure-body spec, or -1 when none.  Lets the outer
     * spec body's EX_CLOSURE emit reference the inner clone's name + env. */
    int32_t inner_closure_spec_idx;
    /* struct-of-closures monomorphization: a generic fn that RETURNS a
     * struct-of-closures (`(make-struct S clo1 clo2 ...)`, lowered to a ctor
     * CALL whose args are the closures) links its FIRST closure via
     * inner_closure_spec_idx above; every ADDITIONAL closure needs its own
     * per-spec clone + suffixed env too, or its captured monomorph fields keep
     * the base int64-carrier type and the ctor-body construction assigns a
     * by-value struct into an int64 slot.  These are the extra links; the
     * EX_CLOSURE / thunk emit sites resolve a closure's inner spec by matching
     * `binding` across the primary index and this list. */
    int32_t extra_inner_closure_spec_idx[TUR_EXTRA_INNER_CLOSURE_MAX];
    uint8_t n_extra_inner_closure_spec_idx;
    /* M6 / gap G6(c): true when this spec is the per-spec clone of a CAPTURED
     * closure PASSED to a generic combinator (the recursive `(fn [c] : B
     * (re-cata alg c))` handed to `fmap`).  Scopes the return-only-poly result
     * recovery (recover a recursive call's `B` result from the active spec's
     * bindings) so it fires only inside such a clone body, not for every
     * return-poly call. */
    bool is_passed_closure_clone;
    /* poly-closure-result-specialization: set once this spec's body has been
     * emitted, so the spec emit loop can hoist an inner-closure clone ahead of
     * its outer (so the suffixed env struct lands at file scope) without
     * double-emitting it. */
    bool emitted;
    /* M4a (docs/archive/m4-typeclass-per-method-abi-plan.md): when this spec
     * is for a *typeclass instance method* (not an ordinary defn), the M4b/M4c
     * emit paths route it through the per-instantiation dict singleton path
     * instead of the uniform-carrier dict.  Set by emit_abi_intern_spec when
     * `fn->binding` resolves to an `__inst_*` method.
     *
     * Carries the `TypeClassInstance *` so the dict-emit loop can mangle the
     * dict struct name with this spec's type-arg tuple, and so the dispatch
     * site in emit_expr.c knows which singleton variant to reference.
     *
     * NULL for ordinary defn specs and for HKT-class instance methods (which
     * keep the uniform carrier ABI per Plan M6/M7 — see the plan doc's HKT
     * carve-out). */
    struct TypeClassInstance *typeclass_inst;
    /* VBM2b (van-laarhoven-monomorphization-plan): true when this spec is a
     * by-value monomorphized van Laarhoven lens body (`<lens>__mono_<hash>`),
     * whose HKT tyvar `f` is pinned to a concrete WIDE by-value functor.  While
     * its body is scanned/emitted the MB2.5 HKT carve-out
     * (emit_abi_register_call) is OPENED so the `fmap` dispatch inside mints a
     * by-value instance twin instead of the int64-carrier method -- the box
     * this whole path exists to remove. */
    bool is_vl_wide_mono;
    /* CM2 (van-laarhoven-consumer-mono-plan): true when this spec is a CONSUMER
     * clone -- one specialization of an ambiguous consumer (a lens param reached
     * with >= 2 distinct wide lenses) with the lens param bound to ONE concrete
     * lens.  `consumer_lens_binding` is that lens param's `Binding *`; while the
     * clone body is emitted, the `(l g s)` poly-call redirect resolves `l` to
     * `consumer_lens_name`/`consumer_lens_hash` (its `<lens>__mono_<hash>` body),
     * exactly as the |set|==1 VBM3 redirect does for a uniquely-resolved param.
     * The clone links `inner_closure_spec_idx` to the shared by-value `g` twin so
     * its `(l g s)` builds `g` by value (the twin thunk), not the boxed carrier. */
    bool is_consumer_mono;
    const void *consumer_lens_binding;
    const char *consumer_lens_name;       /* stable ptr into the mono-spec registry */
    unsigned long long consumer_lens_hash;
} EmitAbiSpecialization;

typedef struct EmitCtx {
    Buf  *file;     /* file-scope decls (statics, includes) */
    Buf  *main_;    /* main() body */
    Buf  *thunk_typedefs; /* TS1: shared thunk typedef prelude */
    int   indent;
    int   tmp_n;
    /* S1 (jit-engine-plan, findings 16): the return C type of the call
     * expression an EX_CALL builder just finished composing, handed to
     * emit_value's panic-hoist so the `__ps_N` temp can be declared with a
     * real type instead of `__auto_type` (which c2mir cannot parse).  This is
     * GROUND TRUTH, not inference: each builder writes the same ret_c string
     * it spelled into the cast / thunk-typedef of the call text itself.
     * Protocol: a builder sets it as the LAST thing before returning its
     * composed call string (nested argument calls were already hoisted and
     * consumed their own notes by then); emit_value captures and CLEARS it
     * unconditionally after every dispatch, so a note from a void/never call
     * (whose hoist is skipped) can never leak onto a later call.  Empty
     * string == no note. */
    char  call_ret_note[256];
    /* consolidation increment 2 (bind cell): set while emitting an
     * EX_POLY_WRAP argument of a call whose resolved callee is the CARRIER
     * base instance entry (an __inst_* binding with no matched by-value
     * spec and a tyvar-mentioning result).  Such a callee invokes the
     * continuation through the int64 carrier cast, so the wrapper must box
     * a by-value aggregate result (the spill shim) -- pairing the wrapper
     * ABI with the SELECTED entry point instead of guessing from receiver
     * abstractness (result-monad-bind-typed-boundary-miscompiles). */
    bool  poly_wrap_callee_carrier;
    /* Phase 2: when emitting a function body, these are the parameter bindings
     * that should use raw names (without ID suffix) when referenced. */
    Binding **fn_params;
    uint32_t  n_fn_params;
    /* inline-c-locals-invisible-to-inline-c-blocks: local bindings in the
     * function currently being emitted that an inline-C block in that same
     * function references BY THEIR SOURCE NAME.  A parameter already reaches
     * inline C that way (`raw_name_for_binding` above the id-suffix path), and
     * raw_name_for_binding's own comment states the intent for locals too --
     * but a local got `<name>_<id>`, so `(let [key ...] ```c ... key ... ```)`
     * emitted `'key' undeclared`.  These bindings are spelled raw so the two
     * agree.  Populated per function body and heavily filtered (see
     * emit_inline_c_raw_locals_collect): a name only lands here when it is a
     * plain C identifier, unshadowed within the function, not a parameter
     * name, not a C keyword or libc symbol, AND actually mentioned as an
     * identifier token by one of the function's inline-C blocks -- so a
     * function whose inline C does not name a local emits exactly what it
     * emitted before. */
    const Binding **inline_c_raw_locals;
    uint32_t        n_inline_c_raw_locals;
    /* M7: while emitting the ARGUMENTS of a constructor call, the enclosing
     * ctor's substituted field type for the argument being emitted (NULL
     * otherwise).  A nested bare ctor -- the inner `(None)` of `(Some (None))`
     * -- has no concrete type of its own, and the fallback it would otherwise
     * take (the active spec's RESULT family) is the OUTER family, so it emits
     * `ctor_None__Opt__int` where the outer expects `tur_adt_Opt__int`.  The
     * field type is the answer, and only the enclosing call knows it.  Set and
     * restored around each argument; never nested-owned (the pointee lives in
     * the caller's frame for exactly that span). */
    const Type *pending_ctor_field_ty;
    /* Phase 3: Track emitted env struct names to avoid duplicates */
    const Symbol **env_struct_names;
    /* The `__fn` field spelling each registered env struct was ACTUALLY
     * emitted with (a typed-thunk typedef, or NULL meaning `int64_t`),
     * parallel to env_struct_names.
     *
     * A struct is emitted once, at whichever site reaches it first, but its
     * `__fn` is assigned at every closure-construction site -- and those sites
     * do not all resolve the thunk's result the same way.  A generic site
     * resolves a tyvar result to the int64 carrier while a monomorphising site
     * (`..__byval`, `..__spec__..`) resolves the same result to a concrete
     * aggregate typedef, so recomputing the spelling at the assignment site
     * can contradict the declaration and store a function pointer through an
     * `int64_t` field ("makes integer from pointer without a cast").  Record
     * the decision once here and have the assignment read it back. */
    char     **env_struct_fn_typedefs;
    uint8_t   n_env_struct_names;
    uint8_t   cap_env_struct_names;
    /* TS1: per-signature thunk typedef tracking */
    char    **thunk_typedef_names;
    uint32_t  n_thunk_typedef_names;
    uint32_t  cap_thunk_typedef_names;
    /* closure-typed-invocation-abi-plan: per-signature typed fat-shim tracking.
     * EX_FN_TO_FAT boxes a non-capturing fn as { shim, orig }; for a non-int64
     * closure signature the shim must speak the closure's declared C types so
     * the typed-thunk invocation cast at the call site matches the shim's ABI. */
    char    **fatshim_names;
    uint32_t  n_fatshim_names;
    uint32_t  cap_fatshim_names;
    /* type-of-cast-kind-granularity: per-monomorph identity for `any` box tags.
     * A primitive keeps its TypeKind as its tag; a struct/ADT interns its
     * monomorph C name here and rides TUR_ANY_ID_BASE + index, so `cast` / `is?`
     * / `type-of` distinguish two struct types instead of both reading
     * "struct". */
    char    **any_type_names;   /* identity key: type_name(), per monomorph */
    char    **any_type_shown;   /* what type-of reports for that id */
    /* any-struct-box-leak-per-widen: is the payload behind this id HEAP-BOXED
     * at the widen site (a by-value aggregate) rather than carried in the tag's
     * value word?  Only a boxed one has anything to free, and the tag is the
     * only thing a drop site knows -- an `any` local is typed `any`, not by its
     * payload.  Interned alongside the id so the two cannot drift. */
    bool     *any_type_boxed;
    /* any-struct-box-leak-per-widen (the temporary case): C temp names holding
     * owned `any` values whose payload box must be dropped once the call
     * consuming them has been materialized.  Pushed when the argument is
     * emitted, drained by the enclosing call's emission -- the mark/drain pair
     * in emit_value keeps nested calls from stealing each other's entries. */
    char    **any_pending;
    uint32_t  n_any_pending;
    uint32_t  cap_any_pending;
    /* RM1: pending frees for fresh sum-carrier boxes consumed as accessor
     * arguments -- the any_pending discipline (mark before children, drain
     * after the call materializes), draining as a null-guarded free. */
    char    **sum_pending;
    Type     *sum_pending_types;
    bool     *sum_pending_owned;   /* entry's Type is a malloc'd spine to free at drain */
    uint32_t  n_sum_pending;
    uint32_t  cap_sum_pending;
    /* value-struct-payload-sum-monomorph-box-has-no-owner: pending frees for
     * fresh BY-VALUE sum monomorph temps whose arm holds a boxed value-struct
     * payload, consumed as a non-retaining argument.  Name + type pairs (the
     * arm layout is per-monomorph); same mark/drain discipline. */
    char    **vsp_pending;
    Type     *vsp_pending_types;
    uint32_t  n_vsp_pending;
    uint32_t  cap_vsp_pending;
    /* value-struct-payload-sum-monomorph-box-has-no-owner (dictionary sites):
     * the one argument node the enclosing class-method call has ADMITTED for
     * the drop-after stamp (Expr.sum_box_drop_after_dyn resolved against the
     * re-resolved instance's mask).  Set by the consumer's argument loop
     * around that argument's emission and restored after; the call hoist
     * treats pointer-identity with this as the stamp.  A nested call inside
     * the argument never matches (it is a different node). */
    const Expr *sum_drop_admit;
    /* container-element-form-plan CE2 (store half): set by the call argument
     * loop while it emits an argument that is a Vec ELEMENT STORE (the
     * callee declares `(Vec A)` and this slot as `A`) whose resolved element
     * form is CE_WORD and whose element is a niche option; the bridge's niche
     * row then hands the slot the payload WORD instead of materializing a
     * carrier box.  Restored after the argument, so nothing nested sees it. */
    bool ce_word_store_sink;
    /* any-struct-box-leak-per-widen: C names of `any` locals whose payload box
     * the ENCLOSING SCOPES own, innermost last.  The scope-exit drop is a
     * trailing free, so an early exit -- a `return`, or a self-tail-call's
     * back-edge goto -- jumps past it; those two sites walk this list and drop
     * first, the same shape as the dynvar guards beside them.  A given path
     * takes one or the other, never both, so nothing is freed twice. */
    char    **any_scope_drops;
    uint32_t  n_any_scope_drops;
    uint32_t  cap_any_scope_drops;
    uint32_t  n_any_type_names;
    uint32_t  cap_any_type_names;
    /* poly-to-fat-typed-shim-plan: per-signature typed poly-to-fat shim tracking.
     * EX_POLY_TO_FAT boxes a typeclass-method closure (tur_poly_fn_t) as
     * { shim, fn, env }; for a non-int64 method signature the slot-0 shim must
     * speak the method's declared C types so the typed-thunk invocation cast at
     * the sink's call site matches the shim's ABI (mirrors fatshim_names). */
    char    **poly_fatshim_names;
    uint32_t  n_poly_fatshim_names;
    uint32_t  cap_poly_fatshim_names;
    /* fn-value-fat-normalization: STATIC {shim, orig} boxes.  A box whose
     * `orig` is a file-scope function is a CONSTANT, so it does not need the
     * per-execution malloc EX_FN_TO_FAT would otherwise emit -- which is not
     * merely slow but an unbounded leak, since nothing drops a box handed to a
     * normalized param (a 5e6-iteration `(apply1 add3 acc)` loop leaked
     * 122 MiB).  `fatbox_keys` dedups on "<shim>|<orig>"; the definitions land
     * in `thunk_typedefs` and the fill statements in `fatbox_init`, emitted as
     * one `__tur_fatbox_init` registered in the earliest static-init band.
     *
     * `fatbox_names` holds the `__tur_fatbox_<i>` spelling ensure_static_fatbox
     * returns, one owned string per key and freed with them.  It used to be a
     * function-scoped `static char name[96]`, which is only correct while every
     * caller consumes the name before asking for another -- correct by call-site
     * discipline, not by construction; see
     * docs/archive/c-name-accessors-share-static-buffers.md. */
    char    **fatbox_keys;
    bool      fatbox_keep_emitted;   /* __tur_fatbox_keep glue is in thunk_typedefs */
    char    **fatbox_names;
    uint32_t  n_fatbox_keys;
    uint32_t  cap_fatbox_keys;
    Buf      *fatbox_init;
    /* constrained-byval dispatch: per-(class,struct-instance) carrier-adapter
     * witness-dict tracking.  A constrained existential over a by-value struct
     * payload points its witness at one of these `dict_<Class>_<T>__exbox`
     * thunk dicts so EX_EXISTS_DISPATCH's carrier ABI (int64 receiver) lines up
     * with the instance method's by-value-struct ABI. */
    char    **exbox_dict_names;
    uint32_t  n_exbox_dict_names;
    uint32_t  cap_exbox_dict_names;
    /* Phase 3: For closure thunk emission, track the current closure */
    struct Closure *closure;
    const char *env_var_name;  /* Name of the casted env variable (e.g., "__env_4") */
    /* Phase 4 v1: Frame tracking for unified defer model */
    const char *frame_var;    /* Name of current tur_frame variable (e.g., "__frame_3") */
    /* S1b/dynvar early-exit: the dynamic-binding guards currently in scope,
     * innermost LAST.  An early `return` out of a `binding` body must pop them
     * explicitly: the __attribute__((cleanup)) that covers this on the cc path
     * is discarded by c2mir with no diagnostic, which left the dynvar key
     * pointing at a returned function's stack frame (a SEGV, not a leak).
     * The pop is idempotent, so firing here AND via the attribute is safe. */
    char    **dynvar_guard_ptrs;
    char    **dynvar_guard_names;
    uint32_t  n_dynvar_guards, cap_dynvar_guards;
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
    /* direct-reset-shift-degrades fix: true while emitting the body of a base
     * (reset ...) that is lowered via the setjmp/longjmp escape path (branch-
     * capable).  A base `shift`/`shift0` emitted with this set aborts to the
     * innermost reset (tur_cur_shift_reset) instead of returning its operand.
     * Cleared across a nested DK-lowered reset (emit_cps_reset) so its shifts
     * still feed the DK abort-value model. */
    bool in_shift_escape;
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
    /* M5 Finding 7: the clone_name of the active (outer) specialization at the
     * moment this call was recorded -- NULL when recorded at top level.  One
     * shared source body (e.g. an `Eq Vec` instance method) is scanned once per
     * outer spec (Vec__int, Vec__bool, ...); each scan records the SAME inner
     * call Expr* with a DIFFERENT callee clone_name.  Keying the lookup on
     * (call Expr*, outer clone_name) instead of the call Expr* alone lets each
     * element-type spec body resolve its own callee clone, instead of all of
     * them collapsing onto the first-recorded (int) clone. */
    const char **specialized_call_outer;
    uint32_t     n_specialized_calls;
    uint32_t     cap_specialized_calls;
    /* KB-022: function bindings that are the target of a direct (non-specialized)
     * call.  A generic-unsafe function with such a callsite must still have its
     * carrier definition emitted, since no specialization clone will stand in. */
    const Binding **carrier_call_bindings;
    uint32_t        n_carrier_call_bindings;
    uint32_t        cap_carrier_call_bindings;
    /* dead-base-thunk-chain-references-undefined-ctor (fix direction 1,
     * narrowed): a HEAP parametric ADT never gets a base `ctor_X` definition
     * (only per-spec monomorphs), yet the dead base generic thunk chain still
     * emits suffix-less references to it -- `-O2` strips the chain, but a hand
     * `-O0` compile of emit-c output died at link on the undefined symbol.
     * Each such reference registers its mangled ctor name + arity here; the
     * traps are flushed as static file-scope definitions into the
     * forward-decl band (which precedes every function body), so the emitted
     * C is fully self-contained at any -O level.  A genuinely live call --
     * a compiler defect; it was an unconditional link error before -- now
     * aborts loudly at runtime with the ctor named. */
    char    **dead_base_ctor_names;
    uint32_t *dead_base_ctor_arities;
    uint32_t  n_dead_base_ctors;
    uint32_t  cap_dead_base_ctors;
    const EmitAbiSpecialization *current_abi_specialization;
    const char *current_fn_ret_ctype;
    /* MB1 / forall-dict-pass-multi-constraint-hkt-plan (Task 1.4): set while
     * emitting a dict-clone FnDef body (see FnDef.dict_clone_*).  A class-method
     * call carrying a `dict_arg` whose instance's class matches one of
     * `dict_dispatch_classes[0..dict_dispatch_n)` is emitted as a runtime
     * dispatch through the matching dict param `dict_dispatch_param_cnames[k]`
     * (an int64 carrier), indexing the class method's slot in the dict layout.
     * `dict_dispatch_n == 0` normally.  The scalar
     * `dict_dispatch_param_cname` / `dict_dispatch_class` mirror slot 0 for the
     * single-dict ambient-dict lowering paths (lens composition) that predate
     * the vector and still key off the first dict. */
    const char     *dict_dispatch_param_cname;
    struct TypeClass *dict_dispatch_class;
    uint8_t          dict_dispatch_n;
    struct TypeClass *dict_dispatch_classes[MAX_FN_CONSTRAINTS];
    const char      *dict_dispatch_param_cnames[MAX_FN_CONSTRAINTS];
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 3) +
     * forall-dict-pass-nested-mapper-general-plan (Phase 1): set while emitting a
     * nested MAPPER lambda that was converted into a dict-capturing closure
     * (FnDef.dict_env_classes / dict_env_bindings).  A class-method call whose
     * class matches ANY of `cur_dict_env_classes[0..cur_dict_env_n)` dispatches
     * through that class's CAPTURED dict -- an `env->dict` load via
     * capture_env_access(cur_dict_env_bindings[k]) -- indexing the method slot,
     * instead of the baked representative instance.  The vectors are parallel;
     * `cur_dict_env_n == 0` normally. */
    uint8_t           cur_dict_env_n;
    struct TypeClass *cur_dict_env_classes[MAX_FN_CONSTRAINTS];
    struct Binding   *cur_dict_env_bindings[MAX_FN_CONSTRAINTS];
    /* Variant 2 (generic-struct-opaque-element): the EX_FN_DEF whose body the ABI
     * scan is currently descending into (top-level scan only).  Used to tell a
     * generic *relay* call (inside a generic body, resolvable by binding
     * composition once the enclosing generic specializes) apart from a top-level
     * call with unresolvable phantom tyvars (KB-022) that needs its carrier
     * fallback emitted. */
    const Expr  *current_scan_fn;
    /* nested-construct-byvalue (Gap #5): set while the ABI scan descends into the
     * argument subexpressions of a #{Construct} call that is itself emitting as
     * the int64 carrier (no by-value spec).  A nested construct argument under
     * such a carrier consumer must NOT be promoted to a by-value spec, or the
     * carrier consumer (`ok(int64_t)`) would be handed a by-value aggregate
     * (`Option__int`).  Gates the Phase-5 construct promotion in
     * emit_abi_register_call. */
    bool         abi_scan_suppress_construct_byvalue;
    const char  *fn_name_override;
    /* J3: when true, the clone body being emitted via fn_name_override gets
     * external (not static) linkage -- set alongside fn_name_override. */
    bool         fn_name_override_external;
    /* Phase D: pass-by-ptr param bindings for the current function.
     * Populated by emit_fn_def before body emission; cleared after.
     * expr_is_pbp_param checks this to decide whether a receiver uses -> . */
    /* Pass-by-ptr param bindings for the current fn (or specialization group).
     * Growable (realloc) rather than fixed-[MAX_FN_ARITY] so a function with an
     * arbitrary number of by-const-ptr struct params is handled without a cap. */
    Binding    **pbp_param_ptrs;
    uint32_t     n_pbp_params;
    uint32_t     cap_pbp_params;
    /* ASan/LSan plan (Option C): arena for the transient Type nodes that
     * emit_resolve_type / emit_abi_instantiate_type clone while resolving
     * generic type variables to concrete types for ABI specialization. These
     * nodes live for the whole emit pass (specs reference them) and are
     * released in bulk when the pass finishes -- replacing the previous
     * never-freed mallocs that LeakSanitizer flagged. NULL falls back to
     * malloc (process-lifetime), preserving the old behavior for any context
     * that does not own an arena. */
    Arena       *type_arena;
    /* cps-transform-plan (a): the whole program, so the serial env codec can
     * scan for Serializable instances when marshaling a struct/nominal env. */
    const Expr  *program_root;
    /* Debugger Phase 4 (--debug): the (file_id, line) of the last `#line`
     * directive emitted into the current output stream, so consecutive
     * statements on the same source line do not each re-emit one.  line == 0
     * means "none emitted yet"; emit_line_reset() restores that at each
     * function-body boundary so the first statement always re-anchors. */
    uint32_t     dbg_last_line;
    uint16_t     dbg_last_file_id;
    /* CONV-S1 (borrow-struct-field): set while emitting the operand of an
     * immutable/mutable borrow so EX_GET_FIELD omits its outer `(cty)` rvalue
     * cast and yields a bare member lvalue (`(p).x`), so `&` has an lvalue
     * operand.  Read-and-cleared at EX_GET_FIELD entry so only the outermost
     * field access of the borrow operand is affected, not nested reads. */
    bool         lvalue_mode;
    /* G3 (compiled-catch-unwind-general-lowering): the general stackless
     * catch-unwind segment splitter fills expression "holes" -- a suspended
     * sub-expression (a self-call or catch-unwind whose value is delivered on a
     * later driver iteration) is replaced by a C temp when the enclosing
     * expression is re-emitted in the resume segment.  emit_value consults this
     * pointer-keyed override table first: an Expr* that matches `sub_holes[i]`
     * emits `sub_names[i]` verbatim instead of recursing.  Empty (n==0) off the
     * general path, so it never perturbs any other emission. */
    const Expr  *sub_holes[16];
    const char  *sub_names[16];
    uint8_t      n_sub_holes;
    /* BR3b (catch-unwind-byref-aggregate-br3b-plan): while emitting a fallible
     * reader call INSIDE the stackless trampoline driver, the post-call
     * tur_panicking signal must route to the driver's unwind loop with a `break`,
     * not the native `return` (which would escape the driver, abandoning the
     * live continuation chain).  emit_panic_signal_return honors this flag; the
     * BR3b pre-emit sets it around the reader call and clears it after. */
    bool         panic_signal_is_break;
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

/* VBM3: append the `<lens>__mono_<hash>` symbol (shared by the by-value lens
 * body emit and the poly-call redirect). */
void emit_vl_mono_name(Buf *out, const char *lens_name, unsigned long long hash);

/* CM2: append the `<consumer>__lens_<lenshash>` consumer-clone symbol (shared by
 * the clone emit and the CM3 call-site rewrite). */
void emit_vl_consumer_mono_name(Buf *out, const char *consumer_name,
                                unsigned long long lens_hash);

/* Like emit_carrier_bridge, but for a CK_CONCRETE -> CK_CARRIER crossing whose
 * carrier value is STORED in a heap container that outlives the current
 * expression (e.g. vec-push! / map-set! / set-add! of a by-value aggregate
 * element).  The default bridge spills a by-value aggregate to a stack local
 * and carries (int64_t)(intptr_t)(&tmp) -- a DANGLING address once the
 * producing frame returns (see
 * docs/archive/history/vec-push-byvalue-aggregate-element-stores-dangling-stack-address.md).
 * Here the aggregate is instead heap-promoted: a malloc'd copy is made and the
 * heap pointer is carried, matching how :heap structs are already carried (the
 * reader reconstructs by deref, unchanged).  Every other crossing -- inline
 * scalar, heap-struct pointer reinterpret, pointer-sized leaf, or the
 * carrier->concrete direction -- delegates to emit_carrier_bridge unchanged.
 *
 * Ownership: consumes src_str (frees it when wrapping). */
char *emit_carrier_bridge_escaping(EmitCtx *ctx, Buf *body,
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
/* True when the inline-C body contains a __TUR_CNAME_<name>__ template marker,
 * which expands to the mangled C identifier of the named binding (so inline-C
 * never hardcodes the name-mangling scheme). */
bool inline_c_has_cname_template(const InlineC *ic);
const Expr **flatten_program_items(const Expr *program, uint32_t *out_n);
/* gcc14-int-conversion (carrier-representation-tracking): a side table recording
 * the ACTUAL emitted C parameter-type string of each function, keyed by emitted
 * C name.  Populated from the forward-declaration pass (ground truth, since the
 * monomorphized source type collides between a generic carrier-ABI callee and a
 * concrete-pointer callee).  A call site consults it to bridge int64<->pointer
 * against what the callee's signature really is.  emit_sig_reset() clears it at
 * the start of each program emission. */
void emit_sig_reset(void);
void emit_sig_record_param_ctype(const char *cname, uint32_t idx, uint32_t n_params,
                                 const char *ctype);
const char *emit_sig_lookup_param_ctype(const char *cname, uint32_t idx);
/* S1 (jit-engine-plan section 4): the same side table's return-type half.  A
 * call site consults it to name the type of a hoisted call temp outright,
 * instead of emitting GNU C's `__auto_type` -- which c2mir cannot parse at all
 * and which accounts for 193 of the 256 full-corpus JIT failures. */
void emit_sig_record_ret_ctype(const char *cname, uint32_t n_params,
                               const char *ctype);
const char *emit_sig_lookup_ret_ctype(const char *cname);
/* gcc14-int-conversion (carrier-representation-tracking): a side table recording
 * the ACTUAL emitted C type of each LOCAL variable / temp, keyed by its (globally
 * unique via fresh_tmp) C name.  A binder init `int64_t z = __t169;` reading a
 * control-result temp `__t169` declared `tur_adt_X *` straddles the
 * int64<->pointer duality, but the init EXPRESSION's TYPE c-names to the carrier
 * (so a type-based check under-fires) while keying on the type broadly over-fires
 * (139-fixture churn).  Consulting the temp's REAL emitted C type bridges only the
 * genuine straddle.  emit_localvar_reset() clears it per program. */
/* inline-c-locals-invisible-to-inline-c-blocks: collect the locals of `body`
 * that an inline-C block inside it names, so they can be emitted by their raw
 * source name.  Does not descend into nested fn-defs (those are emitted as
 * their own C functions and run their own collection).  Allocates *out with
 * malloc; the caller frees it.  Yields nothing when the body has no inline C. */
void emit_inline_c_raw_locals_collect(const struct Expr *body,
                                      struct Binding **params, uint32_t n_params,
                                      const struct Binding ***out, uint32_t *out_n);
void emit_localvar_reset(void);
void emit_localvar_record_ctype(const char *cname, const char *ctype);
const char *emit_localvar_lookup_ctype(const char *cname);

/* inline-c-option-carrier-box-leaks: the owned-carrier side table.  A call
 * temp holding a carrier box an inline-C body malloc'd is marked here, and the
 * carrier->concrete bridge frees the box after copying its contents out.  See
 * the table's comment in emit_module.c. */
void emit_owned_carrier_mark(const char *cname);
bool emit_owned_carrier_is(const char *cname);
void emit_owned_carrier_clear(const char *cname);
/* CE2: raw Vec slot-word temps (see emit_module.c). */
void emit_slot_word_mark(const char *cname);
bool emit_slot_word_is(const char *cname);
/* S1 (jit-engine-plan section 4): true when an emitted C type NAME denotes a
 * scalar -- any pointer, or one of the primitive/stdint spellings the emitter
 * produces.  Anything else (a struct typedef such as `Option__int` or
 * `tur_adt_Vec__int`) is reported as non-scalar.
 *
 * Deliberately conservative in that direction: the only consumer picks between
 * `((T)0)` and `(T){0}`, and `(T){0}` is what every site emitted before, so a
 * false "aggregate" verdict is a no-op while a false "scalar" verdict would be
 * a miscompile. */
bool emit_c_type_is_scalar(const char *cname);
/* S1: a zero of `cname`, spelled so c2mir accepts it.  `((T)0)` for scalars,
 * `(T){0}` for aggregates.  Returns a malloc'd string the caller frees.
 *
 * A scalar compound literal is C99-legal and every cc takes it, but c2mir
 * rejects it outright ("braces around scalar initializer", c2mir.c:7781), and
 * the panic-propagation return emits one per hoisted call -- 75-139 per TU. */
char *emit_c_zero_of(const char *cname);
bool emit_str_is_bare_ident(const char *s);
/* cps-let-binder-bridge-lacks-position-check: the single position-level test --
 * is `v` a bare identifier whose RECORDED emitted C type is exactly
 * `want_ctype`, and is that type a by-value aggregate?  Shared by every
 * carrier->concrete bridge so the copies cannot drift.  See emit_expr.c. */
bool emit_value_is_recorded_as(const char *v, const char *want_ctype);
Type emit_type_from_kind(TypeKind k);
Type emit_resolve_type(EmitCtx *ctx, Type t);
const char *emit_type_c_name(EmitCtx *ctx, Type t);
/* Increment 4 stage 3 (repr-decision-function-plan): the shadow instrument,
 * shared across emitting TUs.  `repr_form_from_cty` recovers the ReprForm a
 * site ACTUALLY chose from the C type it declared (`cty`), given the type's
 * own C spelling (`own_cty`); `repr_shadow_report` prints one `repr-shadow`
 * line when that disagrees with `repr_of`'s intended protocol.  Neither ever
 * changes a decision -- the shadow is measurement only. */
ReprForm repr_form_from_cty(Type resolved, const char *own_cty,
                            const char *cty);
void repr_shadow_report(const char *site, ReprPosition pos, Type resolved,
                        ReprForm want, ReprForm got, const char *cty);
/* fn-typed-return: typedef name for a concrete thin function-value return type,
 * or NULL when the return is not such a fn / is a dict-dispatched method impl
 * (see emit_core.c). */
const char *emit_fn_return_typedef(const FnDef *fd, const Type *rft);
/* instance-method-closure-return: carrier for an `__inst_*` method's fn-typed
 * return -- the fn-ptr typedef for a thin body, else the int64_t closure
 * carrier; NULL for non-method / non-fn returns (see emit_core.c). */
const char *emit_inst_fn_return_carrier(const FnDef *fd, const Type *rft);
/* KB-021: arbiter of which struct-valued types may use the int64_t carrier ABI
 * (see emit_core.c). */
bool type_uses_carrier_abi(Type t);
/* opaque-pointer-c-spelling: true when a function's DECLARED result is the
 * int64 carrier word while its BODY is an opaque newtype over a pointer.  The
 * `_body_c` preference (emit_fns.c's definition emitter and emit_module.c's
 * forward-decl mirror) exists so a by-value AGGREGATE body is not squeezed
 * through the declared carrier word; a pointer opaque is not an aggregate, it
 * is one word, and every CALLER sees the declared type.  Preferring the body's
 * `void *` there declares a signature the function's own `return (int64_t)...`
 * statements contradict.  Both emitters ask this so the prototype and the
 * definition cannot disagree. */
bool emit_fn_body_is_opaque_ptr_over_carrier_result(const FnDef *fd,
                                                    TypeKind declared_result);
/* catch-unwind-returned-err-box-payload-leak: true when the already-resolved
 * type is a `(Result A B)` whose err arm B is an inline scalar, so a caught
 * result box returned by value can take the full (payload-reclaiming) free
 * instead of the shallow struct-only free (see emit_core.c). */
bool result_err_arm_is_freeable_scalar(const Type *t);
/* M5 straddle (root cause C): every tail leaf of `e` is a carrier-int64
 * producer call (a #{Construct} helper or an __inst_ method).  Defined in
 * emit_fns.c; consumed there and in emit_module.c's forward-decl mirror. */
bool fn_body_tail_is_carrier_producer(const struct Expr *e);
/* instance-method-return-carrier-bridge: every tail leaf of `e` already emits a
 * by-value concrete carrier-ABI aggregate (post-M2 #{Construct} spec, make-struct
 * literal, by-value var).  Gates off the carrier->concrete return deref so an
 * already-by-value producer is not dereferenced as a heap pointer.  Defined in
 * emit_expr.c. */
bool fn_body_tail_emits_byvalue_carrier_abi(struct EmitCtx *ctx, const struct Expr *e);
Type fn_body_tail_byvalue_carrier_type(struct EmitCtx *ctx, const struct Expr *e);
/* CONV-S1 seam 4: does an inline-C body with declared result `rft` return that
 * result BY VALUE (the concrete aggregate) rather than through the int64
 * carrier?  This is the single question that decides an inline-C function's C
 * return type, so it must be asked the same way everywhere:
 *
 *   - emit_fns.c   -- emits the definition's signature
 *   - emit_module.c-- emits the matching forward declaration
 *   - emit_expr.c  -- decides whether a CONSUMER must bridge carrier->by-value
 *
 * The first two had hand-duplicated copies that had to be kept in lockstep; the
 * third inferred it from the type's shape and got it wrong for a by-value
 * result, deref-ing a value that was never a pointer.  Defined in emit_fns.c.
 * `rft` is the declared (unresolved) result type; pass body_is_inline_c so
 * callers that already computed it do not re-derive it. */
bool inline_c_returns_byvalue_adt(struct EmitCtx *ctx, bool body_is_inline_c,
                                  const Type *rft);
/* B3 part 2: per-effect integer tag (memoized by symbol) used by the DK backend
 * (dk_perform / dk_handler tags).  Defined in emit_cps_ir.c; called from
 * emit_effects.c to stamp a first-class handler entry's DK case tag. */
int effect_tag(const struct Symbol *eff);
/* Phase 5 dead-instance elimination: is this HKT typeclass instance live (any
 * method base directly referenced)?  Used to skip dead instances' dict + bases
 * in lockstep.  Defined in emit_module.c. */
struct TypeClassInstance;
bool emit_instance_is_live(const struct EmitCtx *ctx, struct TypeClassInstance *inst);
/* nested-construct-byvalue: the FnDef a constrained-instance body re-dispatches a
 * return/argument-dispatched method to under the active spec (e.g. the cstr
 * `dec` impl).  Used by the ABI scan to mark that instance live so the emitted
 * spec body's reference to it resolves.  Defined in emit_core.c. */
struct FnDef *emit_reresolve_method_fndef(struct EmitCtx *ctx, const struct Expr *call);
/* G2 (carrier<->concrete nested dispatch): shared dispatch-type resolver -- given
 * a typeclass-method call inside an active spec whose receiver/result element was
 * erased to the carrier, recover the concrete dispatch type (`out_resolved`) and
 * the call's dict (`out_dict`).  Returns false when no re-resolution applies.
 * Exposed so emit_module.c's ABI-registration pass can mint a by-value spec for a
 * redispatched parametric instance method whose receiver is itself a concrete
 * parametric container.  Defined in emit_core.c. */
bool emit_reresolve_disp_type(EmitCtx *ctx, const Expr *call,
                              Type *out_resolved, const Expr **out_dict);
/* RC2 (generic-show-wrapper-cps-monomorphization-plan): re-resolve a carrier-erased
 * typeclass-method call to the concrete per-ABI-spec instance method symbol, or NULL
 * when the call is not a genuine tyvar dispatch inside a spec (self-gating: safe on
 * an ordinary int dispatch).  Shared by the direct emitter and the CPS emitter so a
 * `(show x)` inside a COLORED wrapper clone body dispatches to `__inst_Show_show_<T>`
 * rather than the baked int carrier representative.  Defined in emit_core.c. */
char *emit_reresolve_method_call(EmitCtx *ctx, const Expr *call);
/* R2 (carrier-crossing-recovery-routing-plan): shared first-stage dispatch-tyvar
 * identification -- writes the TY_TYVAR a typeclass-method call dispatches on
 * (ascribed receiver, bare receiver, or result type) into *out, or returns false
 * when the call carries no dispatch tyvar.  The single spelling both the emit-time
 * chokepoint and the scan-time predicate consult.  Defined in emit_core.c. */
bool emit_dispatch_tyvar(const Expr *call, Type *out);
/* R2 (carrier-crossing-recovery-routing-plan): shared constraint-var resolution.
 * Given an instance's `type_param_constraints` and the receiver (class-var)
 * container's type-args (outermost-first) in `elems[0..n_elems)`, map each
 * `where (Class tyvar)` constraint to its concrete element: the constraint's
 * `param_idx` indexes the receiver's type-arg list (e.g. `(Eq A)` with
 * param_idx 0 against receiver elems `[int]` yields `A -> int`).  Constraints
 * with param_idx < 0 (direct type_arg), a NULL tyvar, or an out-of-range
 * param_idx are skipped.  Writes up to `cap` (name,type) bindings into `out`
 * and returns the count.  The param_idx convention lives here so the dispatch
 * chokepoint's constraint-var tail (which matches one entry by name) and
 * emit_abi_register_call's binding augmentation never re-derive it apart.
 * Each caller keeps its own receiver extraction (struct-strict vs any-TY_APP),
 * which is load-bearing for instance selection; only the mapping is shared.
 * Defined in emit_core.c. */
uint8_t emit_abi_constraint_var_bindings(const struct TypeClassInstance *inst,
                                         const Type *elems, uint8_t n_elems,
                                         AbiTypeBinding *out, uint8_t cap);
/* Value-side chokepoint core (carrier<->concrete): given a binding that is a
 * parameter of the active ABI specialization, recover its monomorphized concrete
 * type from `current_abi_specialization->arg_types[]`.  Returns false when there
 * is no active spec or the binding is not one of its params.  This is the single
 * sanctioned `params[pi] == b -> arg_types[pi]` recovery; every emit site that
 * needs a spec-param's concrete type must route through it (directly, or via
 * emit_var_spec_arg_type for an EX_VAR) instead of re-rolling the loop locally.
 * Defined in emit_expr.c. */
bool emit_spec_arg_type_for_binding(EmitCtx *ctx, const struct Binding *b,
                                    Type *out);
/* R3 chokepoint gate (carrier-crossing-recovery-routing-plan): assert a type
 * recovered by a carrier<->concrete recovery chokepoint is concrete before it
 * flows into code emission.  A leftover parametric param (TY_TYVAR in the spine)
 * is a 'forgot to route' hole -- a hard `tur` ICE in Debug (compiled out under
 * NDEBUG/Release; `TUR_ABI_NO_ROUTE_ICE=1` downgrades it to a warning).  Defined
 * in emit_core.c. */
void emit_abi_assert_routed_concrete(EmitCtx *ctx, const Type *recovered,
                                     const char *site, bool deep);
/* G6: true when a call's result type and a candidate spec's result type are
 * distinct PRIMITIVE kinds -- a return-differentiated sibling spec that the
 * by-args lookup must not match.  Defined in emit_expr.c. */
bool emit_spec_result_mismatch(Type call_result, Type spec_result);
/* G2: the concrete instance-method FnDef a class-method call resolves to for a
 * given recovered dispatch type (e.g. `__inst_Enc_enc_Cons` for `(Cons int)`).
 * Defined in emit_core.c. */
FnDef *emit_concrete_inst_method_fndef(EmitCtx *ctx, const TypeClass *tc,
                                       Type resolved,
                                       const char *method_field_name);
/* Debugger Phase 4 (--debug): emit a `#line N "path/to/file.tur"` directive
 * into `out` for `span`, so the C compiler maps the following generated code
 * back to the Turmeric source for gdb/lldb.  A no-op unless g_emit_debug_lines
 * is set, the span carries a line, and its file_id resolves to a path.  Skips
 * a redundant directive when (file_id, line) is unchanged from the last one
 * emitted into this stream (tracked on ctx).  Guarantees the directive starts
 * at column 0 (prepends a newline if mid-line). */
void emit_line_directive(EmitCtx *ctx, Buf *out, Span span);
/* Reset the line-directive dedup tracker so the next emit_line_directive call
 * always emits.  Called at each function-body boundary. */
void emit_line_reset(EmitCtx *ctx);
void indent_buf(Buf *b, int n);
bool expr_is_divergent(const Expr *e);
bool expr_contains_return_or_throw(const Expr *e);
bool expr_tail_diverges(const Expr *e);
/* Fat-closure-env scoped-free escape analysis: true if `b` (holding a fat
 * closure value) is used as a value rather than only as a direct-call callee
 * within `e`.  See emit_core.c. */
bool closure_binding_escapes(const Expr *e, const Binding *b);
/* Does any inline-C block occur under `e`?  (emit_core.c) */
bool expr_subtree_has_inline_c(const Expr *e);
/* catch-unwind-thunk-closure-leak scoped-free escape analysis: like
 * closure_binding_escapes, but a use of `b` as a read-only ok?/err?/ok-val
 * argument is not an escape.  See emit_core.c. */
bool catch_box_binding_escapes(const Expr *e, const Binding *b);
bool sum_box_binding_escapes(const Expr *e, const Binding *b);
/* RM1: the read-only Option/Result accessor family -- shared between the
 * escape walk's whitelist and elab_call.c's drop-after stamp so the two
 * cannot drift. */
bool sum_box_reader_name(const char *nm);
bool sum_param_is_nonretaining(const Expr *body, const Binding *p,
                               bool result_cannot_carry);
/* The pending-drop bracket for a call at STATEMENT position (emit_stmt.c);
 * see emit_pending_drops_mark's comment in emit_expr.c. */
void emit_pending_drops_mark(EmitCtx *ctx, uint32_t m[3]);
void emit_pending_drops_drain(EmitCtx *ctx, Buf *body, const uint32_t m[3]);
/* catch-unwind-return-bridge-residuals (Part B): like catch_box_binding_escapes
 * but the single occurrence `ignore` (the return-tail use the caller is about to
 * copy out and free) is not counted as an escape.  Used to prove a returned
 * caught box is sole-owned -- it escapes nowhere except that return. */
bool any_box_binding_escapes(const Expr *e, const Binding *b);
bool any_box_binding_escapes_except(const Expr *e, const Binding *b,
                                    const Expr *ignore);
bool catch_box_binding_escapes_except(const Expr *e, const Binding *b,
                                      const Expr *ignore);
/* catch-unwind-panic-payload-leaks (Leak 2): admit a deep box free when `b` is
 * read through a reader (not only the scalar-accessor whitelist) but every such
 * reader result is confined to the scope (consumed by a non-retaining print
 * sink), and the scope value itself cannot carry a box-owned pointer out
 * (`scope_result` is a non-pointer scalar / nil).  See emit_core.c. */
bool catch_box_binding_reader_confined(const Expr *body, const Binding *b,
                                       TypeKind scope_result);
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
/* separator-fold-collides-emitted-c-names: injective spelling for ADT and
 * constructor NAMES (typedefs, ctor symbols, member paths, drop glue). */
char *mangle_adt_name(const char *name);
char *mangle_ctor_symbol(const struct AdtDef *adt, const char *ctor_name);
/* duplicate-ctor-names-collide-in-emitted-c: program-wide census of constructor
 * names, so an UNAMBIGUOUS one keeps its bare-name alias for hand-written
 * inline C.  Fed from elab_register_adt_def; see emit_core.c. */
void ctor_census_reset(void);
void ctor_census_snapshot(struct AdtDef *const *defs, uint32_t n_defs);
bool ctor_base_name_is_unique(const char *mangled_ctor_name);
void emit_ctor_bare_alias(Buf *out, const struct AdtDef *def,
                          const struct CtorDef *ctor);
char *adt_field_member_path(const AdtDef *def, const CtorDef *ctor, uint32_t fi);
char *raw_name_for_binding(const Binding *b);
char *emit_call_name(EmitCtx *ctx, const Expr *call, const Binding *b);
/* struct-of-closures monomorphization: find the inner-closure body spec (the
 * primary inner_closure_spec_idx or one of the extra struct-of-closures links)
 * of outer spec `cur` whose lifted-fn binding is `binding`.  NULL when none
 * matches.  The EX_CLOSURE construction and thunk-call emit sites use this to
 * pick the register-class-/layout-correct clone + suffixed env for EACH closure
 * a struct-of-closures return builds, not just the first. */
const EmitAbiSpecialization *emit_inner_closure_spec_for_binding(
        const EmitCtx *ctx, const EmitAbiSpecialization *cur,
        const Binding *binding);
/* MB2.5 (constrained-hkt-forall-mode-b-plan): true when `call` is a class-method
 * call inside a dict-clone body that dispatches through the runtime dict param
 * (the same condition emit_call_name uses to route the call through
 * `((void **)dict)[slot]`).  Such a call speaks the carrier ABI end to end -- the
 * dict slot stores the carrier instance method -- so any by-value aggregate
 * marshalling (M7 spec matching, carrier->concrete arg deref) must be suppressed
 * on it; the aggregate box/unbox happens at the caller (poly-carrier) boundary. */
bool emit_call_is_dict_param_dispatch(EmitCtx *ctx, const Expr *call);
/* forall-dict-pass-multi-constraint-hkt-plan (Task 1.4): index of the dict slot
 * whose class owns the method call, or -1 when it is not a dict-param dispatch. */
int emit_call_dict_param_dispatch_index(EmitCtx *ctx, const Expr *call);
/* forall-dict-pass-nested-mapper-general-plan (Phase 1): index of the captured
 * env-dict slot whose class owns the method call, or -1 when it is not an
 * env-dict dispatch. */
int emit_call_dict_env_dispatch_index(EmitCtx *ctx, const Expr *call);
/* MB2 (constrained-hkt-forall-mode-b-plan): true when a function body tail is a
 * generic (tyvar-returning) call that emits as the int64 carrier and is NOT
 * resolved to a concrete by-value spec -- so a pointer-returning function must
 * bridge int64->pointer at its return.  Defined in emit_expr.c. */
bool emit_tail_call_returns_tyvar_carrier(EmitCtx *ctx, const Expr *e);
/* GHE struct-receiver: true when a constrained-generic method call re-resolved
 * in the active ABI spec targets a struct/ADT-receiver instance whose method
 * takes the receiver by `const T *`, so the by-value receiver arg must be passed
 * by address.  Defined in emit_core.c. */
bool emit_reresolved_receiver_is_by_ptr(EmitCtx *ctx, const Expr *call);
char *name_for_binding(EmitCtx *ctx, const Binding *b);
/* WIN1: emit the binary-stdout prologue for a generated main(). Windows opens
 * stdout in text mode, which would turn every 
 into 
. */
void emit_win_binary_stdio_prologue(Buf *out);

void emit_c_string(Buf *out, StrSlice s);
char *atom_nil(void);
char *atom_bool(bool b);
char *atom_int_typed(int64_t i, TypeKind k);
char *atom_float32(double f);
char *atom_float(double f);
char *atom_var(EmitCtx *ctx, const Binding *b);
/* B7b: is this `^mut` binding promoted to a shared heap cell for the function
 * the CPS backend is currently emitting?  Owned by emit_cps_ir.c (g_byref_muts);
 * exposed so the value-position chokepoint (atom_var) can deref a read no matter
 * which emitter produces it -- a delegated `set!` value expression goes through
 * the direct emitter even inside a CPS-lifted body. */
bool emit_binding_is_byref_cell(const Binding *b);
char *atom_cstr(StrSlice s);
char *emit_builtin(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_dict_name(char *buf, size_t buflen, const TypeClassInstance *inst);
void collect_defined(const Expr *e, Binding ***defs, uint32_t *ndefs, uint32_t *cdefs);
Binding **collect_handle_captures(const Expr *body, uint32_t *n_out);
bool use_typed_thunk_abi(Type result_type, Type *param_types, uint8_t n_params);
/* win64-aggregate-return: the result window where Win64 uses sret and SysV
 * uses registers (a concrete app monomorph, <= 16 bytes, not 1/2/4/8). */
bool type_app_result_win64_sret_only(Type t);
/* use_typed_thunk_abi with `win64_result` admitting that window in result
 * position; the plain form is this with false, so SysV is unchanged. */
bool use_typed_thunk_abi_ex(Type result_type, Type *param_types, uint8_t n_params,
                            bool win64_result);
char *ensure_typed_thunk_typedef(EmitCtx *ctx, Buf *out,
                                 Type result_type, Type *param_types, uint8_t n_params);
/* fn-value-fat-normalization (effect-row increment): fat-closure env struct +
 * drop glue at file scope, deduped via ctx->env_struct_names.  Called from the
 * EX_CLOSURE construction site (emit_expr.c) and from the CPS twin pre-pass
 * (emit_cps_ir.c), whichever runs first. */
void emit_closure_env_struct_and_glue(EmitCtx *ctx, Buf *out,
                                      struct Closure *closure,
                                      const Symbol *env_name,
                                      bool resolve_spec_params);
/* Register / read back the `__fn` field spelling an env struct was emitted
 * with (NULL = `int64_t`).  Every env-struct emit site registers; every
 * `__fn` assignment site reads, so the two cannot disagree. */
void emit_env_struct_register(EmitCtx *ctx, const Symbol *env_name,
                              const char *typedef_name);
const char *emit_env_struct_fn_typedef(EmitCtx *ctx, const Symbol *env_name);
/* closure-typed-invocation-abi-plan: ensure a typed fat-shim exists for the
 * given closure signature, returning its C function name.  Returns NULL when
 * the signature is the all-int64_t carrier case (caller uses the preamble
 * __tur_fatshim<arity> shim instead) -- this keeps int64 fixtures churn-free. */
const char *ensure_static_fatbox(EmitCtx *ctx, const char *shim,
                                 const char *fnptr);
/* Same box with slot 0 chosen by the preprocessor: `win_shim` under _WIN32,
 * `sysv_shim` otherwise.  For the Win64 sret-only result window. */
const char *ensure_static_fatbox_dual(EmitCtx *ctx, const char *win_shim,
                                      const char *sysv_shim, const char *fnptr);
/* Emit the no-op drop glue the static / stack fat boxes carry as their
 * header, once.  False when there is nowhere to put it. */
bool ensure_fatbox_keep(EmitCtx *ctx);
/* catch-unwind-aggregate-return-miscompiled: per-type boxing trampoline for an
 * aggregate-returning catch-unwind / catch-panic-of thunk. */
/* type-of-cast-kind-granularity: the `any` box tag for a type -- its TypeKind
 * for a primitive, an interned per-monomorph id for a struct/ADT. */
int64_t emit_any_type_id(EmitCtx *ctx, Type t);
/* any-struct-box-leak-per-widen: the predicate the `any` widen uses to decide
 * whether a payload is heap-boxed.  Exported so emit_any_type_id can intern the
 * same answer for the drop side -- one predicate, not two that can drift. */
bool emit_type_is_byvalue_adt(EmitCtx *ctx, Type t);
/* Emit the per-program name table `__tur_any_name_ext` for the ids allocated
 * above.  Always emitted (a stub when none were), since the preamble
 * forward-declares it. */
void emit_any_type_name_table(EmitCtx *ctx, Buf *out);

const char *ensure_catch_box_shim(EmitCtx *ctx, Type result_type);
/* ... and the float-return half, which returns the value's BITS. */
const char *ensure_catch_bits_shim(EmitCtx *ctx, Type result_type);

char *ensure_typed_fatshim(EmitCtx *ctx,
                           Type result_type, Type *param_types, uint8_t n_params);
/* ... with `win64_result` widening the result gate to the Win64 sret-only
 * window.  The plain form is this with false. */
char *ensure_typed_fatshim_ex(EmitCtx *ctx,
                              Type result_type, Type *param_types, uint8_t n_params,
                              bool win64_result);
/* arrow-struct-typed-arrow-abi: the erased-carrier fatshim used when the typed
 * shim is declined but a parameter is a wide by-value aggregate -- slot 0 keeps
 * the `int64_t (*)(void *, int64_t...)` spelling the erased call site casts to,
 * unboxing each b4box parameter and boxing a wide result.  NULL when the
 * signature is not in that set (the generic `__tur_fatshim<arity>` stands). */
char *ensure_carrier_fatshim(EmitCtx *ctx,
                             Type result_type, Type *param_types, uint8_t n_params);
/* constrained-byval dispatch: ensure a carrier-adapter witness dict exists for a
 * by-value struct payload boxed into a constrained existential, returning the
 * dict's base name (caller references `&<name>_singleton`).  Each method slot is
 * a thunk taking the receiver as the int64 carrier (the heap-box pointer),
 * dereferencing it to the concrete struct, and forwarding to the real instance
 * method through its dict singleton -- so EX_EXISTS_DISPATCH's carrier ABI lines
 * up with the instance's by-value-struct ABI.  Returns NULL when the instance is
 * not adaptable this way (a method that returns the class variable needs an
 * inverse re-box that is not implemented); the caller then falls back to the
 * real dict. */
char *ensure_exists_byval_witness_dict(EmitCtx *ctx,
                                       const TypeClassInstance *inst,
                                       Type payload_ty);
/* M7 carrier-spill shim: a poly thunk (`real_fn`) that RETURNS a by-value
 * aggregate (e.g. a Monad continuation returning `Option__int`) cannot be cast
 * to the int64 `tur_poly_fn_t.fn` ABI without corrupting the struct return.
 * Emit a wrapper `int64_t shim(void*__e, P0..) { Aggr r = real_fn(__e, a0..);
 * void*p=malloc(sizeof r); memcpy(p,&r,sizeof r); return (int64_t)p; }` so the
 * aggregate is boxed to the int64 carrier (layout-compatible with the carrier
 * the consumer reads back).  Returns the shim name, or NULL when result_type is
 * not a by-value aggregate (the plain int64 carrier needs no shim). */
char *ensure_aggregate_spill_shim(EmitCtx *ctx, const char *real_fn,
                                  Type result_type, Type *param_types,
                                  uint8_t n_params);
/* nested-bind-over-result-typed-boundary: the fat-closure twin of the above.
 * A CAPTURING continuation has no named wrapper, so the shim is keyed on the
 * signature and reads the real entry point out of the closure env's `__fn`
 * slot (offset 0) at run time.  Returns the shim name, or NULL when the return
 * already rides the int64 carrier or a param is not int-register-class. */
/* Adapter for a `:fn` value carried as a BARE C function pointer (a let-bound
 * non-capturing lambda) rather than a closure box.  Stashes the pointer in the
 * tur_poly_fn_t env slot and casts it back to its env-less signature.  See the
 * definition in emit_module.c. */
char *ensure_bare_fnptr_poly_shim(EmitCtx *ctx, Type result_type,
                                  Type *param_types, uint8_t n_params);
char *ensure_fat_aggregate_spill_shim(EmitCtx *ctx, Type result_type,
                                      Type *param_types, uint8_t n_params);
/* erased-float-carrier: shims that carry a float-class param/result of a poly
 * thunk as its BITS in the int64 carrier at the positions an erased typeclass-
 * method sink reads as int64 (`erased_mask` bit i / `erased_result`).  The
 * named form wraps `real_fn` (a __poly_ wrapper); the fat form is keyed on the
 * signature and reads the closure's `__fn` out of slot 0 of its env.  Both
 * return NULL when no erased position is float-class (the plain wrapper /
 * typed thunk already agrees with the consumer).  Defined in emit_module.c. */
char *ensure_float_carrier_shim(EmitCtx *ctx, const char *real_fn,
                                Type result_type, Type *param_types,
                                uint8_t n_params, uint64_t erased_mask,
                                bool erased_result);
char *ensure_fat_float_carrier_shim(EmitCtx *ctx, Type result_type,
                                    Type *param_types, uint8_t n_params,
                                    uint64_t erased_mask, bool erased_result);
/* E2 (fat-closure fn-value threading): ensure a `<wrapper>__cps` twin exists for
 * a poly-wrap thunk boxing an effectful named fn, DK-threading its call through
 * the direct->CPS registry.  Returns the malloc'd twin name (caller uses it for
 * the tur_poly_fn_t.fn_cps slot), or NULL when already emitted. */
char *ensure_poly_wrap_cps_thunk(EmitCtx *ctx, const char *wrapper_name,
                                 const char *inner_fn);
/* poly-to-fat-typed-shim-plan: ensure a typed poly-to-fat shim exists for the
 * given (result, arg0..argN) method signature, returning its C function name.
 * Returns NULL for the all-int64_t carrier case (caller uses the preamble
 * __tur_poly_to_fat<N> shim instead), keeping int64 poly boxes churn-free. */
char *ensure_typed_poly_to_fat(EmitCtx *ctx, Type result_type,
                               const Type *arg_types, uint32_t n_args);
/* erased-float-carrier: the typed poly-to-fat shim for a box whose stored thunk
 * speaks the int64 carrier at `erased_mask` / `erased_result` (a method param
 * `g : (fn [a] b)` inside the ERASED instance base body, where the pack site
 * bridged a float-class wrapper through its bits).  Bridges those positions
 * back from bits to the sink's native float type; the rest forward as the
 * plain typed shim does.  Returns NULL when no erased position is float-class
 * (the plain typed shim already matches). */
char *ensure_typed_poly_to_fat_erased(EmitCtx *ctx, Type result_type,
                                      const Type *arg_types, uint32_t n_args,
                                      uint64_t erased_mask, bool erased_result);

/* ------------ emit_effects.c: effects/CPS expression emission ------------ */
/* Region C -- algebraic effects (EX_DEFECT, EX_PERFORM, EX_HANDLE, EX_RESUME,
 * EX_DISCONTINUE).  Expression-position emission only; emit_program runtime
 * fragments remain in emit_module.c. */
char *emit_effects_defect(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_handle(EmitCtx *ctx, Buf *body, const Expr *e);
/* FH2-FH5: first-class handler values */
char *emit_effects_handler_lit(EmitCtx *ctx, Buf *body, const Expr *e);
char *emit_effects_compose_handlers(EmitCtx *ctx, Buf *body, const Expr *e);
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
/* any-struct-box-leak-per-widen: enclosing-scope `any` drops (see emit_expr.c). */
void any_scope_drops_push(EmitCtx *ctx, const char *name);
void any_scope_drops_pop(EmitCtx *ctx, uint32_t mark);
void emit_any_scope_drops(EmitCtx *ctx, Buf *body);
/* any-struct-box-leak-per-widen: does let-binding `idx` hold an `any` whose
 * payload box that scope owns and may drop?  Exported because emit_tail emits a
 * tail-position `let` inline rather than through emit_let_value. */
bool let_binding_any_freeable(EmitCtx *ctx, const Expr *e, uint32_t idx);
void emit_temp_decl(EmitCtx *ctx, Buf *body, Type type, const char *name, const char *init_or_null);

/* True when a handle's sole case is the built-in `Unsafe` effect -- a pure
 * compile-time marker that is never performed, so its fiber-lift never
 * suspends and the body can be emitted directly in place (preserving the
 * body's real C type instead of truncating a by-value struct result through
 * the fiber's int64_t result slot).  See emit_effects_handle. */
bool emit_handle_is_pure_unsafe(const struct HandleExpr *h);

/* ------------ emit_stmt.c: statement-position emission ------------ */
void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_while_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_set_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_set_deref_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
void emit_set_field_stmt(EmitCtx *ctx, Buf *body, const Expr *e);

/* ------------ emit_fns.c: function-definition emission ------------ */
void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e);

/* dead-base-thunk-chain-references-undefined-ctor: register a suffix-less
 * reference to the (never-defined) base ctor of a heap parametric ADT, and
 * flush the accumulated static trap definitions into a pre-body band.  See
 * the EmitCtx.dead_base_ctor_* comment. */
void emit_note_dead_base_ctor(EmitCtx *ctx, const char *mangled, uint32_t n_args);
void emit_flush_dead_base_ctor_traps(EmitCtx *ctx, Buf *out);
/* G4: reset the per-module shared-driver group registry. */
void gs_reset_group_registry(void);

#endif
