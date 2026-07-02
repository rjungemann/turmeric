#ifndef TUR_ELAB_INTERNAL_H
#define TUR_ELAB_INTERNAL_H

/* Shared internals for the elab_*.c translation units. This header is not
 * installed; the public elaborator API lives in elab.h. It exposes the Elab
 * state struct, supporting types, global linting flags, and prototypes for
 * every elaborator helper so the split translation units can call across
 * file boundaries. */

#include "elab.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "builtins.h"
#include "diag.h"
#include "reader.h"    /* Phase M2: read_all for module loading */
#include "typeclass.h"  /* Phase 15 */
#include "types.h"
#include "effect.h"    /* Phase 19 */
#include "globals.h"   /* ET4: g_effect_types_enabled */
/* Phase U5: External declarations for global unsafe linting configuration */
extern uint32_t g_unsafe_max_lines;
extern bool g_unsafe_warn_nested;
extern bool g_unsafe_require_safety;
extern bool g_unsafe_stats_enabled;
extern bool g_lint_unsafe_enabled;
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;
/* Phase R6: Result/panic linting flags */
extern bool g_warn_unused_result;
/* F4 (cross-plan-followups): --Werror=deprecated promotes ^deprecated
 * warnings to errors so a clean build can gate against new uses. */
extern bool g_werror_deprecated;
extern bool g_lint_panic;
/* Phase C2: --no-contracts -- strip contract checks during elaboration */
extern bool g_no_contracts;
/* Phase G1: GADT feature flag */
extern bool g_gadt_enabled;
/* LT0: Linear types feature flag */
extern bool g_linear_enabled;
/* UT0: Uniqueness types feature flag */
extern bool g_unique_enabled;
/* ST0: Substructural types feature flag */
extern bool g_substructural_enabled;
/* SS0a: Session types feature flag */
extern bool g_sessions_enabled;
/* IT0: Union types feature flag */
extern bool g_union_types_enabled;
extern bool g_intersection_types_enabled;
#define ELAB_MAX_MACRO_EXPANSION_DEPTH 256

/* ---- shared file-scope state ---- */
/* MS2: Track whether we're inside an atomically block for TUR-E0502 checking.
 * Declared here (before elab_resume) and also set in elab_atomically below. */
extern bool elab_in_atomically;
/* ============================================================================
 * Phase 20: Software Transactional Memory - Elaboration
 * ============================================================================ */

/* Track whether we're inside an stm block for TUR-E0009 checking */
extern bool elab_in_stm;

/* ---- shared type definitions ---- */

/* SS5: Global protocol interaction tree (compile-time only, arena-allocated).
 * Represents the multi-party interaction structure of a defprotocol. */
typedef enum GlobalInteractionKind {
    GI_MSG,      /* (-> From To T) -- From sends T to To */
    GI_CHOICE,   /* (choice From [label branch] ...) -- From selects a labelled branch */
    GI_LOOP,     /* (loop label body...) -- recursive global protocol */
    GI_CONTINUE, /* (continue label) -- jump back to loop label */
    GI_END,      /* end of protocol */
} GlobalInteractionKind;

typedef struct GlobalBranch {
    const char               *label;
    struct GlobalInteraction *body;
} GlobalBranch;

typedef struct GlobalInteraction {
    GlobalInteractionKind kind;
    union {
        struct {
            const char               *from;  /* interned role name */
            const char               *to;    /* interned role name */
            struct Type              *msg;   /* message type */
            struct GlobalInteraction *rest;  /* next step */
        } msg;
        struct {
            const char               *decider;   /* role making the choice */
            GlobalBranch             *branches;  /* arena-allocated array */
            int                       n_branches;
            struct GlobalInteraction *rest;      /* steps after all branches (NULL if diverge) */
        } choice;
        struct {
            const char               *label;
            struct GlobalInteraction *body;
            struct GlobalInteraction *rest;  /* steps after loop (if any) */
        } loop;
        struct {
            const char *label; /* loop label to continue to */
        } cont;
    };
} GlobalInteraction;

/* SS5: Registry entry for a declared global protocol. */
typedef struct GlobalProtocol {
    const char  *name;       /* interned protocol name */
    struct Type *type;       /* TY_GLOBAL type node */
} GlobalProtocol;

/* ---- scope ---- */

/* Phase 12: Borrow tracking in Scope */
typedef enum BorrowKind {
    BK_IMMUT,   /* &T - immutable borrow */
    BK_MUT,     /* &mut T - mutable borrow */
} BorrowKind;

typedef struct ScopeBorrow {
    Binding *binding;       /* The binding being borrowed */
    BorrowKind kind;        /* BK_IMMUT or BK_MUT */
    struct ScopeBorrow *next; /* Next in the list */
} ScopeBorrow;

typedef struct Scope {
    struct Scope *parent;
    Binding     **bindings;
    uint32_t      n;
    uint32_t      cap;
    /* Phase 12: Active borrows in this scope */
    ScopeBorrow  *borrows;
} Scope;

/* ---- elaborator state ---- */

/* Phase M2: A loaded module's exported bindings. */
typedef struct ElabModule {
    const Symbol *name;          /* module name (interned) */
    Binding     **exports;       /* exported bindings (each has .name for lookup) */
    uint32_t      n_exports;
    /* Phase M4: exported macros from this module */
    struct MacroDef **exported_macros;
    uint32_t         n_exported_macros;
    /* PR5-3-D: Effects exported by this module */
    Effect      **exported_effects;
    uint32_t      n_exported_effects;
    bool          is_loading;    /* circular import detection */
} ElabModule;

/* Forward-declared so this header doesn't drag in reader_macros.h. */
struct ReaderMacroRegistry;

typedef struct Elab {
    Arena       *arena;
    SymbolTable *st;
    Scope       *scope;     /* current */
    Scope        global;
    uint32_t     next_id;
    uint32_t     next_gensym_id;  /* Phase 6: for generating unique symbol names */
    /* Transitive-RM: shared reader-macro registry, set by the driver
     * before elaborate_program runs. Module loaders (elab_module.c,
     * elab_toplevel.c) read with this so imported files see the same
     * user macros the entry file did. May be NULL. */
    struct ReaderMacroRegistry *user_macros;

    /* Phase 3: Collect file-scope definitions (FN_DEF) from nested contexts */
    Expr       **file_scope_defs;
    uint32_t    n_file_scope_defs;
    uint32_t    cap_file_scope_defs;
    /* Phase 15: Typeclass environment */
    TypeClassEnv typeclass_env;

    /* Cached symbols for special-form dispatch. */
    const Symbol *sym_def;
    const Symbol *sym_define; /* internal define -- body form only */
    const Symbol *sym_let;
    const Symbol *sym_letstar; /* let* -- sequential-binding let (each binding sees prior ones) */
    const Symbol *sym_letrec; /* letrec -- recursive/mutually-recursive let */
    const Symbol *sym_if;
    const Symbol *sym_do;
    const Symbol *sym_unsafe;
    const Symbol *sym_when;
    const Symbol *sym_unless;
    const Symbol *sym_case;
    const Symbol *sym_set;
    const Symbol *sym_while;
    const Symbol *sym_defn;     /* Phase 2 */
    const Symbol *sym_fn;       /* Phase 2 */
    const Symbol *sym_c_fn;     /* typed-c-abi-function-pointers: (c-fn [A...] R) bare C fn-ptr type */
    const Symbol *sym_lambda;   /* λ — Unicode alias for fn */
    const Symbol *sym_extern_c; /* Phase 2 */
    const Symbol *sym_caret_mut;     /* ^mut */
    const Symbol *sym_caret_private; /* ^private — module-private visibility annotation */
    /* Phase P3: HAMT lowering */
    const Symbol *sym_caret_persistent; /* ^persistent — immutable map annotation */
    /* LT0: Linear types */
    const Symbol *sym_caret_linear;     /* ^linear — linear value annotation */
    /* UT0: Uniqueness types */
    const Symbol *sym_caret_unique;     /* ^unique -- unique value annotation */
    /* ST0: Substructural types */
    const Symbol *sym_caret_affine;     /* ^affine -- affine value annotation */
    const Symbol *sym_caret_relevant;   /* ^relevant -- relevant value annotation */
    /* LB1: ^borrow -- non-consuming parameter annotation for linear/affine handles */
    const Symbol *sym_caret_borrow;     /* ^borrow -- borrow (read without consuming) annotation */
    const Symbol *sym_caret_fat;        /* ^fat -- fat-closure-consuming parameter (A#1) */
    const Symbol *sym_caret_extends;    /* ^extends -- effect hierarchy parent annotation (ET4) */
    const Symbol *sym_caret_capability; /* ^capability -- coarse capability effect tag (stdlib-effect-rows) */
    /* MS1: multi-shot continuation annotation */
    const Symbol *sym_caret_multishot;        /* ^multishot -- MS1: safe multi-shot via snapshot semantics */
    /* F4 (cross-plan-followups): ^deprecated definition annotation */
    const Symbol *sym_caret_deprecated;
    const Symbol *sym_map_new;    /* map-new - create new map */
    const Symbol *sym_assoc;      /* assoc - insert/update key-value */
    const Symbol *sym_dissoc;     /* dissoc - delete key */
    const Symbol *sym_map_get;    /* get - get value by key */
    const Symbol *sym_map_has;    /* has? - check key exists */
    const Symbol *sym_map_count;  /* count - number of entries */
    const Symbol *sym_map_merge;  /* merge - merge two maps */
    /* HAMT function symbols for lowering */
    const Symbol *sym_hamt_new;   /* hamt/new */
    const Symbol *sym_hamt_set;   /* hamt/set */
    const Symbol *sym_hamt_del;   /* hamt/del */
    const Symbol *sym_hamt_get;   /* hamt/get */
    const Symbol *sym_hamt_has;   /* hamt/has? */
    const Symbol *sym_hamt_count; /* hamt/count */
    const Symbol *sym_hamt_merge; /* hamt/merge */
    const Symbol *sym_hamt_hash_ptr; /* hamt_hash_ptr */
    const Symbol *sym_defer;      /* Phase 4 */
    const Symbol *sym_return;     /* return - early return with defer firing */
    /* Phase 5 */
    const Symbol *sym_ref;        /* ref */
    const Symbol *sym_deref;      /* @ (deref operator) - stored as symbol for parsing */
    const Symbol *sym_drop;       /* drop! - explicit drop for ref<T> */
    const Symbol *kw_else;         /* :else (the symbol named "else") */
    const Symbol *kw_derive;       /* :as (the symbol named "as") - for inline-C */
    /* LT3: lref<T> — linear owning pointer */
    const Symbol *sym_lref;        /* lref (type name in type expressions) */
    const Symbol *sym_lref_new;    /* lref/new */
    /* Phase 9: rc<T> + weak<T> */
    const Symbol *sym_rc_of;       /* rc/of */
    const Symbol *sym_rc_clone;    /* rc/clone */
    const Symbol *sym_rc_drop;     /* rc/drop */
    const Symbol *sym_rc_ptr;      /* rc->ptr */
    const Symbol *sym_rc_strong_count; /* rc/strong-count */
    const Symbol *sym_rc_from_ref; /* rc/from-ref */
    const Symbol *sym_ref_from_rc; /* ref/from-rc */
    const Symbol *sym_weak;        /* weak */
    const Symbol *sym_upgrade;     /* upgrade */
    const Symbol *sym_weak_pred;   /* weak? */
    const Symbol *sym_ref_pred;    /* ref? */
    /* Phase 18: Delimited continuations */
    const Symbol *sym_reset;      /* reset */
    const Symbol *sym_shift;      /* shift */
    const Symbol *sym_shift0;     /* shift0 */
    const Symbol *sym_call_cc;    /* call/cc (sugar: (reset (shift k (f k)))) */
    const Symbol *sym_escape;     /* escape (sugar: (reset (shift k (f (fn [_] k))))) */
    /* Phase B2: Cloneable continuations */
    const Symbol *sym_cloneable_reset;   /* cloneable-reset */
    const Symbol *sym_cloneable_shift;   /* cloneable-shift */
    const Symbol *sym_call_cc_star;       /* call/cc* (sugar for cloneable call/cc) */
    /* Phase 21: Serializable continuations */
    const Symbol *sym_serial_reset;      /* serial-reset */
    const Symbol *sym_serial_shift;      /* serial-shift */
    /* Phase 19: Algebraic effects */
    const Symbol *sym_defeffect;  /* defeffect */
    const Symbol *sym_perform;    /* perform */
    const Symbol *sym_handle;     /* handle */
    const Symbol *sym_try_with;   /* try-with (sugar for handle) */
    const Symbol *sym_with_handler; /* with-handler (sugar for handle; async-friendly alias) */
    const Symbol *sym_with;         /* WITH-V0: (with src [field value ...]) functional struct update */
    const Symbol *sym_resume;     /* resume */
    const Symbol *sym_discontinue;/* discontinue */
    const Symbol *sym_k;          /* continuation parameter name k */
    const Symbol *sym_effect_unsafe; /* built-in effect name: Unsafe */
    /* ET3: handler type expression and compose-handlers */
    const Symbol *sym_handler_type;     /* "handler" type expression keyword */
    const Symbol *sym_compose_handlers; /* "compose-handlers" call form */
    /* Phase U3: Unsafe primitives - pointer operations */
    const Symbol *sym_ptr_deref;   /* ptr-deref */
    const Symbol *sym_ptr_write;  /* ptr-write */
    const Symbol *sym_ptr_add;     /* ptr-add */
    const Symbol *sym_ptr_sub;     /* ptr-sub */
    const Symbol *sym_ptr_nullq;   /* ptr-null? */
    const Symbol *sym_ptr_of;      /* ptr-of */
    /* Phase U3: Unsafe primitives - type casting */
    const Symbol *sym_unsafe_cast; /* unsafe-cast */
    const Symbol *sym_reinterpret; /* reinterpret */
    const Symbol *sym_transmute;   /* transmute */
    /* Phase U3: Unsafe primitives - unchecked array ops */
    const Symbol *sym_array_get_unchecked;  /* array-get-unchecked */
    const Symbol *sym_array_set_unchecked;  /* array-set-unchecked */
    /* Phase U3: Unsafe primitives - raw memory */
    const Symbol *sym_raw_malloc;  /* raw-malloc */
    const Symbol *sym_raw_free;    /* raw-free */
    const Symbol *sym_raw_realloc; /* raw-realloc */
    const Symbol *sym_raw_memcpy;  /* raw-memcpy */
    const Symbol *sym_raw_memset;  /* raw-memset */
    /* Phase U3: Unsafe primitives - FFI */
    const Symbol *sym_c_call;      /* c-call */
    const Symbol *sym_dlopen;      /* dlopen */
    const Symbol *sym_dlsym;       /* dlsym */
    const Symbol *sym_dlclose;     /* dlclose */
    /* Phase 10: GC */
    const Symbol *sym_gc_force;    /* gc! */
    const Symbol *sym_gc_enable;   /* gc-enable! */
    const Symbol *sym_gc_disable;  /* gc-disable! */
    /* Phase 6: Macro system */
    const Symbol *sym_defmacro;   /* defmacro */
    const Symbol *sym_quote;      /* quote */
    const Symbol *sym_quasiquote; /* quasiquote */
    const Symbol *sym_unquote;    /* unquote */
    const Symbol *sym_unquote_splicing; /* unquote-splicing */
    const Symbol *sym_gensym;     /* gensym */
    const Symbol *sym_thread;    /* -> threading macro */
    const Symbol *sym_thread_last; /* ->> threading macro */
    /* Phase 11: defstruct */
    const Symbol *sym_defstruct;   /* defstruct */
    const Symbol *sym_make_struct; /* make-struct */
    const Symbol *sym_default_of;  /* M2b: default-of */
    /* SI4-C: defopaque -- named opaque int64_t newtype for REPL type tags */
    const Symbol *sym_defopaque;   /* defopaque */
    const Symbol *kw_copy;        /* :copy keyword for defstruct */
    const Symbol *kw_move;        /* :move keyword for defstruct */
    const Symbol *kw_linear;      /* LT4: :linear keyword for defstruct (exactly-once) */
    const Symbol *kw_affine;      /* :affine keyword for defopaque (at-most-once) */
    const Symbol *kw_heap;        /* :heap keyword for defstruct (typed-pointer ABI) */
    const Symbol *kw_no_auto_ctor;/* CTOR-V0: :no-auto-ctor keyword for defstruct */
    /* Phase 12: Borrow traits */
    const Symbol *sym_borrow;      /* & symbol for immutable borrow */
    const Symbol *sym_borrow_mut;  /* &mut for mutable borrow */
    /* Phase 15: Typeclasses */
    const Symbol *sym_defclass;    /* defclass */
    const Symbol *sym_definstance; /* definstance */
    /* Phase HKT H5: defkind — kind alias declarations */
    const Symbol *sym_defkind;     /* defkind */
    /* Phase HKT-P2: defrec — recursive type binders */
    const Symbol *sym_defrec;      /* defrec */
    const Symbol *sym_deftype;      /* deftype */
    /* Phase TA1: defalias — primitive type alias declarations */
    const Symbol *sym_defalias;

    const Symbol **type_alias_names;  /* interned alias name symbols */
    TypeKind      *type_alias_kinds;  /* resolved target TypeKind */
    uint32_t       n_type_aliases;    /* number of declared aliases */
    uint32_t       cap_type_aliases;  /* allocated capacity */
    /* Phase HKT-P1: type-app — type-level application */
    const Symbol *sym_type_app;    /* type-app */
    /* Phase HRT0: Higher-ranked type quantifiers (type-level only; reject in expression position) */
    const Symbol *sym_forall;      /* forall */
    const Symbol *sym_exists;      /* exists */
    const Symbol *sym_forall_u;    /* ∀ — Unicode alias for forall */
    const Symbol *sym_exists_u;    /* ∃ — Unicode alias for exists */
    /* Phase HRT1: Rank-2 type expression forms */
    const Symbol *sym_arrow;       /* -> (function type constructor in type expressions) */
    const Symbol *sym_ascribe;     /* :: (type ascription operator) */
    /* Phase HRT2: Existential type intro/elim */
    const Symbol *sym_pack;        /* pack (existential introduction) */
    const Symbol *sym_open;        /* open (existential elimination) */
    /* Phase HKT (v2): reserved typeclass names — user definitions rejected with diagnostic */
    const Symbol *sym_hkt_Functor;
    const Symbol *sym_hkt_Applicative;
    const Symbol *sym_hkt_Monad;
    const Symbol *sym_hkt_Traversable;
    const Symbol *sym_hkt_Foldable;
    /* Phase R2: Panic */
    const Symbol *sym_panic;
    const Symbol *sym_panic_with;
    const Symbol *sym_catch_unwind;
    const Symbol *sym_catch_panic_of;
    /* Phase R5: no-unwind attribute */
    const Symbol *sym_no_unwind_attr;
    /* #[used] attribute: retain a defn with external C linkage (see
     * Binding.retain_c_linkage). */
    const Symbol *sym_used_attr;
    /* Phase M6: (export-as "c_name") attribute for explicit C symbol naming */
    const Symbol *sym_export_as_attr;
    /* M2a (end-to-end-monomorphization-plan): #{Construct} marker on a
     * polymorphic stdlib constructor defn. Picked up in elab_fns.c when the
     * defn's effect-row map is parsed. */
    const Symbol *sym_construct_attr;
    /* M5 residual-straddle retirement: #{ByVal} marker symbol.  See the
     * `prefer_byvalue_spec` comment on Binding in expr.h for the rationale
     * and demolition date. */
    const Symbol *sym_byval_attr;
    const Symbol *sym_panic_payload_type;
    const Symbol *sym_panic_payload_value;
    const Symbol *sym_panic_payload_file;
    const Symbol *sym_panic_payload_line;
    const Symbol *sym_panic_payload_downcast;
    /* Phase R1: ? operator (reserved; not yet implemented) */
    const Symbol *sym_question;
    /* Phase T19-B: thread-spawn form — (thread-spawn closure) */
    const Symbol *sym_thread_spawn;
    /* Phase T21-F: async/await forms */
    const Symbol *sym_async;
    const Symbol *sym_await;
    /* Phase SEL1: fair multi-channel select */
    const Symbol *sym_select;
    const Symbol *sym_recv;   /* :recv keyword */
    const Symbol *sym_send;   /* :send keyword */
    /* Phase 20: Software Transactional Memory */
    const Symbol *sym_stm;           /* stm */
    const Symbol *sym_atomically;    /* atomically */
    const Symbol *sym_retry;         /* retry */
    const Symbol *sym_check;         /* check */
    const Symbol *sym_or_else;       /* or-else */
    const Symbol *sym_tvar;          /* tvar (type name) */
    const Symbol *sym_new;           /* new (for tvar/new, etc.) */
    const Symbol *sym_read;          /* read (for tvar/read, etc.) */
    const Symbol *sym_write;         /* write (for tvar/write, etc.) */
    const Symbol *sym_modify;        /* modify (for tvar/modify, etc.) */
    const Symbol *sym_swap;          /* swap (for tvar/swap, etc.) */
    const Symbol *sym_cas;           /* cas (for tvar/cas, etc.) */
    const Symbol *sym_tmvar;         /* tmvar (type name) */
    const Symbol *sym_tchan;         /* tchan (type name) */
    const Symbol *sym_tsem;          /* tsem (type name) */
    const Symbol *sym_dosync;        /* dosync (macro) */
    const Symbol *sym_with_tvar;     /* with-tvar (macro) */
    /* Legacy symbols for TVar:: syntax (kept for compatibility) */
    const Symbol *sym_tvar_new;      /* tvar/new */
    const Symbol *sym_tvar_read;     /* tvar/read */
    const Symbol *sym_tvar_write;    /* tvar/write */
    const Symbol *sym_tvar_modify;   /* tvar/modify */
    const Symbol *sym_tvar_swap;     /* tvar/swap */
    const Symbol *sym_tvar_cas;      /* tvar/cas */
    /* Phase N: (as TargetType expr) numeric cast */
    const Symbol *sym_as;
    /* IT4 gradual typing */
    const Symbol *sym_type_of;  /* (type-of x) — cstr name of an any-typed value's type */
    const Symbol *sym_cast;     /* (cast x T) — unsafe downcast from any to T */
    const Symbol *sym_is_q;     /* TY3: (is? x T) — runtime type test on an any value */
    /* Phase 13: Lifetime annotations */
    /* We recognize lifetime annotations as symbols starting with '\'' */
    /* No specific symbol needed - we check the symbol name at runtime */
    /* Macro storage: symbol -> MacroDef */
    /* For now, use a simple approach: store macros in a list */
    struct MacroDef **macros;
    uint32_t n_macros;
    uint32_t cap_macros;
    /* Phase 19: Algebraic effects */
    EffectEnv *effect_env;  /* Global effect registry */
    /* Phase 19 TUR-E0008: Effect-scope tracking.
     * handled_effect_names: stack of effect names covered by enclosing handle.
     * fn_body_depth: depth inside defn/fn bodies; TUR-E0008 only fires at 0. */
    const Symbol **handled_effect_names;
    uint32_t n_handled_effects;
    uint32_t cap_handled_effects;
    uint32_t fn_body_depth;
    const Symbol *current_fn_name;  /* Phase R6: track current function name for linting */
    uint32_t unsafe_depth;
    uint32_t macro_expand_depth;
    /* Phase U5: Unsafe linting configuration */
    uint32_t unsafe_max_lines;      /* max lines in unsafe block before warning (0 = disabled) */
    bool     unsafe_warn_nested;     /* warn on nested unsafe blocks */
    bool     unsafe_require_safety; /* require ;;; SAFETY: comments in unsafe blocks */
    bool     unsafe_stats_enabled;  /* track unsafe block statistics */
    uint32_t unsafe_block_count;    /* count of unsafe blocks seen */
    uint32_t unsafe_total_lines;    /* total lines in unsafe blocks */
    /* Phase 19: cont? predicate */
    const Symbol *sym_cont_pred; /* cont? */
    /* Phase G0: ADT registry - maps ADT names to AdtDef */
    AdtDef **adt_defs;
    uint32_t n_adt_defs;
    uint32_t cap_adt_defs;
    /* Phase G0: ADT special symbols */
    const Symbol *sym_defdata;
    const Symbol *sym_match;
    /* Phase G1: GADT special symbols */
    const Symbol *sym_defgadt;
    const Symbol *sym_colon; /* bare ":" used as annotation separator in defgadt */
    /* IT0: Union type pipe separator "|" */
    const Symbol *sym_pipe;        /* "|" -- used in (A | B | C) union type expressions */
    /* IT2: Intersection type ampersand separator "&" */
    const Symbol *sym_ampersand;   /* "&" -- used in (A & B & C) intersection type expressions */
    /* LS1: the lifetime context of the signature currently being elaborated, so
     * that a borrow *type* annotation like &'a int can intern its 'a into stable
     * per-function LifetimeIds.  NULL outside defn signature parsing (a borrow
     * type encountered with no active context simply gets no lifetime). */
    LifetimeContext *cur_lifetime_ctx;
    /* Phase RT (return-type-directed dispatch): a narrow, opt-in expected-type
     * channel.  Pushed by elab_ascribe ((:: e T)) and typed let binders
     * (let [x : T e]) before elaborating the inner/value expression; read by
     * elab_call to resolve return-resolved typeclass constraints from the
     * expected result type.  NULL everywhere it is not explicitly pushed, so
     * the common path is unaffected.  This is a one-slot stack: the call
     * elaborator clears it for nested sub-calls so it applies only to the
     * outermost call of the ascribed expression. */
    Type *expected_type;
    /* Phase G2: current per-arm skolem environment (NULL outside GADT match arms) */
    SkolemEnv *g2_skolem_env;
    /* Phase G2: GADT constructor whose arm is currently being elaborated.
     * NULL outside GADT match arms.  Used by diagnostics to name which
     * constructor's return-type annotation caused a type refinement. */
    const CtorDef *g2_current_ctor;
    /* KB-025: named type variables quantified by the enclosing fn/defn
     * signatures (params + return type), accumulated outermost-first.  A GADT
     * match arm whose result type is a named type variable NOT in this set is
     * a skolem that escapes the arm into a concrete return position. */
    const char *sig_tyvars[32];
    uint8_t      n_sig_tyvars;
    /* Phase G3: coerce special form */
    const Symbol *sym_coerce;
    /* Phase G3: (~ a b) equality constraint notation */
    const Symbol *sym_tilde;
    /* Phase M0: Module system */
    const Symbol *sym_defmodule;  /* defmodule */
    const Symbol *sym_export;     /* export */
    const Symbol *sym_effect;     /* effect — used to parse (effect Name) in export/refer lists */
    const Symbol *sym_import;     /* import */
    const Symbol *sym_load;       /* load */
    const Symbol *kw_as;          /* :as */
    const Symbol *kw_refer;       /* :refer */
    bool has_defmodule;           /* whether defmodule has been seen in this file */
    /* Phase M1: Module namespace system */
    const Symbol    *current_module_name; /* name of module being elaborated, or NULL */
    const DefModule *current_module;      /* DefModule being elaborated, or NULL */
    /* Phase M2: Module registry */
    const char      *module_base_dir;     /* base dir for resolving module file paths */
    const char      *module_stdlib_dir;   /* stdlib fallback dir (e.g. "stdlib") */
    const char     **module_include_dirs; /* extra search dirs from -I flags / spices */
    int              n_module_include_dirs;
    /* LS2 (local-spice-dev-workflow-plan): per-include-dir provenance for
     * the workspace-sibling warning. Each entry is parallel to
     * module_include_dirs[i]:
     *   - module_include_workspace_producer[i]: producer's workspace-member
     *     directory path (e.g. "spices/alpha") when this include dir came
     *     from auto-added workspace siblings; NULL otherwise.
     *   - module_include_warned[i]: set after the first warning is emitted
     *     for this dir so we never re-warn for the same (consumer, producer)
     *     pair within one tur invocation.
     * These arrays may be NULL when no workspace context applies. */
    const char     **module_include_workspace_producer;
    bool            *module_include_warned;
    /* LS2: names this consumer declares in its own :spices map, used to
     * decide whether a workspace-sibling resolution warrants a warning. */
    const char     **module_consumer_declared_spices;
    int              n_module_consumer_declared_spices;
    struct ElabModule *loaded_modules;    /* registry of loaded modules */
    uint32_t         n_loaded_modules;
    uint32_t         cap_loaded_modules;
    uint16_t         next_import_file_id; /* file_id counter for imported source files */
    /* load-not-expanded-in-imported-or-project-modules: compilation-global set
     * of canonical (load "path") paths already spliced+elaborated, shared by the
     * entry preprocessor (elaborate_program) and every imported module's
     * preprocessor (elab_load_module) so a path is expanded exactly once across
     * the whole build -- no duplicate symbols when the entry and an imported
     * module both `(load ...)` the same file. */
    const Symbol   **load_expanded_paths;
    uint32_t         n_load_expanded_paths;
    uint32_t         cap_load_expanded_paths;
    /* Phase M3: Separate compilation — skip inlining imported modules */
    bool             separate_compilation;
    /* load-not-expanded-in-imported-or-project-modules: true while elaborating
     * the forms of a module pulled in via `import` (inside elab_load_module).
     * Under separate compilation an imported module's definitions belong to its
     * own translation unit; registering them for emission in the importer's TU
     * re-emits them (e.g. a typeclass instance's method body referencing the
     * owner's internal ADT) where the supporting typedefs are absent.  The
     * emission-registration sites consult this flag (together with
     * separate_compilation) to skip re-registering imported defs. */
    bool             in_imported_module;
    /* SB2: When true, (import ...) is forbidden (sandboxed environment). */
    bool             sandboxed;
    /* MF3: true while elaborating the auto-loaded stdlib prefix; new global
     * bindings created during this window are marked is_from_stdlib so
     * user code that later shadows them gets a hard diagnostic instead
     * of broken C output. */
    bool             in_stdlib_load;
    /* Phase M4: During macro expansion, the defining module of the currently
     * expanding macro (so private helper macros from that module are visible).
     * Cross-module wrapper-macro bug fix: when an outer macro M (defined in
     * module A) emits a form referencing a stdlib wrapper W and an inner
     * helper H (also in A), and the form expands like
     * `(W (H ...))`, the inner W expansion overwrites macro_expansion_module
     * with W's defining module (NULL for stdlib), hiding H from the inner
     * lookup. We track the *stack* of in-progress expansion modules instead;
     * elab_lookup_macro walks the stack so any module in the active expansion
     * chain contributes its visibility. macro_expansion_module remains the
     * innermost entry for back-compat with sites that read it directly. See
     * docs/reported/cross-module-wrapper-macro-vec-arg-elaborated-as-expression.md. */
    const Symbol    *macro_expansion_module;
    const Symbol   **macro_expansion_stack;
    uint32_t         n_macro_expansion_stack;
    uint32_t         cap_macro_expansion_stack;
    /* Phase B2 CPS-CL7: tracks nesting depth of cloneable-reset for
     * detecting cloneable-shift outside any reset boundary. */
    int              cloneable_reset_depth;
    /* CF7.3: the scope that was active immediately before the current function
     * body's inner scope was pushed (i.e., e->scope just before scope_init in
     * elab_fn/elab_defn).  check_cloneable_capture stops here so bindings
     * from outer function scopes are not falsely flagged as needing Clone. */
    struct Scope    *fn_entry_outer_scope;
    /* Edge 1 (hkt-matcher-cata-...): the binding group of the `letrec`/named-let
     * init currently being lifted.  Set by elab_letrec immediately around each
     * init's elaboration and snapshotted+cleared at elab_fn entry, so the
     * init's OWN top-level fn excludes the group (a direct self/mutual call is
     * recursion, handled by the recursion machinery / env-ptr), while a NESTED
     * closure inside the init sees an empty group and therefore captures the
     * letrec-bound value through its env.  NULL/0 when not in a letrec init. */
    struct Binding **letrec_self_group;
    uint32_t         letrec_self_group_n;
    /* Phase 21: tracks nesting depth of serial-reset for detecting
     * serial-shift outside any serial-reset boundary. */
    int              serial_reset_depth;
    /* Phase EX1d: nesting depth of `open` forms.  Each open mints a fresh
     * skolem id (open_skolem_next++); the depth counter exists so nested
     * opens cannot confuse each other's escape checks. */
    int              open_skolem_depth;
    uint32_t         open_skolem_next;
    /* Phase P3: HAMT lowering - track if HAMT functions are used */
    bool             needs_hamt;
    /* Phase P3: set true while elaborating the RHS of a ^persistent let binding
     * so that map-new (which has no first arg to inspect) can be lowered to
     * hamt/new even though is_persistent_map would otherwise be false. */
    bool             in_persistent_let;
    /* PR5-3-D: Effects brought into scope via :refer [(effect Name)] imports */
    Effect      **referred_effects;
    uint32_t      n_referred_effects;
    uint32_t      cap_referred_effects;
    /* Phase RF0: forward-declared type symbols for mutual recursion support.
     * Names added here during the type pre-pass are allowed to be re-elaborated
     * by defstruct/defdata without triggering "already defined" errors. */
    const Symbol   **forward_type_syms;
    uint32_t         n_forward_type_syms;
    uint32_t         cap_forward_type_syms;
    /* CT0: Contract keyword symbols */
    const Symbol    *kw_pre;                /* :pre */
    const Symbol    *kw_post;               /* :post */
    const Symbol    *sym_result;            /* "result" -- bound name in :post predicates */
    const Symbol    *sym_tur_contract_check; /* tur-contract-check */
    /* SS0b: Session type constructor symbols (used in type annotations) */
    const Symbol    *sym_session_type;      /* "Session" — type constructor */
    const Symbol    *sym_session_Send;      /* "Send"    — protocol Send[T, Q] */
    const Symbol    *sym_session_Recv;      /* "Recv"    — protocol Recv[T, Q] */
    const Symbol    *sym_session_Close;     /* "Close"   — terminal protocol */
    const Symbol    *sym_session_Choose;    /* "Choose"  — internal choice */
    const Symbol    *sym_session_Branch;    /* "Branch"  — external choice */
    const Symbol    *sym_session_Rec;       /* "Rec"     — recursive protocol */
    const Symbol    *sym_session_Timeout;   /* "Timeout" -- timeout protocol (SS3c) */
    /* SS0b: Session channel operation symbols (used in expression position) */
    const Symbol    *sym_close;             /* "close"        */
    const Symbol    *sym_offer;             /* "offer"        */
    const Symbol    *sym_choose_left;       /* "choose-left"  */
    const Symbol    *sym_choose_right;      /* "choose-right" */
    const Symbol    *sym_make_session;      /* "make-session" */
    const Symbol    *sym_recv_timeout;      /* "recv-timeout" -- SS3c */
    /* SS3a: Session Rec label stack -- used during type parsing to resolve
     * bare label references inside (Rec label body) expressions.
     * rec_labels[i] is the interned label symbol; rec_types[i] is the
     * arena-allocated TY_SESSION_REC node being constructed (body filled later). */
#define ELAB_MAX_REC_DEPTH 8
    const Symbol    *rec_labels[ELAB_MAX_REC_DEPTH]; /* active Rec labels */
    struct Type     *rec_types[ELAB_MAX_REC_DEPTH];  /* corresponding TY_SESSION_REC nodes */
    uint8_t          rec_depth;                       /* number of active Rec binders */
    /* SS5: Global protocol symbols */
    const Symbol    *sym_defprotocol;  /* "defprotocol" */
    const Symbol    *sym_make_protocol; /* "make-protocol" */
    const Symbol    *sym_send_to;       /* "send-to" */
    const Symbol    *sym_recv_from;     /* "recv-from" */
    /* SS5: Global protocol type symbols (appear in type annotations) */
    const Symbol    *sym_global_type;   /* "Global" -- type constructor */
    const Symbol    *sym_role_type;     /* "Role"   -- type constructor */
    /* SS6: Projection type annotation */
    const Symbol    *sym_project_type;  /* "project" -- SS6: (project G R) type annotation */
    /* SS5: Global protocol registry */
    GlobalProtocol  *global_protocols;
    uint32_t         n_global_protocols;
    uint32_t         cap_global_protocols;
    /* DV0: Dynamic vars (-Xdynamic-vars) */
    const Symbol    *sym_defdynamic;        /* "defdynamic" */
    const Symbol    *sym_binding;           /* "binding" (DV1: dynvar override form) */
    DynVarEntry    **dynvar_entries;        /* all registered dynamic vars */
    uint32_t         n_dynvars;
    uint32_t         cap_dynvars;
    /* DV1: Active binding frames (compile-time set! scope check) */
    const Symbol   **active_dynvar_bindings; /* dynvar names with a live binding frame */
    uint32_t         n_active_dynvar_bindings;
    uint32_t         cap_active_dynvar_bindings;
    /* GF1: Generator elaboration context */
    struct GenContext *gen_ctx;     /* non-NULL when inside a (gen ...) body */
    uint32_t          gen_counter; /* monotonically increasing, for unique struct names */
    const Symbol     *sym_gen;          /* "gen" */
    const Symbol     *sym_yield;        /* "yield" */
    const Symbol     *sym_gen_next;     /* "gen-next" */
    const Symbol     *sym_gen_done;     /* "gen-done?" */
    /* CF5 (control-flow-completeness-plan): set true while elaborating a match arm body. */
    bool              in_match_arm;
    /* CF6 (control-flow-completeness-plan): set true while elaborating an inline async closure body.
     * Used by elab_await to check that bindings in scope are Send. */
    bool              in_async_body;
    /* bare-fat-result-monomorphization-plan (Phase B): per-call-site
     * specialization of a bare-^fat callee over the incoming closure's result
     * kind.  See elab_specialize_bare_fat (elab_call.c) and elab_defn. */
    bool              bare_fat_spec_active;     /* re-elaborating a clone now */
    bool              bare_fat_force_canonical;  /* sweep: emit the deferred error */
    TypeKind          bare_fat_spec_kind;       /* the bare-^fat param's result kind */
    const Symbol     *bare_fat_spec_name;       /* mangled name for the clone */
    Binding          *bare_fat_spec_result;     /* clone's binding, filled by elab_defn */
    /* Memo of (orig callee, result kind) -> specialized binding.  A NULL spec
     * with matching (orig,kind) marks an in-progress specialization, so a
     * recursive reference is caught instead of looping forever. */
    struct BareFatSpec { Binding *orig; TypeKind kind; Binding *spec; } *bare_fat_specs;
    uint32_t          n_bare_fat_specs;
    uint32_t          cap_bare_fat_specs;
    /* Lazy bare-^fat bindings whose canonical (int) body was deferred; swept
     * after top-level elaboration so a never-specialized one still surfaces its
     * real (deferred) diagnostic instead of silently vanishing. */
    Binding         **bare_fat_lazy_bindings;
    uint32_t          n_bare_fat_lazy_bindings;
    uint32_t          cap_bare_fat_lazy_bindings;
    /* L6 follow-up (strict-row-elements): when non-zero, the unknown-name
     * fallthrough in type_expr_from_form (F_SYM line ~484 and F_KEYWORD
     * line ~1756) emits a hard error instead of returning a NULL-def
     * opaque placeholder. Used inside the #row{...} element resolution
     * loop so a typo'd component name in `#row{Pos Velocityy}` becomes a
     * compile error rather than silently elaborating to opaque. Counter
     * (not bool) so nested rows can save/inc/dec/restore around their
     * element recursion. */
    uint8_t           strict_unknown_types;
    /* defstruct-as-defadt: a `(make-struct Name ...)` on a NON-parametric
     * lowered record ADT rewrites to the auto-bound ctor call `(Name ...)`,
     * which then hits elab_call's strict positional arg typecheck.  At default,
     * make-struct of a non-parametric struct does NO field typecheck (it accepts
     * the value as-is, e.g. `0`-as-NULL for a ptr<void> field, a NULL ptr<void>
     * for an rc<T> field).  Set true around that rewrite's elab_call so the ctor
     * call's OWN arg check relaxes to parity with default make-struct.  elab_call
     * reads-and-clears it at entry, so nested calls during arg elaboration do not
     * inherit the leniency. */
    bool              make_struct_lenient_args;
    /* structdef-retirement slice 2 (CTOR-V0): a `:no-auto-ctor` lowered record ADT
     * keeps its value-namespace constructor binding (make-struct rewrites
     * `(make-struct Name ...)` to the ctor call `(Name ...)` and needs it), but a
     * DIRECT `(Name ...)` call must still be rejected.  make-struct sets this flag
     * around its rewrite's elab_call so the no_auto_ctor guard there lets the
     * rewritten call through; elab_call reads-and-clears it at entry so a nested
     * user `(Name ...)` during arg elaboration is still rejected. */
    bool              make_struct_ctor_rewrite;
} Elab;

/* GF1: per-gen elaboration state (stack-allocated, linked by parent pointer) */
typedef struct GenContext {
    uint32_t          n_yields;         /* number of (yield ...) forms seen so far */
    TypeKind          element_kind;     /* TypeKind of the first yield (TY_UNKNOWN until set) */
    bool              element_kind_set; /* true once first yield is elaborated */
    struct GenContext *parent;          /* enclosing GenContext (NULL for outermost) */
    /* CF5: true when the enclosing function calls itself inside this gen body */
    bool              is_recursive;
    /* Collect let-bindings inside gen body for struct field promotion */
    Binding         **let_bindings;     /* arena-allocated array of Binding* */
    uint32_t          n_let_bindings;
    uint32_t          cap_let_bindings;
} GenContext;

/* Phase 6: Macro definition */
typedef struct MacroDef {
    const Symbol *name;
    Form **params;
    uint32_t n_params;       /* number of fixed params (excludes rest param) */
    bool is_variadic;        /* true if [params & rest] syntax used */
    const Symbol *rest_param; /* rest-arg symbol name, or NULL */
    /* docs/reported/macro-args-elaborated-before-expansion.md:
     * per-param ^syntax marker.  When is_syntax_param[i] is true, the
     * corresponding arg form is passed to the macro as data: substitute_params
     * leaves the param symbol in place and elab_eval_macro_form binds the
     * symbol to the raw arg Form in the CT env, so CT builtins like
     * (first decl) / (symbol-name (first decl)) walk the AST instead of
     * trying to evaluate it as code. NULL when no params are marked. */
    bool *is_syntax_param;
    bool rest_is_syntax;     /* ^syntax marker on the rest param */
    Form *body;
    Span span;
    /* Phase M4: module that defined this macro (NULL = stdlib/pre-module) */
    const Symbol *defining_module_name;
    /* Phase M4: true when injected via :refer — visible in any module context
     * but defining_module_name still holds the original module so private
     * helpers of that module remain accessible during expansion. */
    bool is_referred;
} MacroDef;

typedef enum CtValueTag {
    CT_VAL_FORM = 0,
    CT_VAL_FN,
} CtValueTag;

typedef struct CtEnv CtEnv;

typedef struct CtFn CtFn;

typedef struct CtValue {
    CtValueTag tag;
    union {
        Form *form;
        CtFn *fn;
    } as;
} CtValue;

typedef struct CtBinding {
    const Symbol *name;
    CtValue value;
    struct CtBinding *next;
} CtBinding;

struct CtEnv {
    CtEnv *parent;
    CtBinding *bindings;
    Elab *elab;
    bool *ok;
    /* True only when this env directly evaluates the inner of a `~@` splice.
     * In that position a list whose head names a user macro is expanded at
     * compile time (so a macro can recurse over a list to GENERATE the spliced
     * sequence, e.g. `~@(chain (rest xs))`). Elsewhere a macro-headed call is
     * left as data for ordinary elaboration -- expanding it during CT eval
     * would mis-fire on data forms threaded through builtins like `list`
     * (e.g. an accumulated `(map-assoc ...)`).  Not inherited by child envs.
     * docs/reported/ct-macro-evaluator-no-function-call-in-splice.md */
    bool expand_macro_head;
};

struct CtFn {
    Form **params;
    uint32_t n_params;
    bool is_variadic;
    const Symbol *rest_param;
    Form *body;
    CtEnv *env;
    Span span;
};

/* ---- elaborator function prototypes ---- */

/* elab_core.c */
Type type_from_kind(TypeKind k);
TypeKind typekind_from_symbol(const char *name);
uint32_t fwd_decl_scan_params(const Form *params_f, TypeKind *arg_kinds);
bool typekind_is_numeric(TypeKind k);
int type_size_bytes(TypeKind kind);
bool typekind_is_concrete_for_disjoint(TypeKind k);
bool types_provably_disjoint(Type *a, Type *b);
void scope_init(Scope *s, Scope *parent);
void scope_free(Scope *s);
bool scope_borrow_conflicts(const Scope *s, Binding *binding, BorrowKind kind);
bool scope_add_borrow(Scope *s, Binding *binding, BorrowKind kind, Span span);
void scope_add(Scope *s, Binding *b);
Binding *scope_lookup(Scope *s, const Symbol *name);
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
    Binding **self_exclude, uint32_t n_self_exclude, uint32_t *n_out);
void elab_register_file_def(Elab *e, Expr *def_expr);
/* used-attr-whole-program: force-load a module by name so its defns are
 * emitted on the single-file/whole-program path even with no `(import)`. */
void elab_force_load_module(Elab *e, const char *module_name);
int elab_expand_module_loads(Elab *e, Arena *arena, SymbolTable *st,
                             Form *const *forms, uint32_t nforms,
                             Form ***out_forms, uint32_t *out_n);
const Symbol *intern_cstr(SymbolTable *st, const char *s);
bool binding_mark_moved(Binding *b, Span use_span);
bool binding_check_not_moved(Binding *b, Span use_span, const char *use_desc);
uint32_t move_state_snapshot_bindings(const Scope *scope,
    Binding ***out_bindings,
    bool **out_states);
bool *move_state_capture_current(Binding **bindings, uint32_t n);
void move_state_restore(Binding **bindings, const bool *states, uint32_t n);
uint32_t linear_state_snapshot_bindings(const Scope *scope,
    Binding ***out_bindings,
    bool **out_states);
bool *linear_state_capture_current(Binding **bindings, uint32_t n);
void linear_state_restore(Binding **bindings, const bool *states, uint32_t n);
bool is_binding_consumed(const Expr *body, Binding *binding);
Binding *expr_closure_fn_binding(const Expr *expr);
bool expr_closure_return_dispatches(const Expr *expr);
bool expr_closure_return_dispatches_untyped(const Expr *expr);
void elab_init_state(Elab *e, Arena *arena, SymbolTable *st);
MacroDef *elab_lookup_macro(Elab *e, const Symbol *name);
Binding *binding_new(Elab *e, const Symbol *name, Type type,
    bool is_mut, bool is_global, Span span);
int elab_read_file(const char *path, char **out, size_t *out_len);
ElabModule *elab_find_loaded_module(Elab *e, const Symbol *name);
bool effect_row_contains_symbol(const EffectRow *row, const Symbol *name);
Expr *e_nil(Elab *e, Span span);
char *elab_mangle_binding_name(const Binding *b);

/* elab_macros.c */
void elab_register_macro(Elab *e, MacroDef *macro);
Form *quasiquote_expand_form(Elab *e, Form *f);
Form *elab_expand_macro(Elab *e, MacroDef *macro, Form **args, uint32_t n_args);
Expr *elab_defmacro(Elab *e, const Form *call);
Expr *elab_gensym(Elab *e, const Form *call);

/* TY2.2: wrap a value in EX_UNION_INJECT to widen it to the `any` top type. */
Expr *elab_coerce_to_any(Elab *e, Expr *value);

/* Gap 1 (instance-method-return-not-unified): report a genuine,
 * carrier-independent return-type conflict between a function's DECLARED return
 * and its body's actual type.  The int64 carrier ABI deliberately unifies the
 * representation of int / cstr / bool / opaque-handle / struct-handle, so a
 * width-compatible "mismatch" (return 42 where :cstr is declared) cannot be
 * soundly rejected without fighting the carrier -- those are the bridges the
 * ABI relies on.  What CAN be rejected is a clash of distinct NOMINAL
 * identities: the declared return is a concrete record ADT / opaque newtype
 * (carrying a def pointer) and the body yields a DIFFERENT concrete ADT.  Two
 * distinct nominal defs never share a carrier representation, so this is always
 * a real error.  ret_adt is the declared return's ADT def pointer; body is the
 * elaborated body's type.  Returns true on a conflict; a primitive / carrier /
 * tyvar / applied / unknown body is tolerated.  (structdef-retirement DS-D: the
 * former StructDef ret_struct parameter is gone -- every former struct is a
 * record ADT.) */
bool return_type_nominal_conflict(const AdtDef *ret_adt, Type body);

/* float-register-class-returns: sibling of return_type_nominal_conflict that
 * catches the one carrier-tolerated return mismatch that is NOT a benign
 * representation bridge -- a float-vs-non-float clash.  `int`, `cstr`, `bool`,
 * opaque handles, and struct/ADT handles all share the int64 GP register, so
 * swapping them in the result position is a no-op reinterpret; a float lives in
 * an xmm register, so returning a float where the declared return is a concrete
 * non-float (or vice versa) is a genuine register-class miscompile (xmm0 vs
 * rax).  Fires only when EXACTLY ONE side is a floating kind (TY_FLOAT*) and the
 * other is a concrete (register-pinned) non-float; tyvar / unknown / never / any
 * sides are tolerated (not yet a concrete cross-class result).  `declared` is
 * the function's declared return TypeKind; `body` is the elaborated body's type.
 * The int-literal -> float coercion is handled by the caller (it widens the
 * literal in place) before this check runs. */
bool return_type_register_class_conflict(TypeKind declared, Type body);

/* float-register-class-returns: if `declared` is a floating kind (TY_FLOAT*)
 * and `body` is a bare integer literal, widen the literal to that float in
 * place (mirroring numeric-literal coercion in argument / binding positions)
 * and return true.  The caller then treats the return as a coercion rather
 * than a register-class conflict.  Returns false (and leaves `body` untouched)
 * otherwise. */
bool rc_widen_int_literal_to_float_return(TypeKind declared, Expr *body);

/* pointer-vs-scalar-returns: the next carrier-tolerated slice past the nominal
 * and float guards.  `cstr` (a `const char*`) rides the same int64 GP register
 * as `int` / `bool`, so swapping them in the result position is a no-op
 * reinterpret the carrier ABI cannot see -- yet a bare integer is never a valid
 * string pointer in surface Turmeric.  Only the COMMIT direction is a sound
 * rejection: the declared return is concretely `cstr` and the body yields a
 * concrete integer-family scalar (int / bool / intN / uintN).  The reverse
 * (a declared integer carrier with a `cstr` body) is the deliberate
 * carrier-handle bridge -- generic / typeclass code legitimately returns a
 * pointer handle through an int64 result slot, exactly as it returns a struct
 * handle through `int` -- so it is left to a future carrier-aware unification,
 * like the same-register int-vs-bool case.  `declared` is the function's
 * declared return TypeKind; `body` is the elaborated body's type.  Returns true
 * only on the commit-direction conflict. */
bool return_type_pointer_scalar_conflict(TypeKind declared, Type body);

/* carrier-aware-return-unification Phase 2: the REVERSE pointer-scalar shape --
 * a concrete integer-family declared return with a concrete `cstr` body.  This
 * is the carrier-handle bridge the commit-direction helper tolerates, so it is
 * sound to reject ONLY for a committed position; the dispatcher applies that
 * gate.  Returns true on the shape match. */
bool return_type_pointer_scalar_reverse_conflict(TypeKind declared, Type body);

/* carrier-aware-return-unification Phase 2b: a `bool`-vs-non-bool-integer
 * mismatch -- exactly one of declared / body is `bool` and the other is a
 * concrete non-bool integer-family scalar.  `bool` and the integer family share
 * the int64 0/1 carrier, but the language treats them as distinct (binding
 * position already rejects the swap), so this is sound to reject ONLY for a
 * committed position; the dispatcher applies that gate.  Returns true on the
 * shape match. */
bool return_type_bool_integer_conflict(TypeKind declared, Type body);

/* carrier-aware-return-unification: classify a return position so the shared
 * dispatcher knows how much to reject against the int64 carrier ABI.
 *   RET_CLASS_COMMITTED -- a genuinely committed position: a monomorphic,
 *     non-`#{Unsafe}` `defn` that does not participate in the carrier.  Rejects
 *     every concrete ground mismatch: symmetric register-class, the reverse
 *     pointer-scalar direction (integer return, cstr body -> TUR-E0709), AND the
 *     bool-vs-integer mismatch (both directions -> TUR-E0709).
 *   RET_CLASS_CARRIER_FN -- a generic or `#{Unsafe}` `defn`.  Register-class
 *     stays symmetric (a float never rides the int64 carrier), but the reverse
 *     pointer-scalar direction is the deliberate carrier-handle bridge and is
 *     tolerated.
 *   RET_CLASS_CARRIER_METHOD -- a typeclass instance method.  Full carrier
 *     tolerance: register-class only in the float-COMMIT direction (a
 *     non-float-declared method with a float instance body is the per-instance
 *     bridge) and the reverse pointer-scalar direction tolerated.
 * Phase 3 will route grounded instance methods to RET_CLASS_COMMITTED. */
typedef enum {
    RET_CLASS_COMMITTED,
    RET_CLASS_CARRIER_FN,
    RET_CLASS_CARRIER_METHOD,
} ReturnClass;

/* Which return-position predicate fired (RET_CONFLICT_NONE = no conflict).  The
 * caller maps each to its site-specific diagnostic (function vs instance
 * method) and error code (NOMINAL -> TUR-E0001, REGISTER_CLASS -> TUR-E0707,
 * POINTER_SCALAR -> TUR-E0708, TYPE_REVERSE / BOOL_INTEGER -> TUR-E0709). */
typedef enum {
    RET_CONFLICT_NONE = 0,
    RET_CONFLICT_NOMINAL,
    RET_CONFLICT_REGISTER_CLASS,
    RET_CONFLICT_POINTER_SCALAR,
    RET_CONFLICT_TYPE_REVERSE,
    RET_CONFLICT_BOOL_INTEGER,
} ReturnConflict;

/* carrier-aware-return-unification: single dispatcher over the return-position
 * predicates above.  Runs nominal -> register-class -> pointer-scalar (commit)
 * -> pointer-scalar (reverse) and returns the first conflict; `cls` calibrates
 * the register-class and reverse-pointer-scalar axes (see ReturnClass).  Callers
 * widen an int-literal -> float body in place with
 * rc_widen_int_literal_to_float_return BEFORE calling, and should skip the
 * lazy-probe placeholder and inline-C (fiat TY_NIL) bodies as before. */
ReturnConflict return_position_conflict(const AdtDef *ret_adt,
                                        TypeKind ret_kind, Type body,
                                        ReturnClass cls);

/* TY4: if `e` is a borrow (&x / &mut x) of a named binding, return that
 * binding (the referent); otherwise NULL.  Used by the borrow-escape check. */
const Binding *borrow_referent_binding(const Expr *e);

/* elab_unsafe.c */
Expr *elab_ptr_deref(Elab *e, const Form *call);
Expr *elab_ptr_write(Elab *e, const Form *call);
Expr *elab_ptr_add(Elab *e, const Form *call);
Expr *elab_ptr_sub(Elab *e, const Form *call);
Expr *elab_ptr_nullq(Elab *e, const Form *call);
Expr *elab_ptr_of(Elab *e, const Form *call);
Expr *elab_unsafe_cast(Elab *e, const Form *call);
Expr *elab_reinterpret(Elab *e, const Form *call);
Expr *elab_transmute(Elab *e, const Form *call);
Expr *elab_array_get_unchecked(Elab *e, const Form *call);
Expr *elab_array_set_unchecked(Elab *e, const Form *call);
Expr *elab_raw_malloc(Elab *e, const Form *call);
Expr *elab_raw_free(Elab *e, const Form *call);
Expr *elab_raw_realloc(Elab *e, const Form *call);
Expr *elab_raw_memcpy(Elab *e, const Form *call);
Expr *elab_raw_memset(Elab *e, const Form *call);
Expr *elab_c_call(Elab *e, const Form *call);
Expr *elab_dlopen(Elab *e, const Form *call);
Expr *elab_dlsym(Elab *e, const Form *call);
Expr *elab_dlclose(Elab *e, const Form *call);
Expr *elab_unsafe(Elab *e, const Form *call);

/* elab_forms.c */
Form *splice_internal_defines(Elab *e, Form **items, uint32_t n, Span span);
Expr *elab_define_error(Elab *e, const Form *call);
Expr *elab_let(Elab *e, const Form *call);
Expr *elab_letstar(Elab *e, const Form *call);
Expr *elab_letrec(Elab *e, const Form *call);
Expr *elab_named_let(Elab *e, const Form *call);
Expr *elab_do(Elab *e, const Form *call);
Expr *elab_if(Elab *e, const Form *call);
Expr *elab_thread(Elab *e, const Form *call);
Expr *elab_thread_last(Elab *e, const Form *call);
Expr *elab_set(Elab *e, const Form *call);
Expr *elab_while(Elab *e, const Form *call);
Expr *elab_case(Elab *e, const Form *call);
Expr *elab_defer(Elab *e, const Form *call);
Expr *elab_return(Elab *e, const Form *call);
Expr *elab_question(Elab *e, const Form *call);

/* GF1: Generator forms */
Expr *elab_gen(Elab *e, const Form *call);
Expr *elab_yield(Elab *e, const Form *call);
Expr *elab_gen_next(Elab *e, const Form *call);
Expr *elab_gen_done(Elab *e, const Form *call);

/* elab_memory.c */
Expr *elab_ref(Elab *e, const Form *call);
Expr *elab_lref_new(Elab *e, const Form *call);
Expr *elab_deref(Elab *e, const Form *call);
Expr *elab_drop(Elab *e, const Form *call);
Expr *elab_rc_of(Elab *e, const Form *call);
Expr *elab_rc_clone(Elab *e, const Form *call);
Expr *elab_rc_drop(Elab *e, const Form *call);
Expr *elab_rc_ptr(Elab *e, const Form *call);
Expr *elab_rc_strong_count(Elab *e, const Form *call);
Expr *elab_rc_from_ref(Elab *e, const Form *call);
Expr *elab_ref_from_rc(Elab *e, const Form *call);
Expr *elab_weak(Elab *e, const Form *call);
Expr *elab_weak_upgrade(Elab *e, const Form *call);
Expr *elab_weak_pred(Elab *e, const Form *call);
Expr *elab_ref_pred(Elab *e, const Form *call);
Expr *elab_gc_force(Elab *e, const Form *call);
Expr *elab_gc_enable(Elab *e, const Form *call);
Expr *elab_gc_disable(Elab *e, const Form *call);

/* elab_effects.c */
Expr *elab_reset(Elab *e, const Form *call);
Expr *elab_shift(Elab *e, const Form *call);
Expr *elab_shift0(Elab *e, const Form *call);
Expr *elab_cloneable_reset(Elab *e, const Form *call);
Expr *elab_cloneable_shift(Elab *e, const Form *call);
Expr *elab_call_cc_star(Elab *e, const Form *call);
Expr *elab_serial_reset(Elab *e, const Form *call);
Expr *elab_serial_shift(Elab *e, const Form *call);
Expr *elab_try_with(Elab *e, const Form *call);
Expr *elab_defeffect(Elab *e, const Form *call);
Expr *elab_perform(Elab *e, const Form *call);
Expr *elab_handle(Elab *e, const Form *call);
/* FH2-FH5: first-class handler values */
Expr *elab_handler_lit(Elab *e, const Form *call);
Expr *elab_with_handler(Elab *e, const Form *call);
Expr *elab_compose_handlers(Elab *e, const Form *call);
bool cont_check_double_use(Elab *e, const Form *k_form);
Expr *elab_resume(Elab *e, const Form *call);
Expr *elab_discontinue(Elab *e, const Form *call);
Expr *elab_cont_pred(Elab *e, const Form *call);
Expr *elab_call_cc(Elab *e, const Form *call);
Expr *elab_escape(Elab *e, const Form *call);

/* elab_fns.c */
Expr *elab_defn(Elab *e, const Form *call);
Expr *elab_fn(Elab *e, const Form *call);
/* bare-fat-param-non-int-result inference (Phase A); see
 * docs/upcoming/bare-fat-result-type-inference-plan.md. */
bool kind_is_non_int_register_class(TypeKind k);

/* bare-fat-result-monomorphization (Phase B); see
 * docs/upcoming/bare-fat-result-monomorphization-plan.md.
 *  - elab_specialize_bare_fat: re-elaborate `callee`'s retained body with its
 *    bare-^fat param result kind set to `k`, returning the clone's binding
 *    (memoized by (callee, k)); NULL if it cannot be specialized.
 *  - elab_track_bare_fat_lazy: register a deferred-canonical binding for the
 *    end-of-pass sweep.
 *  - elab_sweep_bare_fat_lazy: surface the deferred diagnostic for any lazy
 *    binding no call site specialized. */
Binding *elab_specialize_bare_fat(Elab *e, Binding *callee, TypeKind k);
void     elab_track_bare_fat_lazy(Elab *e, Binding *b);
void     elab_sweep_bare_fat_lazy(Elab *e);
bool retype_bare_fat_tail_calls(Expr *tail, TypeKind target);
Expr *elab_extern_c(Elab *e, const Form *call);
Expr *elab_def(Elab *e, const Form *call);

/* elab_call.c */
Expr *elab_call(Elab *e, Form *call);
Binding *make_poly_wrapper(Elab *e, Binding *inner_b, uint8_t inner_arity, Span span, bool typed_concrete);
Binding *poly_arg_fn_binding(Expr *arg);

/* elab_module.c */
Expr *elab_load(Elab *e, const Form *call);
Expr *elab_defmodule(Elab *e, const Form *call);
Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error);

/* elab_structs.c */
void elab_add_forward_type(Elab *e, const Symbol *sym);
Expr *elab_defstruct(Elab *e, const Form *call);
Expr *elab_defopaque(Elab *e, const Form *call);
void elab_register_adt_def(Elab *e, AdtDef *def);
Expr *elab_defdata(Elab *e, const Form *call);
/* CONV-S1 (defstruct-as-defadt): true iff this defstruct form qualifies for the
 * slice-1 lowering to a single-variant record defadt.  Shared by the top-level
 * type pre-pass and elab_defstruct. */
bool defstruct_lowers_to_adt(Elab *e, const Form *call);
/* TP6: unpack a TY_APP chain on an ADT type to recover concrete type arguments
 * (into out_args, sized def->n_type_params); true on a fully-applied match.
 * adt_field_instantiate_type substitutes those args for the TY_TYVAR names in a
 * field type.  Shared with the dot-accessor field-access path. */
bool elab_adt_type_extract_args(const Type *t, const AdtDef *def, Type *out_args);
Type adt_field_instantiate_type(Elab *e, const AdtDef *def, const Type *t,
                                const Type *type_args);
/* Grounds a record-ADT ctor's type params (named in `tps`) by unifying each
 * declared field full_type against the supplied value's actual type, descending
 * into TY_APP/TY_FN fields.  Inference only -- a concrete field never fails. */
bool adt_field_collect_type_args(const char **tps, uint8_t n_tps,
                                 const Type *expected, Type actual,
                                 Type *type_args, bool *have_type_args);
TypeKind gadt_skolem_lookup(const SkolemEnv *env, const char *name);
Expr *elab_defgadt(Elab *e, const Form *call);
Expr *elab_coerce(Elab *e, const Form *call);
CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name);
Expr *elab_match(Elab *e, const Form *call);
Expr *elab_make_struct(Elab *e, const Form *call);
Expr *elab_with(Elab *e, const Form *call); /* WITH-V0 */
Expr *elab_default_of(Elab *e, const Form *call);  /* M2b */
Expr *elab_borrow_immut(Elab *e, const Form *call);
Expr *elab_borrow_mut(Elab *e, const Form *call);

/* elab_types.c */
Expr *elab_defkind(Elab *e, const Form *call);
Expr *elab_defrec(Elab *e, const Form *call);
Expr *elab_defalias(Elab *e, const Form *call);
Type *type_expr_from_form(Elab *e, const Form *form, const Symbol *rec_name,
    const Symbol **type_params, Kind *type_param_kinds,
    uint8_t n_type_params);
/* ptr-generic-parameterised-type: resolve a "ptr<T>" type-name string to a
 * typed raw pointer (TY_PTR_VOID with non-NULL pointee).  Returns NULL when
 * the name is not a typed "ptr<...>" form (e.g. "ptr<void>" or a non-ptr
 * name), so callers can fall through to their existing handling. */
Type *ptr_type_from_keyword_name(Elab *e, const char *name, uint32_t len,
    Span span, const Symbol *rec_name, const Symbol **type_params,
    Kind *type_param_kinds, uint8_t n_type_params);
/* Resolve a type-annotation form (F_KEYWORD `:int`, spaced F_TYPE_ANN
 * wrapping any form, or a list constructor like `(-> a b)`) into a Type*.
 * Unwraps F_TYPE_ANN and delegates to type_expr_from_form for the inner
 * form, with arrow / borrow / handler / session / role heads routed
 * through their constructor paths.  Used by defn, fn, defstruct, and
 * let-bindings to share one type-form parser. */
Type *fn_type_from_form(Elab *e, const Form *form,
                        const Symbol **type_params,
                        Kind *type_param_kinds,
                        uint8_t n_type_params);
/* MF4: separate struct / GADT namespaces. Resolves a type name by walking
 * the ADT registry first, then the struct registry. Returns an
 * arena-allocated Type* (TY_ADT or TY_STRUCT) when found, else NULL.
 * Prefers GADTs over structs when both share a name, per the MF4 design. */
Type *elab_lookup_type_by_name(Elab *e, const Symbol *name);
/* CTOR-V0: scope-aware lookup that returns the nearest TY_STRUCT/TY_ADT binding
 * for `name` (skipping shadowing value bindings of other kinds), or NULL. */
Binding *scope_lookup_type_def(struct Scope *s, const Symbol *name);
/* Variadic HKT rows: validate a type-application argument's kind against the
 * constructor's positional parameter kind (row concern only). arg_index is
 * 0-based. Returns false and emits TUR-E0012 on a row/non-row mismatch. */
bool check_row_type_arg_kind(Type ctor_type, uint8_t arg_index, Type arg_type,
                             Span arg_span);
Expr *elab_deftype(Elab *e, const Form *call);
Expr *elab_type_app(Elab *e, const Form *call);
Expr *elab_ascribe(Elab *e, const Form *call);
Expr *elab_pack(Elab *e, const Form *call);
Expr *elab_open(Elab *e, const Form *call);

/* elab_typeclasses.c */
Expr *elab_defclass(Elab *e, const Form *call);
Expr *elab_definstance(Elab *e, const Form *call);
Expr *elab_method_call(Elab *e, const Form *call);
/* Phase RT: if `name` is a typeclass method whose dispatch type variable
 * appears only in the return type (a return-only-dispatch method, e.g.
 * `default-of [] : a`), resolve the instance from the expected-type channel
 * (e->expected_type) and emit a direct call to that instance's impl.  Sets
 * *handled to true when `name` matched such a method (even if resolution
 * failed and a diagnostic was emitted), so the caller does not fall through to
 * the ordinary call path.  Returns NULL when not handled or on error. */
Expr *elab_try_return_dispatch(Elab *e, const Form *call, const Symbol *name,
                               bool *handled);

/* elab_concurrent.c */
Expr *elab_thread_spawn(Elab *e, const Form *call);
Expr *elab_async(Elab *e, const Form *call);
Expr *elab_await(Elab *e, const Form *call);
Expr *elab_select(Elab *e, const Form *call);
Expr *elab_panic(Elab *e, const Form *call);
Expr *elab_panic_with(Elab *e, const Form *call);
Expr *elab_catch_unwind(Elab *e, const Form *call);
Expr *elab_catch_panic_of(Elab *e, const Form *call);
Expr *elab_panic_payload_type(Elab *e, const Form *call);
Expr *elab_panic_payload_value(Elab *e, const Form *call);
Expr *elab_panic_payload_file(Elab *e, const Form *call);
Expr *elab_panic_payload_line(Elab *e, const Form *call);
Expr *elab_panic_payload_downcast(Elab *e, const Form *call);
Expr *elab_stm(Elab *e, const Form *call);
Expr *elab_atomically(Elab *e, const Form *call);
Expr *elab_retry(Elab *e, const Form *call);
Expr *elab_check(Elab *e, const Form *call);
Expr *elab_or_else(Elab *e, const Form *call);
Expr *elab_tvar_new(Elab *e, const Form *call);
Expr *elab_tvar_read(Elab *e, const Form *call);
Expr *elab_tvar_write(Elab *e, const Form *call);
Expr *elab_tvar_modify(Elab *e, const Form *call);
Expr *elab_tvar_swap(Elab *e, const Form *call);
Expr *elab_tvar_cas(Elab *e, const Form *call);

/* elab_toplevel.c */
Expr *elab_as_cast(Elab *e, const Form *call);
Expr *elab_any_type_of(Elab *e, const Form *call);
Expr *elab_any_cast(Elab *e, const Form *call);
Expr *elab_is_q(Elab *e, const Form *call);
Expr *elab_form(Elab *e, Form *f);

/* elab_global.c -- SS5: multi-party global protocol types */
Expr *elab_defprotocol(Elab *e, const Form *call);
Expr *elab_make_protocol(Elab *e, const Form *call);
Expr *elab_send_to(Elab *e, const Form *call);
Expr *elab_recv_from(Elab *e, const Form *call);

/* project.c -- SS6: projection algorithm */
Type *session_project(Elab *e, GlobalInteraction *step, const char *role, Span span);

/* elab_dynvars.c */
Expr       *elab_defdynamic(Elab *e, const Form *call);
Expr       *elab_binding(Elab *e, const Form *call);
DynVarEntry *dynvar_lookup(const Elab *e, const Symbol *name);

/* elab_effects.c -- fx-row-syntax-rename-plan Phase 2 deprecation warner */
void warn_legacy_fx_row(Form *f);

/* elab_sessions.c */
Expr *elab_session_make(Elab *e, const Form *call);
Expr *elab_session_send(Elab *e, const Form *call);
Expr *elab_session_recv(Elab *e, const Form *call);
Expr *elab_session_close(Elab *e, const Form *call);
Expr *elab_session_offer(Elab *e, const Form *call);
Expr *elab_session_choose_left(Elab *e, const Form *call);
Expr *elab_session_choose_right(Elab *e, const Form *call);
Expr *elab_session_recv_timeout(Elab *e, const Form *call);

#endif
