#ifndef TUR_EXPR_H
#define TUR_EXPR_H

#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "buf.h"
#include "forms.h"   /* Span */
#include "symbols.h"
#include "types.h"
#include "lifetimes.h"  /* Phase 13: Lifetime annotations */
#include "typeclass.h"  /* Phase 15: Typeclass constraints */
#include "effect.h"    /* Phase 19: Algebraic effects */

/* Bit for argument index `i` in a per-arg uint64 bitmask (poly_arg_mask,
 * poly_agg_arg_mask).  Yields 0 for i >= 64 so ordinary functions with more
 * than 64 parameters do not trigger an undefined shift -- their masks are 0,
 * so no bit is ever needed past 63.  Only rank-2-poly/aggregate-carrier args
 * set bits, and a single call carries at most 64 of those. */
#define ARG_IDX_BIT(i) ((i) < 64 ? ((uint64_t)1 << (i)) : (uint64_t)0)

/* Forward declarations. */
typedef struct Expr        Expr;
typedef struct Binding     Binding;
typedef struct BuiltinSpec BuiltinSpec;
typedef struct FnDef       FnDef;      /* Phase 2: function definition */
typedef struct ExternC     ExternC;    /* Phase 2: extern C declaration */
typedef struct InlineC     InlineC;    /* Phase 2: inline C block */

/* UT1: Alias state for uniqueness checking.
 * AS_UNIQUE: no live aliases for this binding.
 * AS_ALIASED: at least one other live binding refers to the same value. */
typedef enum AliasState {
    AS_UNIQUE  = 0,  /* Default: no aliases */
    AS_ALIASED = 1,  /* One or more aliases exist */
} AliasState;

/* ST1: Usage state for substructural discipline checking.
 * Tracks how many times a binding has been referenced.
 * USAGE_UNUSED == 0 so zero-initialised Bindings start unused. */
typedef enum UsageState {
    USAGE_UNUSED    = 0,  /* Not yet used */
    USAGE_USED_ONCE,      /* Used exactly once */
    USAGE_USED_MANY,      /* Used two or more times */
} UsageState;

/* A Binding is the resolved target of a `let`/`def`/`defn` name introduction.
 * Bindings are owned by the elaborator and live in the arena. */
struct Binding {
    const Symbol *name;
    Type          type;
    bool          is_mut;
    bool          is_global;     /* top-level def vs. local let */
    bool          is_param;      /* function/extern parameter binding */
    /* True when this binding was introduced by destructuring an ADT
     * constructor pattern in a `match` arm (e.g. `f`/`g` in `(AddF f g)`).
     * collect_free_vars uses it to capture a function-typed match-arm payload
     * invoked as a callee inside an inner closure -- the same env-capture a
     * function-typed parameter gets -- without also capturing a letrec/named-let
     * self-recursive fn binding (which is neither param nor match binding).
     * See hkt-cata-function-typed-carrier-not-threaded. */
    bool          is_match_binding;
    /* True when this binding was introduced by a `letrec`/named-let binding
     * group.  collect_free_vars uses it (together with the active
     * `letrec_self_group` exclusion set) to capture a letrec-bound fn that is
     * invoked as a callee from inside a NESTED closure -- which must reach the
     * outer closure value through its env -- while still leaving a *direct*
     * self/mutual-recursive call in the init's own top-level body to the
     * recursion machinery (it is excluded, never captured).  See
     * hkt-matcher-cata-fnarg-on-toplevel-defn-and-env-struct-collision (Edge 1). */
    bool          is_letrec_binding;
    uint32_t      id;            /* unique within the program */
    Span          span;
    /* TY4: lexical scope depth at declaration (0 = outermost). Stamped by
     * binding_new from the live scope chain; the borrow-escape check compares
     * a borrow referent's depth against where the borrow lands. */
    uint32_t      scope_depth;
    /* Phase 3: For closure bindings, this points to the thunk function binding */
    struct Binding *closure_fn_binding;
    /* generic-closure-return-type-app (Defect B): for a call-head temp
     * (`((pure 5))` -> `(let [__call_head_N (pure 5)] (__call_head_N))`), the
     * head INIT expression that produced the closure value.  The emit-side
     * thunk direct-call uses it to find which OUTER spec the init resolved to
     * (via the specialized-call registry) and target that spec's inner-closure
     * CLONE instead of the shared generic base thunk -- the base bakes the
     * un-monomorphized body (a `ctor_Cons` that is never emitted).  NULL
     * everywhere else. */
    struct Expr *closure_head_init;
    /* fn-value-fat-normalization (effect-row increment): for a `__borrowc`
     * hoist temp of a CAPTURING closure, the lifted lambda's binding.  A
     * dedicated field, NOT closure_fn_binding -- that one carries direct-call
     * semantics at emit (the thunk direct-call path) and setting it on the
     * hoist temp reroutes pure fat dispatch.  Read only by the CPS coloring /
     * threadability walks, which must resolve the temp back to the lambda. */
    struct Binding *hoist_closure_fn_binding;
    /* Returned-closure metadata: if evaluating this binding yields a closure value,
     * this points at the closure's thunk binding. */
    struct Binding *returns_closure_fn_binding;
    /* poly-closure-result-specialization: true when the returned inner closure's
     * body fat-dispatches a captured (ptr<void>/fn-typed) closure -- i.e. its
     * intermediate result types are erased to the int64 carrier and cannot be
     * resolved to a float per-monomorphization clone.  Such a body is NOT
     * register-class-safe to specialize (the emit trigger skips it; elab keeps
     * the TUR-E0705 hard error for a float binding rather than miscompile). A
     * dispatch-free inner body (e.g. `(fn [t] : A val)` returning a captured
     * value) is safe and IS specialized. */
    bool closure_return_dispatches;
    /* poly-closure-inner-dispatch-result-erased (Direction 3): true when the
     * returned inner closure's body dispatches through a binding that Direction 3
     * cannot handle (TY_PTR_VOID bare-fat, or TY_FN without a named-tyvar
     * result_full_type).  Used to gate TUR-E0705 and the inner-spec trigger:
     * if this is false (all dispatches are typed-fn with recoverable result),
     * Direction 3 in emit_expr.c can derive the correct dispatch C type. */
    bool closure_return_dispatches_untyped;
    /* let-bound-sf-loses-outer-arg-type: true when this function's *return value*
     * is itself a fat closure box (its body evaluates to a capturing closure),
     * as opposed to a thin function pointer that merely returns a closure when
     * called.  Lets a let-binding of (f ...) decide between marking the result a
     * closure value (closure_fn_binding) vs a thin closure-returning fn
     * (returns_closure_fn_binding). */
    bool          returns_boxed_closure;
    /* Phase 5: Move semantics - whether this ref binding has been moved */
    bool          is_moved;
    /* local-struct-drop (fn-field): set by the elaborator's byvalue-struct-field
     * drop pass when this let-bound by-value struct local (a) passes the same
     * moved/consumed/escape guards that admit an rc/ref field auto-drop and (b)
     * owns >=1 BOXED fn-field.  Unlike rc/ref fields (freed via an injected
     * `(defer (drop! (.f o)))`), the fn-field box is freed at scope exit by the
     * DIRECT emitter (emit_let_value -> `drop_fnfields_<T>(&o)`), NOT a defer:
     * a `(defer (drop! (.fn o)))` reads a fat-fn field that the CPS/DK backend's
     * continuation-capture admission rejects, evicting a colored fn to the
     * retired direct/fiber path.  Emitting the free directly keeps colored
     * functions untouched (CPS lowering never runs emit_let_value; the box leaks
     * there exactly as it did before local fn-field drops existed) while
     * uncolored functions release it. */
    bool          drops_fn_fields;
    /* Phase 11: span of first move for note chaining diagnostics */
    Span          moved_at;
    /* Phase R5: #[no-unwind] attribute on defn */
    bool          no_unwind;
    /* #[used]: retain this defn with external C linkage under separate
     * compilation, even when it is unexported and unreachable through the
     * Turmeric call graph. Needed for defns reached only via their mangled C
     * symbol -- hand-written cross-module inline-C bridges and C-ABI callbacks
     * taken by address (Arrow release fns, qsort comparators, signal handlers).
     * Without it the unexported defn is demoted to `static` and the raw
     * `extern <mangled>` reference in another TU dangles at link time. */
    bool          retain_c_linkage;
    /* Phase T25: true if this binding is an algebraic-effect continuation (k in handle cases).
     * Used to detect continuation escape into async blocks at compile time. */
    bool          is_continuation;
    /* Phase M1: Module visibility */
    bool          is_exported;          /* listed in module's (export ...) */
    /* G3 (mutable-globals-plan §4.3): this
     * global was exported as `(export (mut g))`, i.e. its module explicitly
     * permits writes from outside.  A plain `(export g)` exports it READ-ONLY:
     * importers may read it, only the defining module may `set!` it.
     *
     * The permission lives at the DEFINITION site, not the use site, so the
     * decision sits with the code that owns the invariant -- the same reason
     * `:sealed` is declared on the opaque rather than asserted by its
     * consumers. */
    bool          is_export_mut;
    /* G4a (mutable-globals-plan §4.4): a
     * `^atomic ^mut` global.  Every read lowers to TUR_ATOMIC_LOAD_* and every
     * `set!` to TUR_ATOMIC_STORE_*, sequentially consistent.
     *
     * Scalars only.  The macro layer takes a POINTER, so unlike thread-local
     * storage this works under the JIT unchanged: atomicity is an OPERATION on
     * storage the JIT already owns, where TLS is storage the host would have to
     * own (see src/runtime/tur_tls.c's header for the same distinction).
     *
     * Does NOT imply `^mut` -- decided 2026-08-05.  `^mut` is the single gate
     * for `set!`, and a second route would make `^atomic` the only annotation
     * conferring write permission as a side effect. */
    bool          is_atomic;
    /* G4b (mutable-globals-plan §4.4, §11.4):
     * a `^thread-local` global.  Each thread gets its own copy, materialized on
     * first access and initialized by running the declared initializer ON THAT
     * THREAD -- which is the whole point: `(def ^thread-local buf (make-buf))`
     * must give each thread its OWN buffer, not share one.
     *
     * Not `__thread`.  C has no dynamic thread-local initialization (a
     * non-constant `__thread` initializer is "initializer element is not
     * constant"), and c2mir has no thread-local storage at all -- it parses
     * `_Thread_local`, warns, and treats the variable as an ordinary global, so
     * every thread would silently share one slot under `tur jit`.  The lowering
     * is a `pthread_key_t` instead: a libc call needs no compiler TLS support,
     * and the key's destructor frees the per-thread block on thread exit, which
     * `__thread` in C would not give us.  See the plan's §11. */
    bool          is_thread_local;
    const Symbol *defining_module_name; /* owning module's name, or NULL for top-level */
    /* Phase M6: explicit C symbol name from ^:export-as attribute, or NULL */
    const char   *c_export_name;
    /* B6 (cps-tramp-resume): true for a typeclass INSTANCE METHOD.  Its
     * c_export_name is an INTERNAL mangled name (`__inst_<class>_<method>_<ty>`)
     * pinned only so the definition matches the dict-slot use site -- NOT a
     * user-facing ABI export.  The CPS/DK backend may therefore emit a `__cps`
     * variant for an effectful instance method (its exported direct entry still
     * backs the dict slot), unlike a genuine `^:export-as` symbol. */
    bool          is_instance_method;
    /* True when the elaborator minted this binding's NAME, rather than a
     * person writing it: a lifted anonymous lambda (`__fn_774`) or a
     * typeclass instance method (`__inst_Eq_eq_qu_int`).  There is no source
     * form a user could navigate to and no name they could have typed, so
     * every surface that enumerates program symbols for a human -- LSP
     * completion, documentSymbol, workspace/symbol -- must skip these.
     *
     * Deliberately its own bit rather than `is_instance_method ||
     * is_lifted_lambda`: those two are *specific* facts consulted by the
     * emitter and the alias rule, and a consumer asking "did a person write
     * this?" should not have to enumerate every species of synthesized
     * binding.  A future mint site opts in with one assignment.
     *
     * Not the same as a `__` prefix.  The stdlib uses that spelling for its
     * own hand-written internal helpers (`__arrow_pair_first`), which do have
     * a source form, are worth hovering, and belong in their own file's
     * outline.  See docs/archive/lsp-completion-internal-symbols.md. */
    bool          is_synthesized;
    /* Phase P3: HAMT lowering - whether this binding is ^persistent (immutable map) */
    bool          is_persistent;
    /* LT1: Linear type checking — whether this binding holds a linear value */
    bool          is_linear;
    /* LT1: whether the linear value has been consumed (moved/used) */
    bool          is_linear_consumed;
    /* Theme 1 (ref<T> deref/auto-drop): true when this ref binding is a
     * non-owning view obtained from `ref/from-rc` -- it shares the rc's
     * payload and must NOT auto-drop.  Such a ref relies on deref being a
     * consumption (it cannot be discharged any other way), so elab_deref
     * leaves it consuming; owning refs (fresh `(ref ...)` or moved) get the
     * non-consuming deref + scope-exit auto-drop instead. */
    bool          is_nonowning_ref;
    /* LB1: ^borrow -- whether this parameter borrows (reads without consuming)
     * its linear/affine argument.  A non-consuming accessor (fs/tmpfile-path,
     * mutex-lock, ...) declares its handle param ^borrow so a later consuming
     * op (fs/tmpfile-free) remains the single legal consumption.  See
     * docs/archive/history/stdlib-linear-handle-borrows.md. */
    bool          is_borrow;
    /* UT0: Uniqueness type -- whether this binding holds a unique value */
    bool          is_unique;
    /* UT0: whether the unique value has been consumed (moved/aliased) */
    bool          is_unique_consumed;
    /* ST0: Substructural annotations — ^affine and ^relevant */
    bool          is_affine;       /* true if annotated with ^affine (no duplication) */
    bool          is_relevant;     /* true if annotated with ^relevant (must be used) */
    bool          is_fat;          /* A#1: ^fat -- param consumes a fat closure */
    /* ST1: Usage tracking for substructural discipline checking */
    UsageState    usage_state;     /* how many times this binding has been referenced */
    /* UT1: alias tracking -- current alias state for this binding */
    AliasState    alias_state;
    /* UT1: name of the binding that aliased this one (for TUR_E0200 message), or NULL */
    const Symbol *alias_name;
    /* Phase HRT1: rank-2 polymorphic function parameter */
    bool          is_poly_fn;     /* true if this binding is a rank-2 poly fn param */
    const struct Type *poly_type; /* full TY_FORALL type, NULL if not rank-2 */
    /* Phase HRT4: for let-bound aliases of global functions, tracks the original
     * function binding so poly_arg_fn_binding can find the callable C name. */
    struct Binding *source_binding;
    /* ER6: true if this binding was introduced by an (extern-c ...) declaration.
     * Used by effect_check to infer #{Unsafe} for calls to extern-c functions. */
    bool          is_extern_c;
    /* DV0: true if this binding was introduced by (defdynamic ...).
     * Used by DV1 binding/set! dispatch to distinguish dynamic vars from plain locals. */
    bool          is_dynvar;
    struct DynVarEntry *dynvar_entry; /* non-NULL iff is_dynvar */
    /* F4 (cross-plan-followups): ^deprecated annotation on a defn/def.
     * Each use site (EX_VAR resolution) emits a DIAG_WARNING with the
     * stored message (or a generic one if message is NULL).  Suppressed
     * for self-recursive references via e->current_fn_name. */
    bool          is_deprecated;
    const char   *deprecation_message;   /* NUL-terminated, arena-owned, or NULL */
    /* M2a (end-to-end-monomorphization-plan): true if this binding's defn was
     * annotated with `#{Construct}`. The constructor's body is synthesized by
     * the codegen as a direct by-value struct construction per ABI spec,
     * rather than going through the int64 carrier helper in the inline-C
     * body. The inline-C body is retained as a fallback for the existential /
     * carrier-return contexts where the call site genuinely wants an int64
     * handle. Generalizes the by-name `ok`/`err` synthesis from Prereq 6. */
    bool          is_construct_template;
    /* M5 residual-straddle retirement (docs/artifacts/m5-residual-straddle-
     * retirement.md): true if this binding's defn was annotated with
     * `#{ByVal}`. Forces emit_abi_intern_spec to mint by-value specs for
     * TY_APP arg types that would otherwise be rejected by the
     * `arg_types[i].kind == TY_STRUCT` gate at emit_module.c.
     *
     * Scaffolding for the M5 transitional window: marks helpers introduced
     * for Path A spec bodies (e.g. `vec-get-byval`, `vec-eq-loop-byval`)
     * whose inline-C bodies cannot compile under carrier semantics.  Goes
     * away once M5-proper's context-aware gate lands -- at that point the
     * by-value preference flows from the calling spec body and the marker
     * is redundant. */
    bool          prefer_byvalue_spec;
    /* MF3 (test-suite-cleanup-plan): true if this binding came from an
     * auto-loaded stdlib module. Set during the M7 promotion in
     * elab_toplevel.c. Used by elab_defn to hard-error on user code that
     * shadows a stdlib name (which would otherwise produce conflicting
     * static functions of the same C name and break the C compile). */
    bool          is_from_stdlib;
    /* KB-021: true when this binding's emitted C value is a *by-value* concrete
     * carrier-ABI aggregate (e.g. a `Tuple2__int__int`/`Cons__int` local or
     * parameter) rather than the int64_t carrier.  Carrier-ABI types have two
     * coexisting C representations; the dictionary-dispatch callsite consults
     * this flag to decide whether a var argument must be bridged to the carrier
     * before the call.  Set at the binding's declaration site (let binding or
     * function parameter emission). */
    bool          emit_byvalue_carrier_abi;
    /* CONV-S1 seam 4 (carrier-held by-value-ADT receiver): true when this
     * parameter's C signature was emitted as the int64 carrier (e.g. a typeclass
     * instance method's dispatch class-var param) even though the binding's
     * elaborated type is a by-value aggregate ADT (`(Option cstr)` under the
     * defstruct-as-defadt lowering).  A field read off such a receiver must cast
     * the int64 carrier to the concrete monomorph pointer and deref, not read the
     * field off the by-value aggregate.  Set in the param emitter (emit_fns.c). */
    bool          emit_carrier_holds_byval;
    /* MB2 (constrained-hkt-forall-mode-b-plan): true when this parameter's C
     * signature was emitted as the int64 carrier (the uniform poly-carrier ABI of
     * a dict-clone) even though the binding's elaborated type is a concrete
     * POINTER-shaped value (a :heap ADT like `Point`, or a raw ptr).  Field reads
     * already bridge such a receiver via the heap-ADT-recv path, but a closure
     * that CAPTURES the param stores it into an env field typed at the concrete
     * pointer C type (`tur_adt_Point *`), so the capture assignment must cast the
     * int64 carrier through intptr_t or it is a -Wint-conversion.  Set in the
     * param emitter (emit_fns.c), consulted at the capture site (emit_expr.c). */
    bool          emit_carrier_holds_ptr;
    /* True when this binding names a defn whose body is an EX_INLINE_C block.
     * The formal-param emitter (emit_fns.c:423) treats inline-C defns as
     * by-value for struct params even when they would normally cross the
     * 16-byte pass-by-pointer threshold (so inline-C can write `opts.field`).
     * The call-site emitter (emit_expr.c, around the EX_CALL `&temp`
     * transform) consults this flag to suppress the matching `&temp`
     * wrap, keeping the two ABI emitters in sync. Set in elab_fns.c when
     * the FnDef is built. */
    bool          body_is_inline_c;
    /* class-defn-constraint-not-discharged-at-call-site: backlink to the owning
     * FnDef's typeclass constraint set (`^Encode T`, or the `[(Encode T)]`
     * middle-vector form), or NULL for a binding with no constraints.  Stamped
     * by elab_fns.c right after the FnDef stores its constraints, so the call
     * site (elab_call.c) can re-discharge each obligation against the concrete
     * type its arguments pin -- the constraint is checked abstractly in the
     * body but must be re-checked when the defn is instantiated. */
    const ConstraintSet *fn_constraints;
    /* RT1 (refinement-types-plan): the defn's per-parameter refinement
     * predicates, for the CALL-SITE crossing check.  All four arrays have
     * `n_refine_params` entries -- one per declared parameter, in order -- and
     * a parameter with no refinement has a NULL `refine_param_preds` slot.
     * `refine_param_names` is kept because a predicate may mention a SIBLING
     * parameter (`[n : int, i : #refine{ j : int | (< j n) }]`), which the call
     * site must replace with the corresponding argument.
     *
     * Stamped by elab_defn onto the binding -- which for a top-level defn is
     * the same object pass 1 forward-declared -- and read by the deferred
     * resolution pass that runs after ALL elaboration.  Deferring is what makes
     * the check order-independent: a call to a function defined later in the
     * file is checked exactly like a call to one defined earlier. */
    const struct Form **refine_param_preds;
    const char        **refine_param_vars;
    const char        **refine_param_names;
    uint32_t            n_refine_params;
    /* C2 / #reads: bitmask of the ^borrow parameters whose mutable state this
     * function's body reads (`#reads w` or `#reads [w g]`), bit i == param i,
     * or 0 for none.  A zeroed Binding therefore defaults to "no #reads".
     * Parameters beyond bit 63 cannot be named (TUR-E0024); an arity that high
     * is already a TUR-W0041 lint.
     *
     * Was a single 1-based index until 2026-08-18 -- see
     * docs/archive/reads-frame-cannot-name-multiple-params.md.  The encoder
     * grants a measure congruence inside a frozen region only when EVERY
     * named parameter is frozen; see docs/guides/stateful-refinements-guide.md.
     * Stamped on the same forward-declared Binding as the refine_* fields, so
     * it is visible to call sites elaborated before the defn. */
    uint64_t            reads_params_mask;
    /* R2 + R4 slice 1 (trusted-refinement-claims-plan): positive evidence
     * that this function's `#reads` frame is broken -- the elaborated body
     * directly reads a mutable global the frame cannot name, or mutable
     * state rooted in a PARAMETER the frame omits.  Stamped where the frame
     * is stamped; TUR-W0383 reports it, and the refinement encoder refuses
     * the congruence override on it.  False never means "clean", only "no
     * evidence" -- an
     * inline-C body is unwalkable and stays false by design. */
    bool                reads_frame_omits_state;
    /* R4 slice 2 (trusted-refinement-claims-plan): the read-side mirror of
     * `writes_checked`.  True only when the deferred rf_resolve_read_frames
     * pass saw the WHOLE elaborated body and attributed every read of
     * mutable state to a frame-named parameter -- silence is never enough,
     * so an inline-C body, an unvouchable call, or an unmodeled form all
     * leave it false (UNVERIFIED, not broken).  Nothing consumes it
     * behaviorally yet -- `--dump-read-frames` is the surface, and the first
     * real consumer is a post-v1 decision recorded in the plan. */
    bool                reads_checked;
    /* WF1 / #writes: the per-argument WRITE frame -- which parameters' mutable
     * state this function's body may write.  A bitmask (bit i = parameter i is
     * in the frame), the same shape `reads_params_mask` above has carried
     * since 2026-08-18 (it was a single 1-based index before that).
     *
     * `writes_declared` is what carries the meaning, and the two states are NOT
     * interchangeable:
     *   - not declared (`writes_declared == false`) -- UNKNOWN.  Assume the
     *     body may write anything; this is the conservative default a
     *     memset-zeroed Binding gets, and it is what every function that
     *     predates WF1 means.
     *   - declared with an empty mask (`#writes []`) -- writes NOTHING.  A
     *     positive, checkable claim, and the strongest frame there is.
     * Collapsing these two into "mask == 0" would silently upgrade every
     * un-annotated function to "writes nothing", which is exactly the stale-
     * hypothesis-proves-a-fresh-lie failure mode WF3 guards against.
     *
     * Capped at 32 parameters by the mask width; a `#writes` naming a parameter
     * beyond that is rejected (TUR-E0378) rather than silently dropped.
     * See docs/archive/checked-write-frames-plan.md (WF1/WF2). */
#define WF_MAX_FRAME_PARAMS 32u   /* mask width; see writes_param_mask below */
/* G2: globals in a write frame are a list, not a mask -- there is no natural
 * index for them -- so the cap is a list length rather than a word width.
 * Chosen to match RT_WF3_MAX_TARGETS's order of magnitude; a frame naming more
 * than this is rejected, never truncated, for the same reason a parameter past
 * the mask width is. */
#define WF_MAX_FRAME_GLOBALS 16u
    uint32_t            writes_param_mask;
    bool                writes_declared;
    /* WF2: true when this function's frame was VERIFIED against its body (a
     * body with no inline C), false when it is trusted-with-declaration (an
     * inline-C body, which the frame walk cannot see into).  Only a checked
     * frame may back an optimization -- elision, reordering, CSE all act on the
     * claim, so they need it checked rather than promised.  WF3's callee-frame
     * widening and WF4's entry-check elision both gate on this bit. */
    bool                writes_checked;
    /* G1 (docs/upcoming/mutable-globals-plan.md): does this function's body
     * write a MUTABLE GLOBAL, directly or through a callee?
     *
     * The `#writes` frame's vocabulary is PARAMETERS.  A global is written by
     * name rather than passed, so it is outside that vocabulary entirely and a
     * frame can neither name it nor exclude it -- which meant a body declaring
     * `#writes []` ("writes nothing") could mutate global state and still be
     * stamped VERIFIED.  VERIFIED is what an optimization may act on, so that
     * claim has to stop being available to a body that writes a global.
     *
     * Kept SEPARATE from `writes_param_mask` for the same reason
     * `writes_declared` is separate from the mask: the two questions have
     * different vocabularies and collapsing them would make one of the answers
     * mean something it does not.  This bit only ever DOWNGRADES a verdict
     * (VERIFIED -> UNVERIFIED); it never produces a diagnostic, because a
     * global write is outside the frame's vocabulary rather than outside the
     * declared frame -- "I cannot check this" is not "you did something
     * wrong". */
    enum WritesGlobal {
        WG_UNCOMPUTED = 0,  /* memo empty; a zeroed Binding starts here */
        WG_NO,              /* walked the body and every resolvable callee */
        WG_YES,             /* a global assignment was seen */
        WG_UNKNOWN,         /* something could not be vouched for */
        WG_IN_PROGRESS,     /* on the current recursion stack (cycle guard) */
    }                   writes_global;
    /* G2 (mutable-globals-plan §4.2): the
     * globals this function's `#writes` frame DECLARES, and (memoized beside
     * the verdict above) the globals its body actually writes.
     *
     * Two lists rather than one because they answer the two halves of the same
     * question the parameter mask answers for arguments: declared-but-unwritten
     * is fine (a frame is an upper bound), written-but-undeclared is
     * TUR-E0382.  Symbols rather than Bindings -- a global is identified by
     * name at the frame site, and the walk that collects the written set works
     * on forms.
     *
     * `writes_globals_overflow` is set when the body writes more distinct
     * globals than the collector can record; coverage is then unanswerable and
     * the verdict degrades to UNVERIFIED rather than silently claiming the
     * frame holds. */
    const struct Symbol **writes_globals_declared;
    uint32_t              n_writes_globals_declared;
    const struct Symbol **writes_globals_seen;
    uint32_t              n_writes_globals_seen;
    bool                  writes_globals_overflow;
    /* RT4: the refinement this function's RESULT satisfies -- either declared
     * (`: #refine{ r : T | q }`) or inferred by template propagation.  A call
     * appearing inside a predicate or an argument asserts it about the value
     * that call produced, so a refined result can discharge the next
     * obligation instead of being an opaque term. */
    const struct Form  *refine_return_pred;
    const char         *refine_return_var;
    /* RT1: for a typeclass INSTANCE method that restated its own result
     * refinement, the CLASS's promise -- checked in addition to its own, and
     * only when `instance_pred |- class_pred` was not actually PROVED.
     *
     * This is what makes it sound to hand a dispatch site the class's result
     * refinement without knowing which instance runs. The variance check
     * reports only on a refutation, so an undecidable pair would otherwise
     * leave the class promise enforced by nothing while callers relied on it.
     * NULL whenever the instance inherited the class's predicate (its own
     * check already IS the class's) or the variance obligation discharged. */
    const struct Form  *refine_class_ret_pred;
    const char         *refine_class_ret_var;
    /* RT4 purity memo, for the congruence question "may two occurrences of
     * this call be modelled as the SAME value?".  0 = not yet computed,
     * 1 = pure, 2 = impure.  Computed by rt_binding_is_pure (elab_fns.c) with
     * a default-deny walk of the body; see the comment there for why the
     * declared effect row is not sufficient evidence on its own. */
    uint8_t             refine_purity;
    /* MB1 (constrained-hkt-forall-mode-b-plan): for a top-level `defn` binding,
     * the FnDef it defines -- lets make_dict_clone reach the original body/params
     * from the binding without scanning file-scope defs (user defns are not yet
     * registered there during elaboration).  NULL for non-defn bindings. */
    struct FnDef *source_fn_def;
    /* bare-fat-result-monomorphization-plan (Phase B):
     *
     * bare_fat_result_kind -- on a bare `^fat g` *parameter* binding, the
     *   TypeKind a direct call `(g x)` yields.  TY_UNKNOWN (the zero default)
     *   means "use the int64 carrier" (the canonical body, current behavior);
     *   a specialized clone stamps the incoming closure's result kind (e.g.
     *   TY_FLOAT) here, and elab_call's CY2 fat-dispatch reads it so `(g x)`
     *   types correctly in ANY position (not just the tail Phase A covers).
     *
     * defn_form / bare_fat_lazy -- on a top-level fn *binding* that has at
     *   least one bare-^fat param.  defn_form retains the `(defn ...)` Form so
     *   the body can be re-elaborated per closure result kind.  bare_fat_lazy
     *   is set when the canonical (int) body did NOT typecheck (it is float-
     *   only, like the plan's gate): no canonical FnDef was emitted, and the
     *   binding becomes callable only through a per-call-site specialization.
     *   bare_fat_specialized records that at least one clone was produced (so
     *   the end-of-pass sweep does not re-surface the deferred canonical
     *   error for a binding that a caller successfully specialized). */
    TypeKind            bare_fat_result_kind;
    const struct Form  *defn_form;
    bool                bare_fat_lazy;
    bool                bare_fat_specialized;
    /* SZ8 (sz8-projection-size-recovery-gap): the declared type-annotation
     * Form for this binding (a function parameter's `: T` annotation, or a
     * let binding's annotation / inferred initializer result type).  Retained
     * so cross-parameter size unification can recover a value's static size
     * index when it flows in as a plain variable or a struct field projection
     * (EX_VAR / EX_GET_FIELD args), not just a direct call.  NULL when no form
     * is recoverable. */
    const struct Form  *decl_type_form;
    /* pr-386 regression fix (docs/archive/history/pr-386-source-binding-alias-breaks-
     * closure-and-with-resource.md): true when this binding is the global
     * `__fn_N` helper minted for a captureless lifted lambda (elab_fns.c).
     * The source_binding alias rule in elab_forms.c must NOT chain a let
     * binding to such a helper: a lifted closure-returning lambda is callable
     * only through the closure-dispatch protocol on the let binding, not by a
     * direct call to the lifted global (whose C signature returns the int64
     * carrier, not a function pointer).  Only user-named globals are valid
     * source_binding targets. */
    bool                is_lifted_lambda;
    /* closure-drop-glue S1c (non-retaining fn-param inference): for a function
     * binding, bit i is set when parameter i is a fn-typed / ^fat parameter that
     * the body only CALLS -- never stores, returns, captures, or passes to a
     * retaining position (i.e. `!closure_binding_escapes(body, param_i)`).  A
     * capturing-closure argument to such a param does NOT escape the callee, so
     * its heap env may be freed at the call scope's exit, exactly like a
     * `^borrow` fn-param (FA_BORROW).  Inferred once when the defn is elaborated;
     * read at the call site (hoist) and by the emit-side escape analysis.  Params
     * beyond bit 31 are left unset (conservative -- no free).  0 for non-fns and
     * fns with no non-retaining fn-param. */
    uint32_t            nonretain_param_mask;
    /* catch-box-reader-confinement-whitelist: bit i set when parameter i is a
     * POINTER-CARRYING SCALAR (cstr / ptr<void>) that the body provably does
     * not retain -- every use of it is discarded or flows into another
     * non-retaining sink.  This is what lets a caught-Result box be deep-freed
     * at scope exit when its message was handed to a USER-DEFINED logger, not
     * only to the hardcoded print family: the property is inferred from the
     * callee's body rather than trusted from a name list.  Params beyond bit 31
     * are left unset (conservative -- no free). */
    uint32_t            nonretain_ptr_param_mask;
    /* closure-drop-glue S1c (fresh-closure-returning fn): true when this function
     * binding's body is a bare capturing EX_CLOSURE with only scalar (Copy)
     * captures and a scalar result -- so every call mallocs a FRESH, uniquely
     * owned env whose bare `free` is fully safe.  A call `(F ...)` to such an F,
     * consumed by a non-retaining fn-param, has its env freed at the call scope's
     * exit (the make-scaler shape).  False for non-fns and any fn that returns a
     * shared/owning-capture/param closure. */
    bool                returns_fresh_closure;
    /* Existential `open` dispatch: when this binding names the `v` of
     * `(open e [a v] ...)` and `e` is a constraint-carrying existential, this
     * points at the packed scrutinee's TY_EXISTS type (carrying the constraint
     * classes, in witness order).  A method call on `v` consults it to resolve
     * dispatch through the runtime witness vtable rather than failing as
     * ambiguous over the erased int64 carrier type.  NULL for ordinary
     * bindings.  See docs/archive/history/existential-open-witness-dispatch.md. */
    const struct Type  *exists_open_type;
    /* van-laarhoven-lens-composition (Gap B2): a synthetic binding standing for
     * the ENCLOSING constrained rank-2 fn's own `Functor f` dictionary.  Emitting
     * a reference to it yields the enclosing dict-clone's dict PARAMETER
     * (ctx->dict_dispatch_param_cname).  Because it is a real binding, an adapter
     * lambda that forwards the dict into a nested constrained rank-2 call captures
     * it through the ordinary free-variable machinery -- so the lifted lambda gets
     * the caller's actual dict, not a hardcoded singleton.  `ambient_repr` is the
     * representative instance used for the plain carrier-base fallback. */
    bool                is_ambient_dict;
    struct TypeClassInstance *ambient_repr;
};

/* GF1: Generator definition -- one per (gen ...) expression */
typedef struct GenDef {
    struct Expr  *body;              /* elaborated body expression */
    struct Binding **captures;       /* free variables captured from enclosing scope */
    uint32_t      n_captures;
    struct Binding **struct_bindings; /* all let bindings in gen body (yield-live) */
    uint32_t      n_struct_bindings;
    uint32_t      n_yield_points;    /* number of (yield ...) forms in body */
    TypeKind      element_kind;      /* TypeKind of values yielded */
    char          struct_name[64];   /* e.g. "__gen_myfn_0_t" */
    char          next_fn[64];       /* e.g. "__gen_myfn_0_next" */
    char          create_fn[64];     /* e.g. "__gen_myfn_0_create" */
} GenDef;

typedef enum ExprKind {
    EX_NIL_LIT = 1,
    EX_BOOL_LIT,
    EX_INT_LIT,
    EX_FLOAT_LIT,       /* Phase 1: Float literals */
    EX_CSTR_LIT,
    EX_VAR,
    EX_LET,
    EX_LETREC,             /* letrec -- mutually-recursive let bindings */
    EX_IF,
    EX_DO,
    EX_WHILE,
    EX_SET,
    EX_DEF,
    EX_BUILTIN,
    EX_FN,              /* Phase 2: anonymous function (no capture) */
    EX_CALL,            /* Phase 2: function call (f a b c) */
    EX_FN_DEF,          /* Phase 2: top-level function definition (defn) */
    EX_EXTERN_C,        /* Phase 2: extern C declaration */
    EX_INLINE_C,        /* Phase 2: inline C block */
    EX_CLOSURE,         /* Phase 3: closure with captured env */
    EX_RETURN,          /* Phase 3/4: early return with defer firing */
    EX_DEFER,           /* Phase 4: defer expression */
    /* Phase 5: ref<T> with move semantics */
    EX_REF,             /* (ref expr) - owning reference constructor */
    EX_DEREF,           /* (@ expr) - dereference ref<T> or ptr<T> */
    /* Phase 9: rc<T> + weak<T> reference counting */
    EX_RC_OF,          /* (rc/of x) - create new rc<T> */
    EX_RC_CLONE,       /* (rc/clone r) - increment strong count */
    EX_RC_DROP,        /* (rc/drop r) - decrement strong count */
    /* Note: (@ r) for rc<T> reuses EX_DEREF */
    EX_RC_PTR,         /* (rc->ptr r) - borrow ptr<T> from rc<T> */
    EX_RC_COUNT,       /* (rc/strong-count r) - get strong count */
    EX_RC_FROM_REF,    /* (rc/from-ref r) - move ref<T> into rc<T> */
    EX_REF_FROM_RC,    /* (ref/from-rc r) - extract unique ref<T> from rc<T> */
    EX_WEAK,           /* (weak r) - create weak<T> from rc<T> */
    EX_WEAK_UPGRADE,   /* (upgrade w) - upgrade weak<T> to option<rc<T>> */
    EX_WEAK_PRED,      /* (weak? w) - check if w is weak<T> */
    EX_REF_PRED,       /* (ref? x) - check if x is ref<T> */
    /* Phase 12: Borrow traits */
    EX_BORROW_IMMUT,   /* (& expr) - create immutable borrow */
    EX_BORROW_MUT,     /* (&mut expr) - create mutable borrow */
    EX_SET_DEREF,      /* (set! (@ r) v) - mutation through mutable borrow */
    /* Phase 15: Typeclasses */
    EX_TYPECLASS_DEF,   /* (defclass ...) - typeclass definition */
    EX_INSTANCE_DEF,   /* (definstance ...) - typeclass instance definition */
    /* Phase R2: Panic */
    EX_PANIC,          /* (panic msg) - print msg to stderr and abort */
    EX_PANIC_WITH,     /* (panic-with payload) - panic with typed payload */
    EX_CATCH_UNWIND,   /* (catch-unwind thunk) - catch any panic at boundary */
    EX_CATCH_PANIC_OF, /* (catch-panic-of Type thunk) - catch panic of specific type */
    EX_PANIC_PAYLOAD_TYPE,    /* (panic-payload-type p) - get type tag */
    EX_PANIC_PAYLOAD_VALUE,   /* (panic-payload-value p) - get value */
    EX_PANIC_PAYLOAD_FILE,    /* (panic-payload-file p) - get file */
    EX_PANIC_PAYLOAD_LINE,    /* (panic-payload-line p) - get line */
    EX_PANIC_PAYLOAD_DOWNS,   /* (panic-payload-downcast p Type) - downcast */
    /* Phase 18: Delimited continuations */
    EX_RESET,          /* (reset body) - establish continuation boundary */
    EX_SHIFT,          /* (shift k body) - capture continuation, pass to k */
    EX_SHIFT0,         /* (shift0 k body) - one-shot shift */
    EX_CALLCC,         /* (call/cc f) / (escape f) - undelimited capture vs the
                        * implicit root prompt; f receives a real cont<T> (CPS8/
                        * call-cc-completion). is_escape selects the abort flavor. */
    /* Phase B2: Cloneable continuations */
    EX_CLONEABLE_RESET, /* (cloneable-reset body) - continuation boundary with cloneable captures */
    EX_CLONEABLE_SHIFT, /* (cloneable-shift k body) - capture cloneable continuation */
    /* Phase 19: Algebraic effects */
    EX_DEFECT,         /* (defeffect Name [params...] : result) - define an effect */
    EX_PERFORM,        /* (perform (EffectName args...)) - perform an effect */
    EX_HANDLE,         /* (handle expr cases...) - handle effects */
    /* FH2-FH5: first-class effect handler values */
    EX_HANDLER_LIT,    /* (handler (E [params] k) body) - handler value literal */
    EX_WITH_HANDLER,   /* (with-handler hv body) - apply a handler value to a body */
    EX_COMPOSE_HANDLERS, /* (compose-handlers h1 h2) - concat two handler tables */
    EX_RESUME,         /* (resume k value) - resume continuation with value */
    EX_DISCONTINUE,    /* (discontinue k exception) - discontinue with exception */
    EX_CONT_PRED,      /* (cont? k) - check if continuation is unconsumed */
    /* Phase T21-F: async/await sugar */
    EX_ASYNC,          /* (async fn-expr) - run no-arg fn in thread; return Future (ptr<void>) */
    EX_AWAIT,          /* (await fut)     - block on Future; return int value */
    /* Phase SEL1: fair multi-channel select */
    EX_SELECT,         /* (select ((ch :recv v) body) ... (:default body)) */
    /* Phase 20: Software Transactional Memory */
    EX_STM,            /* (stm & body) - STM transaction block */
    EX_ATOMICALLY,     /* (atomically stm-block) - execute STM transaction atomically */
    EX_RETRY,          /* (retry) - retry transaction from within stm block */
    EX_CHECK,          /* (check cond) - abort transaction if cond is false */
    EX_OR_ELSE,        /* (or-else stm1 stm2) - try stm1, retry with stm2 if retry */
    EX_TVAR_NEW,       /* (TVar::new init) - create new TVar */
    EX_TVAR_READ,      /* (TVar::read tvar) - read TVar within stm block */
    EX_TVAR_WRITE,     /* (TVar::write tvar value) - write TVar within stm block */
    EX_TVAR_MODIFY,    /* (TVar::modify tvar fn) - modify TVar within stm block */
    EX_TVAR_SWAP,      /* (TVar::swap tvar new) - swap TVar value within stm block */
    EX_TVAR_CAS,       /* (TVar::cas tvar old new) - compare-and-swap within stm block */
    /* Phase N: numeric cast */
    EX_CAST,           /* (as TargetType expr) — explicit numeric cast */
    EX_REINTERPRET,    /* compiler-only bit reinterpret between same-size scalar types */
    /* Phase H §1: dictionary passing */
    EX_DICT,           /* implicit dictionary argument — address of a typeclass instance singleton */
    /* Phase 11: Struct operations */
    EX_MAKE_STRUCT,    /* (make-struct Name v1 v2 ...) - struct literal */
    EX_GET_FIELD,      /* (.field s) - struct field access */
    /* Phase DS3: struct field assignment */
    EX_SET_FIELD,      /* (set! (.field s) v) - struct field write */
    EX_PROGRAM,
    /* Phase M0: Module system */
    EX_DEFMODULE,      /* (defmodule name [docstring] (export ...) (import ...) body...) */
    /* Phase 21: Serializable continuations */
    EX_SERIAL_RESET,   /* (serial-reset body) - establish serializable continuation boundary */
    EX_SERIAL_SHIFT,   /* (serial-shift k body) - capture serializable continuation */
    /* Phase X3: Set literal */
    EX_SET_LIT,        /* #s(e1 e2 ...) - set literal */
    /* Phase HRT1: Rank-2 higher-ranked types */
    EX_POLY_WRAP,      /* wraps a fn/closure as tur_poly_fn_t for rank-2 param passing */
    EX_FN_TO_FAT,      /* A#1: auto-shim a bare fn into a fat closure for a ^fat param */
    EX_POLY_TO_FAT,    /* SC7: convert a tur_poly_fn_t (typeclass-method closure) into a fat-closure handle for a ^fat param */
    EX_ASCRIBE,        /* (:: expr type) — inline type ascription; erased at codegen */
    /* Phase HRT2: Existential types */
    EX_EXISTS_PACK,    /* (pack expr (exists [a] T)) — boxes value as existential */
    EX_EXISTS_OPEN,    /* (open packed [a v] body) — unboxes existential, binds v */
    EX_EXISTS_DISPATCH,/* method call on an open-bound `v`, resolved at runtime
                        * through the existential record's packed witness vtable */
    /* Phase G0: Plain ADTs */
    EX_DEFDATA,        /* (defdata Name [:copy] (Ctor1) (Ctor2 T1 T2) ...) */
    EX_MATCH,          /* (match scrutinee (Ctor1 x y) body1 _ default-body) */
    /* Phase G1: GADTs */
    EX_DEFGADT,        /* (defgadt Name [params] (Ctor : return-type) ...) */
    /* IT4: Tagged union injection — wraps a value into tur_tagged_t */
    EX_UNION_INJECT,   /* (union-inject tag_idx value) — tags a member value for TY_UNION/TY_ANY */
    /* IT4 gradual typing */
    EX_ANY_TYPE_OF,    /* (type-of x) — returns cstr type name of an any-typed value */
    EX_ANY_CAST,       /* (cast x T) — unsafe downcast from any; returns the inner value as T */
    EX_ANY_IS,         /* TY3: (is? x T) — runtime type test; returns bool */
    /* DV0-DV1: Dynamic vars (-Xdynamic-vars) */
    EX_DEFDYNAMIC,       /* (defdynamic *name* :type root-expr) -- declare a dynamic var */
    EX_DYNVAR_READ,      /* *name* -- read current value of a dynamic var */
    EX_DYNVAR_BINDING,   /* (binding [*v* expr ...] body) -- dynamic binding form */
    EX_DYNVAR_SET,       /* (set! *name* expr) on a dynvar -- mutate top binding frame */
    /* GF1: Generator forms */
    EX_GEN,              /* (gen [] body) -- generator expression; returns TY_GENERATOR */
    EX_YIELD,            /* (yield expr)  -- yield a value inside gen body; type TY_NIL */
    EX_GEN_NEXT,         /* (gen-next g)  -- advance generator; returns ptr<void> (option) */
    EX_GEN_DONE,         /* (gen-done? g) -- check if generator is exhausted; returns bool */
    /* AR8: Variadic rest-list construction at call sites */
    EX_CONS_LIST,        /* build a right-folded cons list from N items for & rest param */
    /* SYM0 (runtime-symbols-plan): runtime symbol literal (-Xsymbols).
     * `:foo` in expression position elaborates to this; codegen lowers it to a
     * reference to a static `struct __tur_sym` record (SYM1). */
    EX_SYM_LIT,          /* :foo -- interned symbol literal; type TY_SYM */
    /* CPS2: explicit continuation application in CPS-lowered code.
     * Represents `k(v)` -- applying a continuation to a result value. */
    EX_CPS_CONT_APP,     /* (cps-apply k v) -- continuation application; type is k's result type */
    /* M2b: (default-of T) — yields a zero-valued T. Type lives in Expr::type. */
    EX_DEFAULT_OF,
} ExprKind;

/* Phase 2: FnDef represents a function definition from defn or lifted fn. */
struct FnDef {
    Binding        *binding;     /* name binding */
    Binding       **params;      /* param bindings */
    uint32_t       n_params;     /* bounded only by uint32_t -- no fixed arity cap */
    Type          *param_types;  /* param types (for codegen) */
    /* LS2: full declared return Type, including borrow lifetimes (&'a T).  The
     * binding's TY_FN only carries result_kind (a bare TypeKind), which loses
     * the lifetime IDs the lifetime pass needs; this preserves them. */
    Type           return_type;
    Expr          *body;
    bool           is_variadic;  /* not yet supported in phase 2 */
    /* Phase 3: For closure thunks, store the closure info */
    struct Closure *closure;    /* NULL for non-closure functions */
    /* Phase 4: Future-proofing for v3 effects (effects-plan.md §6.10) - whether this
     * function may capture continuations. Always false in v0/v1. */
    bool           may_capture;
    /* CPS1 (cps-transform-plan): whole-program "may-capture" coloring result.
     * True iff this function can dynamically reach a control operator (directly,
     * transitively through a resolved call, or conservatively via an indirect
     * call). Additive analysis metadata: written by cps_color_program, consumed
     * by the future selective-CPS lowering (CPS3). Distinct from `may_capture`,
     * which the existing Phase-18 delimited-CPS pass owns. */
    bool           cps_colored;
    /* CPS2: true when this function has been selected for the CPS emission path
     * (set by cps_propagate_coloring; mirrors may_capture but is a separate field
     * so the CPS emitter path can be toggled independently). */
    bool           is_cps;
    /* Phase 13: Lifetime annotations */
    LifetimeContext lifetime_ctx;  /* Lifetime parameters and constraints for this function */
    /* Phase 15: Typeclass constraints */
    ConstraintSet  constraints;    /* Typeclass constraints for this function */
    /* Phase P19-3: Inferred effect row for this function.
     * NULL until the effect-row inference pass (P19-2) runs.
     * declared_effect_row lives in Type.as.fn.effect_row (the annotated row);
     * this field carries the pass-computed row so both can coexist. */
    EffectRow     *inferred_effect_row;
    /* CT0: Contract pre/post-conditions — NULL if not specified */
    const struct Form *pre_cond;   /* :pre predicate form, or NULL */
    const struct Form *post_cond;  /* :post predicate form, or NULL */
    /* M4a (docs/archive/m4-typeclass-per-method-abi-plan.md): when this
     * FnDef is a typeclass-instance method (i.e. the implementation behind
     * `__inst_<Class>_<method>__…`), `owner_instance` points back at the
     * `TypeClassInstance` that owns it.  NULL for ordinary defns and for
     * standalone lifted lambdas.
     *
     * Populated by elab_definstance immediately after the FnDef is constructed
     * for each method.  Used by emit_module.c to route the spec through the
     * M4b/M4c per-instantiation emit path on non-HKT classes (HKT-class
     * instance methods keep the uniform carrier ABI per Plan M6/M7). */
    struct TypeClassInstance *owner_instance;
    /* MB1 / forall-dict-pass-multi-constraint-hkt-plan (Task 1.1): when
     * `n_dict_clone > 0` this FnDef is a *dict-clone* -- a copy of a polymorphic
     * constrained function (used as a rank-2 value) that shares the original's
     * body and trailing params but prepends one int64 dict param PER constraint
     * (each holding a dictionary pointer), in constraint order.  While its body
     * is emitted, a class-method call on a constrained type variable (one
     * carrying a `dict_arg` whose instance's class matches one of
     * `dict_clone_classes[0..n)`) dispatches through the matching dict param
     * `dict_clone_params[k]` at runtime instead of the baked representative
     * instance.  `n_dict_clone == 0` means "not a dict-clone".  The vectors are
     * parallel and length `n_dict_clone`. */
    Binding             *dict_clone_params[MAX_FN_CONSTRAINTS];
    struct TypeClass    *dict_clone_classes[MAX_FN_CONSTRAINTS];
    uint8_t              n_dict_clone;
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 1): memoized on the
     * ORIGINAL constrained FnDef the first time it is dict-cloned.  Every clone
     * of the same original reuses these per-constraint dict param Bindings so
     * their identity + cname are STABLE across clones -- a nested mapper lambda
     * (shared across all clones) can then capture one consistently and read the
     * dict from its closure env.  `n_memo_dict == 0` means "not yet cloned". */
    Binding             *memo_dict_params[MAX_FN_CONSTRAINTS];
    uint8_t              n_memo_dict;
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 2) +
     * forall-dict-pass-nested-mapper-general-plan (Phase 1): set on a nested
     * MAPPER lambda (lifted out of a dict-clone body) that dispatches one or
     * more typeclass methods on the dict-clone's constrained type variable(s).
     * The mapper is converted to a closure that CAPTURES one runtime dict per
     * dispatched class (`dict_env_bindings[0..n_dict_env)`); while its body is
     * emitted, a class-method call whose class matches ANY of
     * `dict_env_classes[0..n_dict_env)` dispatches through that class's captured
     * dict (an `env->dict` load) instead of the baked representative instance.
     * The two vectors are parallel; `n_dict_env == 0` means "not a
     * dict-capturing mapper". */
    struct TypeClass    *dict_env_classes[MAX_FN_CONSTRAINTS];
    Binding             *dict_env_bindings[MAX_FN_CONSTRAINTS];
    uint8_t              n_dict_env;
    /* forall-dict-pass-nested-mapper-general-plan (Phase 3): true once this
     * mapper has been through the dict-capturing conversion.  Distinct from
     * `n_dict_env > 0` because a FORWARD-ONLY intermediate mapper (it dispatches
     * nothing itself but captures a dict to forward into a nested mapper it
     * constructs) is converted yet has `n_dict_env == 0`.  The conversion mutates
     * the SHARED body once per original and must stay idempotent across every
     * clone's lowering walk, so this flag -- not `n_dict_env` -- gates re-entry. */
    bool                 dict_env_converted;
    /* The mapper's user-facing (pre-env-prepend) fn type, kept so a SECOND
     * poly-wrap referencing the same already-converted mapper can be rewritten
     * to the same EX_CLOSURE form (its value type is this, boxed). */
    Type                 dict_env_mapper_ty;
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 2): set on the now-dead
     * poly-wrapper of a mapper that was converted to a dict-capturing closure --
     * the closure form (EX_CLOSURE) replaces the wrapper, so emitting it would
     * reference the mapper with the pre-conversion arity.  emit_fn_def skips it. */
    bool                 skip_emission;
    /* WF1/WF2 (van-laarhoven-wide-functor-carrier-plan): set on the
     * functor-wrapping closure `g : (-> A (f A))` of a van Laarhoven lens when
     * `f` is pinned to a WIDE by-value aggregate functor (a `:copy` struct /
     * flat-product ADT wider than the one-int64 carrier word).  Such a closure
     * would otherwise return its `(f A)` aggregate BY VALUE, but it crosses the
     * mode-B poly carrier and is fat-dispatched (slot 0) by the generic
     * dict-clone as an `int64_t`-returning thunk.  When set, emit gives the
     * closure the int64 carrier return type and heap-boxes the aggregate result
     * (the inverse box the lens's poly-carrier boundary already unboxes).  Set
     * unconditionally for wide functors since VBM4 (never for carrier-compatible
     * ones); the VBM3 resolve pass CLEARS it on closures whose consumer resolves
     * uniquely to a mono lens (--enable=vl-wide-mono), so those return `(f A)` by
     * value straight into the by-value mono body. */
    bool                 box_aggregate_result;
    /* lens-composition-codegen-blockers (Blocker 2c): set when this lifted
     * closure is stored as a VALUE into a typed `(fn ...)` struct/ADT field (a
     * lens `get`/`put`), so it is invoked through that field's TYPED thunk (its
     * params spelled by their real C type / by-value fatshims), NOT the uniform
     * int64 carrier.  A wide by-value ADT param of such a closure must cross BY
     * VALUE, so the B4 `b4box` boxing (emit_fns.c needs_box_load) is suppressed
     * for it -- otherwise the boxed definition disagrees with the by-value typed
     * thunk + call site and the arg is corrupted (SIGSEGV).  Closures dispatched
     * through `tur_poly_fn_t` (fmap etc.) or called directly keep b4box. */
    bool                 byval_fn_field_closure;
};

/* Phase 2: ExternC represents an (extern-c ...) declaration. */
struct ExternC {
    const Symbol  *c_name;       /* C identifier */
    Binding       *binding;     /* Turmeric binding (optional) */
    Type           return_type;
    Type          *param_types;
    uint32_t       n_params;     /* bounded only by uint32_t -- no fixed arity cap */
    bool           is_variadic;
    /* CT4: Contract pre/post-conditions on extern-c calls — NULL if not specified */
    const struct Form *pre_cond;   /* :pre predicate form, or NULL */
    const struct Form *post_cond;  /* :post predicate form, or NULL */
};

/* jit-ffi-c2mir-plan F3: the explicit C signature of a `(call-ptr ...)`
 * form -- an indirect call through a raw address (dlsym result) whose
 * signature is stated at the call site rather than carried by a binding.
 * Hangs off EX_CALL's `ptr_sig` so every existing recursive walker (which
 * already visits fn_expr + args) traverses it correctly with no new expr
 * kind.  F4: a slot may also name a by-value record (TY_ADT).
 *
 * F5 reuses the same node for the REVERSE direction.  With `is_callback`
 * set the node is `(callback-ptr f [T1 T2 -> R])`: `fn_expr` is the Turmeric
 * closure rather than a target address, `n_args` is 0, and the node's value
 * is a C function pointer that calls back into that closure.  Sharing the
 * node keeps F3's property that no walker had to learn a new kind -- the
 * closure sits exactly where the address sat, and both are just `fn_expr`. */
typedef struct CallPtrSig {
    Type      return_type;
    Type     *param_types;   /* arena-owned; n_params entries */
    uint32_t  n_params;
    bool      is_callback;   /* F5: (callback-ptr f [sig]), not (call-ptr ...) */
} CallPtrSig;

/* Phase 2: InlineC represents an inline C block. ```c ... ``` */
struct InlineC {
    StrSlice       code;         /* the raw C code (may contain __TUR_CAP_N__ / __TUR_VAL_N__) */
    Type           return_type; /* annotated : T */
    Binding      **captures;     /* captured bindings; __TUR_CAP_N__ substitutes captures[N]'s C name */
    uint8_t        n_captures;
    struct Expr  **val_exprs;    /* SS2: sub-expressions; __TUR_VAL_N__ evaluates val_exprs[N] */
    uint8_t        n_val_exprs;
};

/* Phase 3: Closure represents a fn with captured environment. */
struct Closure {
    FnDef         *fn;           /* the function definition (modified with env param) */
    Binding      **captures;     /* captured bindings from enclosing scope */
    uint8_t        n_captures;
    const Symbol *env_name;     /* generated name for the env struct type */
    /* closure-drop-glue (Model R) #1b: per-capture Drop/Clone instance resolved at
     * elaboration (parallel to `captures`, entry NULL = the capture's type has no
     * such instance).  When a capture's type implements Drop, the closure drop-glue
     * releases it through capture_drop_insts[i]'s `drop` method; if it also
     * implements Clone (a refcounted owner like String), the env-fill retains it
     * through capture_clone_insts[i]'s `clone` method (retain/release balances,
     * aliasing-safe -- the rc-capture pattern generalized through typeclasses).
     * Both arrays are NULL when no capture implements Drop (the common case). */
    struct TypeClassInstance **capture_drop_insts;
    struct TypeClassInstance **capture_clone_insts;
    /* cps-native-handle-in-reset (Reduction B): set when this closure is the
     * RECEIVER of a cross-function `shift` desugared onto __Shift -- i.e. the
     * single argument of `(perform (__Shift recv))`.  Such a receiver is invoked
     * only as `(recv k)` inside the __Shift handler case (never indirect-called
     * elsewhere), and the handler bridge-wraps its DK continuation, so the CPS
     * backend may delegate its build via CT_LETRAW even when it CAPTURES scalars
     * -- unlike a general capturing closure, which is not a valid indirect callee
     * (see indirect_callee_ok).  Scoped strictly to __Shift receivers. */
    bool           is_shift_receiver;
    /* httpd-mw-recover-unblocked-but-unwritten (B): set on the THUNK closure of
     * a `catch-unwind` / `catch-panic-of`.  Such a thunk is created and dropped
     * at the catch site in the same frame, so any `^fat` handle it captured is
     * BORROWED from that frame -- which still holds it.  Releasing it in the
     * thunk env's drop glue frees a handle the enclosing closure still owns:
     * the first call through a captured handle worked and the second read freed
     * memory (`heap-use-after-free ... freed by drop_glue___env_NNNN`).  Only
     * the fat-handle release is suppressed; an rc capture still balances the
     * env-fill retain. */
    bool           fat_captures_borrowed;
    /* cps-dk-multishot-user-effects (Phase A): set when this closure is the fn
     * PAYLOAD of a resumable-payload user effect (`(perform (E g))` where E is
     * resumed through g).  The user-effect analogue of is_shift_receiver: it is
     * boxed into the one-word effect slot and applied once in the handler case
     * (`(f k)`), never indirect-called elsewhere, so the CPS backend delegates its
     * build (CT_LETRAW) even though it captures.  Its boxed env is reaped at the
     * handler case (the generalized __Shift P3.d reap). */
    bool           is_effect_payload;
};

typedef struct LetBinding {
    Binding *binding;
    Expr    *init;
} LetBinding;

/* Phase 19: Algebraic effects */

/* Effect definition: (defeffect Name [param1 : T1, ...] : R) */
typedef struct EffectDef {
    const Symbol *name;           /* Effect name */
    const Symbol **param_names;  /* Parameter names */
    TypeKind *param_types;       /* Parameter types (TypeKind for now) */
    uint8_t n_params;
    TypeKind result_type;        /* Result type of the effect operation */
    /* Phase P19-6: Module visibility */
    bool          is_private;           /* declared with ^private */
    const Symbol *defining_module_name; /* module that declared this effect, or NULL */
    /* ET4: effect hierarchy -- NULL if no ^extends */
    const Symbol *parent_name;          /* name of parent effect (for ^extends), or NULL */
    /* stdlib-effect-rows: declared with ^capability -- a coarse capability tag. */
    bool          is_capability;
} EffectDef;

/* DV0: Dynamic var metadata entry (-Xdynamic-vars).
 * One entry per (defdynamic ...) declaration. */
typedef struct DynVarEntry {
    const Symbol *name;       /* interned symbol, e.g. "*log-level*" */
    Type          value_type; /* declared element type */
    int           index;      /* sequential ID; used as pthread_key_t index in DV2 */
    bool          is_private; /* ^private annotation */
} DynVarEntry;

/* DV1: One override pair inside a (binding [...] body) form. */
typedef struct DynBinding {
    DynVarEntry *entry;         /* which dynamic var is being overridden */
    struct Expr *override_expr; /* the new value for this scope */
} DynBinding;

/* Perform expression: (perform (EffectName arg1 arg2 ...)) */
typedef struct PerformExpr {
    const Symbol *effect_name;   /* Name of the effect to perform */
    Expr **args;                /* Arguments to the effect */
    uint8_t n_args;
    /* cps-dk-multishot-user-effects (Phase A): this effect is resumed THROUGH a fn
     * payload (its constructor has a `(fn [effect-cont] R)` param).  Set by
     * elab_perform from the effect's resumable_payload_param.  Lets the CPS/DK
     * perform-arg gate admit the boxed-fn payload atom (scoped -- a plain non-
     * resumable fn payload effect stays on the fiber path), paired with the
     * handler-case cloneable-cont wrap (keyed on the handler's CK_MULTISHOT). */
    bool resumable_payload;
} PerformExpr;

/* Handle case: (EffectName [param1 param2 ...] k) body ... */
typedef struct HandleCase {
    const Symbol *effect_name;   /* Name of the effect being handled */
    const Symbol **param_names;  /* Parameter names for the effect */
    struct Binding **param_bindings; /* Resolved bindings for params (set by elab) */
    uint8_t n_params;           /* Number of parameters */
    const Symbol *k_name;        /* Name of the continuation parameter */
    struct Binding *k_binding;   /* Resolved binding for k (set by elab) */
    /* LC0: ownership discipline for k.
     * CK_UNIQUE (default): at most one resume/discontinue (affine).
     * CK_LINEAR (^linear k): exactly one resume/discontinue required.
     * CK_MULTISHOT (^multishot k): MS1: safe multi-shot via snapshot semantics. */
    CopyKind cont_kind;
    /* cps-dk-multishot-user-effects (Phase A): this case handles a resumable-payload
     * effect (constructor has a `(fn [effect-cont] R)` param).  Set at elab; drives
     * the CPS/DK cloneable-cont wrap + boxed-payload reap.  Distinct from a bare
     * `^multishot` cont_kind (which a non-payload effect can also carry). */
    bool resumable_payload;
    Expr *body;                 /* Handler body */
} HandleCase;

/* Handle expression: (handle expr case1 case2 ...) */
typedef struct HandleExpr {
    Expr *body;                 /* The expression being handled */
    HandleCase *cases;         /* Array of handle cases */
    uint8_t n_cases;
    /* F2 (shallow handlers): true for `handle-shallow`, false for `handle` /
     * `try-with`.  A shallow handler is NOT re-installed on resume (the
     * effect-side analogue of `shift0`); the flag rides through to the CPS IR
     * (CT_HANDLE) and the interpreter, where it selects dk_handler_shallow vs
     * dk_handler / the no-reinstall re-entry.  Deep vs shallow is
     * type-transparent, so only this bit differs. */
    bool shallow;
    /* defopaque-struct-payload-fails-through-unsafe-helper: set by elab_unsafe
     * for the desugaring of `(unsafe ...)`.  The built-in Unsafe effect is a
     * pure compile-time marker that is never performed, so this handle's
     * fiber-lift never suspends and its body may be emitted directly in place
     * (preserving the body's real C type).  A dedicated flag avoids reading
     * post-lowering case data, which is not reliably populated at emit time. */
    bool is_unsafe_marker;
} HandleExpr;

/* Resume expression: (resume k value) */
typedef struct ResumeExpr {
    Expr *k;                   /* The continuation to resume */
    Expr *value;               /* The value to resume with */
} ResumeExpr;

/* Discontinue expression: (discontinue k exception) */
typedef struct DiscontinueExpr {
    Expr *k;                   /* The continuation to discontinue */
    Expr *exception;           /* The exception to raise */
} DiscontinueExpr;

/* Phase M0: Module system */

typedef struct {
    const Symbol *module_name;   /* module to import, e.g. "geom/vector" */
    const Symbol *alias;         /* :as alias (or NULL) */
    const Symbol **refer_syms;        /* :refer list (or NULL = none) */
    uint32_t n_refer;
    const Symbol **refer_effect_syms; /* PR5-3-D: (effect Name) entries from :refer list */
    uint32_t       n_refer_effects;
    /* Stage 3 (macro-system-direction-plan): `(import m :for-macros)` --
     * macro-time only.  The module is evaluated into the compile's
     * macro-time env so defmacro* bodies can call its functions at
     * expansion time; NO runtime import happens.  Mutually exclusive with
     * :as / :refer (a module needed in both phases is imported twice). */
    bool for_macros;
    Span span;
} ImportSpec;

typedef struct DefModule {
    const Symbol *name;          /* module name, e.g. "geom/vector" */
    const char *docstring;       /* optional docstring (or NULL) */
    const Symbol **exports;           /* exported symbols */
    uint32_t n_exports;
    const Symbol **exports_mut;       /* G3: names in (export (mut g)) -- writable outside */
    uint32_t       n_exports_mut;
    const Symbol **exported_effects;  /* PR5-3-B: effect names in (export (effect Name)) */
    uint32_t       n_exported_effects;
    /* (export-from <mod> name ...) -- names this module re-exports from
     * another. Parallel arrays: reexport_srcs[i] is the module that DEFINES
     * reexports[i]. The consumer sees the defining module's Binding directly
     * (same mangled symbol), so no forwarding wrapper is emitted. */
    const Symbol **reexports;
    const Symbol **reexport_srcs;
    uint32_t       n_reexports;
    ImportSpec *imports;         /* import specs */
    uint32_t n_imports;
    Expr **body;                 /* elaborated body expressions */
    uint32_t n_body;
} DefModule;

/* Phase G0: A single pattern in a match arm */
typedef struct MatchPattern {
    bool is_wildcard;           /* _ wildcard */
    bool is_var;                /* bare variable capture (not a constructor) */
    bool is_literal;            /* literal value comparison (int/bool/float/cstr) */
    CtorDef *ctor;              /* NULL for wildcard/var/literal */
    Binding **bindings;         /* arena-allocated bindings for ctor fields */
    uint32_t n_bindings;
    const Symbol *var_sym;      /* for is_var: the variable being bound */
    Binding *var_binding;       /* resolved binding for is_var */
    int union_member_idx;       /* IT4: index into union members array for type-narrowing arms;
                                 * -1 for wildcard/var arms and non-union (ADT) arms */
    /* for is_literal: the literal value to compare against */
    int8_t      lit_kind;       /* F_BOOL/F_INT/F_FLOAT/F_STR/F_NIL from forms.h */
    bool        lit_bool;
    int64_t     lit_int;
    double      lit_float;
    const char *lit_cstr;
} MatchPattern;

/* Phase G0: One arm of a match expression */
typedef struct MatchArm {
    MatchPattern pattern;
    struct Expr *body;
    struct Expr *guard;  /* Phase G4: optional when-guard; NULL if no guard */
} MatchArm;

/* Phase SEL1: one clause of a (select ...) expression */
typedef struct SelectClauseEntry {
    struct Expr     *chan;         /* channel expression (ptr<void>) */
    int              op;          /* 0=recv, 1=send */
    struct Expr     *send_val;    /* for send: value expression; NULL for recv */
    Binding         *recv_binding; /* for recv: binding allocated by elab; NULL for send */
    struct Expr     *body;        /* clause body expression */
} SelectClauseEntry;

/* GS5/CS3: named-tyvar -> concrete type binding produced during call elaboration.
 * Attached to EX_CALL so emit_module.c can drive ABI specialization without
 * re-deriving the substitution. Owned by the elab arena. */
typedef struct AbiTypeBinding {
    const char *name;
    Type        type;
} AbiTypeBinding;

#define ABI_TYPE_BINDINGS_MAX 16

struct Expr {
    ExprKind kind;
    Type     type;
    Span     span;
    union {
        bool         b;
        int64_t      i;
        double       f;        /* Phase 1: Float literal value */
        StrSlice     s;

        struct { Binding *binding; }                                       var;
        struct { LetBinding *bindings; uint32_t n; Expr *body; }           let_;
        struct { Expr *cond; Expr *then_; Expr *else_or_null; }            if_;
        struct { Expr **items; uint32_t n; }                               do_;
        struct { Expr *cond; Expr *body; }                                 while_;
        /* set-bang-rc-release: `release_old` is stamped by elab_set_rc_release
         * when `target` is an rc-managed binding that owns a continuous +1 from
         * its init to its scope-exit auto-drop.  Overwriting such a binding must
         * release the value being overwritten, or every assignment leaks one rc
         * block.  Deliberately NOT set for a binding whose ownership is
         * hand-managed (moved, or explicitly dropped/consumed somewhere in the
         * body) -- there the auto-drop is suppressed too, and releasing here
         * would double-free.  See emit_set_stmt for the ordering. */
        struct { Binding *target; Expr *value; bool release_old; }          set_;
        struct { Binding *binding; Expr *init; }                           def_;
        struct { const BuiltinSpec *spec; Expr **args; uint32_t n; }       builtin;

        /* Phase 2 */
        struct { FnDef *fn; }                                               fn_def_;
        /* Phase 16 v2: fn_expr is non-NULL for indirect capability field calls
         * (EX_GET_FIELD callee). fn_binding is NULL in that case. */
        /* Phase H §1: dict_arg is non-NULL for typeclass method calls; holds the
         * EX_DICT node for the selected instance's dictionary singleton.
         * Full runtime dict passing is deferred; this field annotates the IR
         * so the information is available for future passes. */
        struct { Binding *fn_binding; Expr **args; uint32_t n_args;
                 struct Expr *fn_expr;
                 struct Expr *dict_arg;
                 bool is_poly_call;   /* Phase HRT1: call through rank-2 poly fn param */
                 /* Bitmasks over argument index.  64-bit: ordinary parameters
                  * are unbounded (these masks stay 0 for non-poly calls), but a
                  * single call can flag at most 64 rank-2-poly / aggregate-carrier
                  * arguments.  Always index via ARG_IDX_BIT(i) so i >= 64 yields 0
                  * rather than an undefined shift. */
                 uint64_t poly_arg_mask; /* Phase HRT3: bitmask of args that are nested poly fns.
                                          * In poly_call: bit i → pass arg by pointer (stack-alloc).
                                          * In direct call: bit i → deref int64_t arg as tur_poly_fn_t*. */
                 uint64_t poly_agg_arg_mask; /* Slice 3 (constrained-hkt-forall): in a poly-wrapper's
                                              * direct inner call, bit i → the int64 arg is a heap-box
                                              * pointer to a by-value aggregate; deref it to the param type. */
                 AbiTypeBinding *abi_bindings; /* GS5/CS3: named-tyvar substitution captured at the call site;
                                                * NULL when the call has no named-tyvar bindings. Arena-owned. */
                 uint8_t  n_abi_bindings;
                 bool is_tail_self_call; /* CF1: direct self-tail-call lowered to a goto backedge
                                          * by the emitter (set by emit_fns.c tco_mark). */
                 /* SZ8: when this call is a sized-GADT constructor, `ctor` is the
                  * resolved CtorDef and `size_index` is the inferred type-level
                  * size index of the constructed value (NULL otherwise). Both
                  * are elaboration-only; size indices are erased in codegen. */
                 struct CtorDef *ctor;
                 struct SizeTerm *size_index;
                 /* jit-ffi-c2mir-plan F3: non-NULL marks this call as
                  * `(call-ptr ...)` -- an indirect call through a raw
                  * address with the explicit C signature here.  fn_expr
                  * holds the pointer expression; fn_binding is NULL.  AOT
                  * codegen emits the direct cast-and-call; turi routes it
                  * through the JIT FFI thunk provider. */
                 struct CallPtrSig *ptr_sig; } call_;
        struct { FnDef *fn; }                                               fn_;
        struct { ExternC *ext; }                                            extern_c_;
        struct { InlineC *inline_c; }                                       inline_c_;
        /* Phase 3 */
        struct { struct Closure *closure; }                                 closure_;
        /* Phase 4 */
        struct { 
            Expr *body;              /* the defer body expression */
            /* v1 lowering: closure-style capture for defer bodies that reference
             * local variables. These are lifted into thunk functions with env structs.
             * Per effects-plan.md §6.10.1, this allows the S1/S2/S3 strategy choice
             * to be a runtime policy decision. */
            Binding **captures;       /* captured bindings from enclosing scope */
            uint8_t n_captures;
        } defer_;
        /* Phase 3/4: (return) or (return expr) - early return with defer firing */
        struct { Expr *value; } return_;
        /* Phase 5 */
        struct { Expr *expr; }        ref_;    /* (ref expr) - inner expression */
        struct { Expr *expr; }        deref_;  /* (@ expr) - expression to dereference */

        /* Phase 9: rc<T> + weak<T> operations */
        struct { Expr *expr; }        rc_of_;      /* (rc/of x) - value to wrap */
        struct { Expr *expr; bool elide; } rc_clone_;  /* (rc/clone r) - rc to clone; elide=true skips rc_strong_increment */
        struct { Expr *expr; bool elide; } rc_drop_;   /* (rc/drop r) - rc to drop; elide=true skips rc_strong_decrement */
        /* Note: (@ r) for rc<T> reuses deref_ field */
        struct { Expr *expr; }        rc_ptr_;    /* (rc->ptr r) - rc to borrow ptr from */
        struct { Expr *expr; }        rc_count_;  /* (rc/strong-count r) - rc to count */
        struct { Expr *expr; }        rc_from_ref_; /* (rc/from-ref r) - ref to convert */
        struct { Expr *expr; }        ref_from_rc_; /* (ref/from-rc r) - rc to convert */
        struct { Expr *expr; }        weak_;      /* (weak r) - rc to create weak from */
        struct { Expr *expr; }        weak_upgrade_; /* (upgrade w) - weak to upgrade */
        struct { Expr *expr; }        weak_pred_;   /* (weak? w) - expr to check */
        struct { Expr *expr; }        ref_pred_;    /* (ref? x) - expr to check */
        /* Phase 12: Borrow traits */
        struct { Expr *expr; }        borrow_immut_; /* (& expr) - expression to borrow immutably */
        struct { Expr *expr; }        borrow_mut_;   /* (&mut expr) - expression to borrow mutably */
        struct { Expr *ref; Expr *value; } set_deref_; /* (set! (@ r) v) - mutation through &mut T */
        /* Phase 15: Typeclasses */
        struct { TypeClass *typeclass; }                                  typeclass_def_;
        struct { TypeClassInstance *instance; }                          instance_def_;
        /* Phase R2: Panic */
        struct { Expr *payload; }        panic_;    /* (panic msg) - message to print before abort */
        struct { Expr *payload; }        panic_with_;    /* (panic-with payload) - typed payload */
        struct { Expr *thunk; }        catch_unwind_;    /* (catch-unwind thunk) - thunk to call */
        struct { TypeKind type_kind; Expr *thunk; } catch_panic_of_; /* (catch-panic-of Type thunk) */
        struct { Expr *payload; }        panic_payload_type_;   /* (panic-payload-type p) */
        struct { Expr *payload; }        panic_payload_value_;  /* (panic-payload-value p) */
        struct { Expr *payload; }        panic_payload_file_;   /* (panic-payload-file p) */
        struct { Expr *payload; }        panic_payload_line_;   /* (panic-payload-line p) */
        struct { Expr *payload; TypeKind target_type; } panic_payload_downs_; /* (panic-payload-downcast p Type) */
        /* Phase 18: Delimited continuations */
        struct { Expr *body; }         reset_;      /* (reset body) - body to run with fresh continuation */
        struct { 
            Expr *k_fn;             /* (shift k body) - k is a function (fn [v] ...) that receives the continuation */
            Expr *body;             /* body to run with captured continuation */
        } shift_;
        struct {
            Expr *k_fn;             /* (shift0 k body) - k is a function that cannot resume */
            Expr *body;             /* body to run */
        } shift0_;
        struct {
            Expr *fn;               /* (call/cc f) / (escape f) - f : cont<T> -> T */
            bool  is_escape;        /* true for (escape f): abort flavor (no re-install) */
        } callcc_;

        /* Phase B2: Cloneable continuations */
        struct { Expr *body; }         cloneable_reset_; /* (cloneable-reset body) */
        struct {
            Expr *k_fn;             /* (cloneable-shift k body) - k receives cloneable continuation */
            Expr *body;             /* body to run with captured cloneable continuation */
            /* CPS-CL1: live locals at this shift site (arena-allocated, set by cps_transform) */
            Binding   **live_captures;
            uint32_t    n_live_captures;
            /* CPS-CL3: the post-shift subtree (code that runs after this shift inside
             * the enclosing reset).  Set by cps_transform; NULL until then. */
            Expr       *cont_body;
            /* CPS-CL11: per-capture deep-clone / drop function names recorded by
             * cps_emit_capture_environment().  Each entry is parallel to
             * live_captures[i]; an entry may be NULL when no Clone method is
             * available (in which case emit.c falls back to a bitwise copy).
             * Strings live in the arena and are safe to inline into emitted C. */
            const char **capture_clone_fns;
            const char **capture_drop_fns;
        } cloneable_shift_;
        /* Phase 19: Algebraic effects */
        struct { EffectDef *def; }                   effect_def_;   /* (defeffect ...) */
        struct { PerformExpr *perform; }             perform_;     /* (perform ...) */
        struct { HandleExpr *handle; }               handle_;      /* (handle ...) */
        /* FH2-FH5: first-class handler values */
        struct { HandleExpr *handle; }               handler_lit_; /* (handler ...) -- cases, body==NULL */
        struct { Expr *handler; Expr *body; }        with_handler_;/* (with-handler hv body) */
        struct { Expr *h1; Expr *h2; }               compose_handlers_; /* (compose-handlers h1 h2) */
        struct { ResumeExpr *resume; }               resume_;      /* (resume k v) */
        struct { DiscontinueExpr *discontinue; }     discontinue_; /* (discontinue k e) */
        struct { Expr *expr; }                       cont_pred_;   /* (cont? k) */
        /* Phase T21-F: async/await */
        struct { Expr *fn_expr; }                    async_;       /* (async fn-expr) */
        struct { Expr *fut_expr; }                   await_;       /* (await fut) */
        /* Phase SEL1: fair multi-channel select */
        struct {
            SelectClauseEntry *clauses;   /* arena-allocated array */
            uint32_t           n_clauses;
            int                has_default;   /* 1 if :default arm present */
            struct Expr       *default_body;  /* :default body; NULL if none */
        } select_;                                                  /* (select ...) */
        /* Phase 20: Software Transactional Memory */
        struct { Expr **body; uint32_t n_body; }      stm_;         /* (stm expr1 expr2 ...) */
        struct { Expr *stm_expr; }                   atomically_;  /* (atomically stm-expr) */
        struct { Span dummy; }                       retry_;       /* (retry) - no fields, dummy for GNU */
        struct { Expr *cond; }                       check_;       /* (check cond) */
        struct { Expr *stm1; Expr *stm2; }           or_else_;     /* (or-else stm1 stm2) */
        struct { Expr *init; }                       tvar_new_;    /* (TVar::new init) */
        struct { Expr *tvar; }                       tvar_read_;   /* (TVar::read tvar) */
        struct { Expr *tvar; Expr *value; }           tvar_write_;  /* (TVar::write tvar value) */
        struct { Expr *tvar; Expr *fn; }              tvar_modify_; /* (TVar::modify tvar fn) */
        struct { Expr *tvar; Expr *new_val; }         tvar_swap_;   /* (TVar::swap tvar new) */
        struct { Expr *tvar; Expr *old_val; Expr *new_val; } tvar_cas_; /* (TVar::cas tvar old new) */
        /* Phase N: numeric cast */
        struct { Expr *expr; TypeKind target_kind; } cast_;        /* (as T e) */
        /* `retain` is meaningful only when one side is an owning kind (rc/weak/
         * ref): true means crossing the carrier hands out a NEW strong
         * reference (a collection taking ownership of a pushed element, or a
         * read handing the caller its own count), false means the existing
         * reference merely moves (a borrow, or a pop transferring the slot's
         * count out).  See docs/archive/history/collections-cannot-hold-rc-values.md. */
        struct { Expr *expr; TypeKind source_kind; TypeKind target_kind; bool retain; } reinterpret_;
        /* Phase H §1: dictionary passing */
        struct {
            TypeClassInstance *instance;
            char dict_name[128];  /* "dict_<Class>_<type>" singleton name */
            char method_name[64]; /* sanitized method field name; '\0' = address-only mode */
            /* van-laarhoven-lens-composition: an AMBIENT dict value -- the dict for
             * the ENCLOSING constrained rank-2 fn's own constraint, forwarded into
             * a nested constrained rank-2 call at the same abstract functor.  When
             * emitted inside a dict-clone body it lowers to the clone's dict
             * PARAMETER (ctx->dict_dispatch_param_cname); otherwise it falls back to
             * `instance`'s singleton (the plain carrier-base form). */
            bool is_ambient;
        } dict_; /* (dict Instance) */

        /* Phase 11: Struct operations */
        struct { Expr **field_values; uint32_t n_fields; } make_struct_; /* (make-struct Name v1...) */
        struct { Expr *struct_expr; uint32_t field_idx;
                 /* CONV-S0/S4: the receiver is a single-variant record ADT;
                  * adt_def/adt_ctor carry the sole variant and codegen reads
                  * `((tur_adt_X *)v)->as.Ctor._<idx>`. */
                 const struct AdtDef *adt_def; const struct CtorDef *adt_ctor;
               } get_field_; /* (.field s) - field read */
        /* Phase DS3: (set! (.field s) v) - field write.  receiver_is_rc is true
         * when the receiver expression is rc<ADT> (auto-deref); codegen casts
         * through the rc-block's value pointer.
         * CONV-S1 seam 4: the receiver is a single-variant record ADT;
         * adt_def/adt_ctor drive the ADT member path. */
        struct { Expr *receiver; Expr *value; uint32_t field_idx; bool receiver_is_rc; const struct AdtDef *adt_def; const struct CtorDef *adt_ctor; } set_field_;

        struct { Expr **items; uint32_t n; }                               program;

        /* Phase X3: Set literal */
        struct { Expr **items; uint32_t n; }                               set_lit_;

        /* Phase M0: Module system */
        struct { struct DefModule *mod; }                                   defmodule_;
        /* Phase 21: Serializable continuations */
        struct { Expr *body; }         serial_reset_; /* (serial-reset body) */
        struct {
            Expr *k_fn;   /* function receiving the serial continuation */
            Expr *body;   /* body expression */
        } serial_shift_;
        /* Phase HRT1: Higher-ranked types */
        struct {
            struct Expr    *inner;           /* the fn/closure being wrapped */
            struct Binding *wrapper_binding; /* the __poly_N wrapper thunk binding */
            /* turi-dict-passing-plan: when the wrapped fn is CONSTRAINED and
             * was dict-cloned for this rank-2 crossing, the clone's global
             * binding.  The interpreter evaluates the poly value to the CLONE
             * (whose leading dict params the elaborated call site supplies)
             * instead of the original -- the tree-walking analogue of the
             * wrapper targeting the clone on the compiled path.  NULL for
             * unconstrained wraps. */
            struct Binding *dict_clone_binding;
            /* Phase CCL: true when inner is a fat closure (void*) rather than a
             * named function.  The emitter packs it into tur_poly_fn_t at the
             * call site instead of emitting a (tur_poly_fn_t){ NULL, wrapper }. */
            bool            is_closure;
            /* Slice 3 (constrained-hkt-forall): true when the sink is a FORALL
             * carrier (rank-2 poly param), so a by-value aggregate result must be
             * heap-boxed by a carrier-spill shim to ride the uniform int64
             * carrier.  False for a typed `:fn` carrier / monad continuation,
             * which the concrete-cast call site consumes by value (no spill). */
            bool            boxes_aggregate;
        } poly_wrap_;
        struct { struct Expr *inner; } ascribe_; /* (:: expr type) — type erased at codegen */
        /* A#1: fat-closure auto-shim.  inner is a bare (non-capturing) fn value;
         * the emitter generates an env-ignoring wrapper thunk and a heap fat
         * struct { thunk, orig_fn_ptr } so a ^fat consumer can fat-call it. */
        /* `static_ok` OPTS IN to the file-scope { shim, orig } box the emitter
         * can use when `inner` is a global fn (fn-value-fat-normalization).
         * Default off, deliberately: the box is then shared between sites and
         * lives forever, which is only sound at a sink that never DROPS its
         * argument.  A `^fat` sink may (tests/fixtures/closure-drop-glue-
         * fatshim calls TUR_CLOSURE_DROP on one), and an owning struct
         * fn-field does at scope exit; the no-op drop glue keeps both correct,
         * but GCC cannot see that through the inlined tur_closure_drop and
         * reports `'free' called on unallocated object`.  Only the normalized
         * NOMINAL param slot sets this -- nothing drops a box handed to one,
         * which is exactly why that slot leaked a box per call. */
        struct { struct Expr *inner; bool static_ok; } fn_to_fat_;
        /* SC7: convert a tur_poly_fn_t {env,fn} (a typeclass-method closure
         * param) into a single-int64 fat-closure handle so a ^fat consumer can
         * fat-call it.  inner is the tur_poly_fn_t value; the emitter heap-boxes
         * { __tur_poly_to_fat1, fn, env } and yields the box pointer. */
        /* poly-to-fat-typed-shim-plan: sink_fn_type is the ^fat consumer's
         * declared fn signature for this argument slot (a TY_FN), threaded from
         * elaboration so the emitter can pick a typed slot-0 shim whose ABI
         * matches the typed-thunk cast the sink will apply.  NULL for the int64
         * carrier case (keeps __tur_poly_to_fat1). */
        struct { struct Expr *inner; const struct Type *sink_fn_type; } poly_to_fat_;
        /* Phase HRT2: Existential types.
         * Phase EX1c: optional resolved constraint witnesses (one per constraint
         * in the target existential type).  NULL when the target has no
         * constraints. */
        struct {
            struct Expr           *value;     /* the value being packed */
            TypeClassInstance    **witnesses; /* arena-allocated; length n_witnesses */
            uint8_t                n_witnesses;
        } exists_pack_;
        struct {
            struct Expr    *packed;      /* the packed existential expression */
            struct Binding *var_binding; /* binding for v (the unboxed inner value) */
            struct Expr    *body;        /* the open body expression */
        } exists_open_;
        /* Existential-open method dispatch: a `(.m v ...)` call whose receiver
         * `v` was bound by an `open` over a constraint-carrying existential.
         * The receiver type is erased to the int64 carrier, so the call cannot
         * pick a static instance; instead it reads the per-constraint witness
         * vtable bundled in the existential record (located via open_binding)
         * and calls the method pointer at method_idx through the carrier ABI. */
        struct {
            struct Binding   *open_binding; /* the `v` binding of the enclosing open */
            const TypeClass  *typeclass;    /* the constraint class being dispatched */
            uint8_t           witness_idx;  /* index into the record's witnesses[] */
            uint8_t           method_idx;   /* index of the method within the class */
            struct Expr     **args;         /* call args; args[0] is the receiver `v` */
            uint32_t          n_args;
        } exists_dispatch_;
        /* Phase G0: ADT definition */
        struct {
            AdtDef  *def;      /* the ADT being defined */
            Binding *binding;  /* global binding for the ADT type */
        } defdata_;
        /* Phase G1: GADT definition — same layout as defdata_ */
        struct {
            AdtDef  *def;      /* the GADT being defined (is_gadt=true) */
            Binding *binding;  /* global binding for the GADT type */
        } defgadt_;
        /* Phase G0: match expression */
        struct {
            struct Expr *scrutinee;
            MatchArm    *arms;      /* arena-allocated array */
            uint32_t     n_arms;
        } match_;
        /* IT4: Tagged union injection — wraps a member value into tur_tagged_t */
        struct {
            int64_t     tag_idx;  /* member index (for TY_UNION) or TypeKind (for TY_ANY) */
            struct Expr *value;   /* the value being injected */
        } union_inject_;
        /* IT4 gradual typing */
        struct { struct Expr *value; } any_type_of_;   /* (type-of x) — x must be TY_ANY */
        /* TY3: (is? x T) — runtime type test; emits TUR_GETTAG(x) == test_tag. */
        /* type-of-cast-kind-granularity: `test_type` carries the NAMED target
         * (a struct/ADT) so emit can allocate the same per-monomorph box id the
         * inject site does; `test_tag` remains the TypeKind for primitives and
         * as the fallback when no named type was resolved. */
        struct { struct Expr *value; int64_t test_tag; Type test_type; } any_is_;
        /* TY2.3: (cast x T) — checked downcast; panics on tag mismatch. */
        struct {
            struct Expr *value;
            TypeKind     target_kind;
        } any_cast_;
        /* DV0: Dynamic var declaration */
        struct {
            DynVarEntry        *entry;      /* the registered dynvar (name, value_type, index) */
            struct Expr        *root_expr;  /* elaborated root value expression */
        } defdynamic_;
        /* DV1: Read the current value of a dynamic var */
        struct {
            DynVarEntry        *entry;      /* which dynamic var to read */
        } dynvar_read_;
        /* DV1: (binding [*v* expr ...] body) -- push override frames, run body, pop */
        struct {
            DynBinding         *pairs;      /* override pairs (var + override expr) */
            uint32_t            n_pairs;
            struct Expr        *body;       /* body evaluated with overrides active */
        } dynvar_binding_;
        /* DV1: (set! *name* expr) on a dynamic var -- mutate top binding frame */
        struct {
            DynVarEntry        *entry;      /* which dynamic var to mutate */
            struct Expr        *value;      /* new value */
        } dynvar_set_;
        /* GF1: Generator expression */
        struct {
            GenDef             *def;        /* generator definition */
        } gen_;
        /* GF1: Yield expression -- inside a gen body */
        struct {
            struct Expr        *value;      /* value being yielded */
            uint32_t            yield_id;   /* 1-based yield point index */
        } yield_;
        /* GF1: Advance a generator -- returns ptr<void> (option) */
        struct {
            struct Expr        *gen_expr;   /* the generator value */
            GenDef             *def;        /* generator definition */
        } gen_next_;
        /* GF1: Check if a generator is exhausted */
        struct {
            struct Expr        *gen_expr;   /* the generator value */
        } gen_done_;
        /* AR8: variadic rest-list construction */
        struct {
            struct Expr **items;   /* the surplus args to pack into a cons list */
            uint32_t     n;        /* number of items */
            TypeKind     item_kind; /* element type (determines cast in emitter) */
        } cons_list_;
        /* SYM0: runtime symbol literal -- carries the interned compile-time Symbol */
        struct {
            const Symbol *sym;     /* interned name (e.g. "foo" for :foo) */
        } sym_lit_;
        /* CPS2: explicit continuation application — used in CPS-lowered functions.
         * Represents applying continuation `k` to a result value: `k(v)`.
         * `cont` is the continuation expression (type TY_CONT); `value` is the
         * argument passed to it. */
        struct {
            struct Expr *cont;     /* the continuation to apply */
            struct Expr *value;    /* the value to pass to the continuation */
        } cps_cont_app_;
    } as;
};

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span);

void  expr_print(Buf *b, const Expr *e);   /* debug only */

/* Map a well-known stdlib helper name (e.g. "float->int") to the stdlib file
 * that defines it (e.g. "stdlib/math.tur"), or NULL when there is no hint.
 * The compiled path uses this to suggest the exact `(load ...)` line on an
 * unknown call head (elab_call.c UCH1); the interpreter reuses it to emit the
 * same hint when a deferred runtime-dispatch head turns out to be unbound. */
const char *tur_stdlib_load_hint(const char *name);

/* True when `name` is dispatched unconditionally as a special form in call-head
 * position, so a `defn`/`defmacro` of that name is unreachable by its bare
 * name.  Drives TUR-W0042; see the table in elab_call.c for the membership
 * rule (deliberately-shadowable and arity-gated forms are excluded). */
bool tur_name_is_reserved_special_form(const char *name);

/* Emit TUR-W0042 at `span` when `name` collides with a reserved special form.
 * `form_kind` names the definition form in the message ("defn", "defmacro"). */
void tur_warn_if_shadows_special_form(const Symbol *name, Span span,
                                      const char *form_kind);

/* Map a legacy "store pointers as :int and hand-roll allocation" form that was
 * never a Turmeric language operator (`sizeof`, `float64*`/`float32*` raw-pointer
 * indexing, `declare`) to a one-line migration pointer, or NULL when the name is
 * not one of them.  Such a name surfaces as a bare "unknown function or operator"
 * call head with no path forward; the hint says what to use instead.  See
 * docs/guides/structs-guide.md (legacy pointer/struct migration). */
const char *tur_legacy_form_hint(const char *name);

#endif
