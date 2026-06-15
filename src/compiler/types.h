#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include "buf.h"
#include "lifetimes.h"  /* Phase 13: Lifetime annotations */
#include "forms.h"      /* Phase HKT-P1: for Span */

/* Forward declarations for typeclasses (Phase 15) */
typedef struct TypeClass TypeClass;
typedef struct TypeClassInstance TypeClassInstance;

/* Forward declaration for effect rows (Phase 19) */
struct EffectRow;

/* Phase 11 / UT0: Ownership/copy traits */
typedef enum CopyKind {
    CK_UNIQUE,    /* Unique: at most one live reference (affine; replaces CK_MOVE) */
    CK_COPY,      /* Copy: bitwise duplication allowed */
    CK_UNSIZED,   /* Unsized: size unknown at compile time (e.g., slices) */
    CK_LINEAR,    /* Linear: must be used exactly once (LT0) */
    CK_MULTISHOT, /* MS1: multi-shot continuation; may be resumed any number of times */
} CopyKind;

/* c-fn-ptr-element-and-size-precision-gap fix: optional precise C spelling for
 * an integer carrier used in a (c-fn ...) FFI signature.  CNUM_DEFAULT means
 * "spell by TypeKind" (int64_t / uint64_t / ...).  The other variants make
 * type_c_name emit the precise C name so a typed c-fn parameter matches an
 * external C callback's `size_t` / `ptrdiff_t` slot exactly.  This is a
 * pure C-spelling hint: the runtime carrier stays the underlying TypeKind
 * (TY_UINT64 / TY_INT64), so a :usize is ABI-identical to a :uint64 and is
 * erased to the carrier everywhere except the precise cfnptr typedef path. */
typedef enum CNumSpelling {
    CNUM_DEFAULT = 0, /* spell by TypeKind */
    CNUM_SIZE_T,      /* size_t   (carrier TY_UINT64) */
    CNUM_PTRDIFF_T,   /* ptrdiff_t (carrier TY_INT64) */
} CNumSpelling;
/* UT0: backward-compat alias — all former CK_MOVE sites now mean CK_UNIQUE */
#define CK_MOVE CK_UNIQUE

/* ST0: Substructural type discipline.
 * Controls which structural rules (weakening, contraction) apply to a value.
 *   SK_STRUCTURAL -- default: weakening + contraction both allowed
 *   SK_AFFINE     -- no contraction: can be discarded, cannot be duplicated
 *   SK_RELEVANT   -- no weakening: must be used, can be duplicated
 *   SK_LINEAR     -- no weakening, no contraction: use exactly once
 * SK_STRUCTURAL == 0 so zero-initialised Types are structural by default. */
typedef enum SubstructKind {
    SK_STRUCTURAL = 0,  /* Default: weakening + contraction both allowed */
    SK_AFFINE,          /* No contraction: can discard, cannot duplicate */
    SK_RELEVANT,        /* No weakening: must use, can duplicate */
    SK_LINEAR,          /* No weakening, no contraction: use exactly once */
} SubstructKind;
/* Phase TP3: Arbitrary-arity kind representation.
 * Kind is an integer-backed type; the arity-5 cap is gone.
 *
 * Encoding:
 *   0       -- KIND_STAR (* -- a concrete, fully-applied type)
 *   1..N    -- arity-N arrow kind; value equals arity
 *              (KIND_ARROW=1 means * -> *, KIND_ARROW2=2 means * -> * -> *, ...)
 *   0xFFFE  -- KIND_TYPEROW (kind-level `List Type`; a row of types, e.g.
 *              the `[Pos Vel]` component row of an ECS Query -- see
 *              docs/reported/variadic-hkt-rows-missing.md). A sentinel, not
 *              an arrow kind: a row is a first-class kind, not a constructor
 *              you apply, so kind_apply_one is the identity on it.
 *   0xFFFF  -- KIND_ROW (effect row variable; sentinel, not an arrow kind)
 *
 * kind_for_arity(n) == (Kind)n for all n.
 * kind_apply_one(k) == k-1 for k in 1..0xFFFD, identity for STAR, TYPEROW, ROW.
 * The hkt_kind field on Type uses this encoding; sizeof(Kind)==2 verified below. */
typedef uint16_t Kind;
static_assert(sizeof(Kind) == 2, "Kind must be exactly 2 bytes");
#define KIND_STAR   ((Kind)0)       /* * -- concrete type, e.g. int, bool, vec<int> */
#define KIND_ARROW  ((Kind)1)       /* * -> * -- unary constructor, e.g. vec, option */
#define KIND_ARROW2 ((Kind)2)       /* * -> * -> * -- binary constructor, e.g. result */
#define KIND_ARROW3 ((Kind)3)       /* * -> * -> * -> * -- ternary, e.g. Tuple3 */
#define KIND_ARROW4 ((Kind)4)       /* 4-ary, e.g. Tuple4 */
#define KIND_ARROW5 ((Kind)5)       /* 5-ary, e.g. Tuple5 */
#define KIND_TYPEROW ((Kind)0xFFFE) /* [*] -- kind-level list of types (Row :: List Type) */
#define KIND_ROW    ((Kind)0xFFFF)  /* Row -- effect row variable */
/* Phase 13: Lifetime annotations */
/* Lifetimes are purely an elaborator construct - no runtime representation */

/* Phase 2 type system: function types are stored inline without recursion
 * by using a simple array of TypeKind values. Compound types (structs, 
 * generics) get added in later phases. */
typedef enum TypeKind {
    TY_UNKNOWN = 0,   /* not-yet-resolved (elaboration in progress) */
    TY_NIL,           /* unit / void; (do) with no body, (println …) result */
    TY_BOOL,
    TY_INT,           /* int64_t — default signed integer; alias for int64 */
    TY_FLOAT,         /* double — default float; alias for float64 */
    TY_CSTR,          /* const char* — string literal type for now */
    TY_PTR_VOID,      /* void* — for extern-c and raw pointers */
    TY_FN,            /* function type — requires checking as.fn */
    /* Phase 5: ref<T> — owning pointer with move semantics */
    TY_REF,           /* ref<T> — owning handle, auto-defer drop at scope end */
    /* Phase 9: rc<T> + weak<T> — reference-counted shared ownership */
    TY_RC,            /* rc<T> — reference-counted owning pointer */
    TY_WEAK,          /* weak<T> — non-owning observer for rc<T> */
    /* Phase 12: Borrow traits */
    TY_REF_IMMUT,    /* &T — immutable borrow (non-owning reference) */
    TY_REF_MUT,       /* &mut T — mutable borrow (exclusive non-owning reference) */
    /* Phase 15: Typeclasses */
    TY_TYPECLASS,    /* Typeclass type (e.g., Eq, Ord) */
    TY_TYPECLASS_INST, /* Typeclass instance type */
    /* Phase 17: Exceptions */
    TY_EXCEPTION,    /* Exception type - wraps any value for throw/catch */
    /* Phase 18: Delimited continuations */
    TY_CONT,         /* cont<T> - captured continuation that returns T */
    /* Phase B2: Cloneable continuations */
    TY_CLONEABLE_CONT, /* cloneable_cont<T> - multi-shot continuation that returns T */
    /* Phase 11: User-defined struct types */
    TY_STRUCT,       /* user-defined struct type - see as.struct_ for StructDef */
    /* Phase G0: User-defined sum types (ADTs) */
    TY_ADT,          /* user-defined sum type (ADT) — see as.adt_ for AdtDef */
    /* Phase R2: Panic - diverging/never type */
    TY_NEVER,        /* ! - diverging type (never returns; bottom type) */
    /* Phase HKT-P1: Type application (partially-applied type constructor) */
    TY_APP,          /* (type-app F A) — apply type constructor F to argument A */
    /* Phase HKT-P2: Recursive types */
    TY_REC,          /* (defrec Name [params] body) — recursive type binder */
    /* Phase N: fixed-width numeric types */
    TY_INT8,         /* int8_t */
    TY_INT16,        /* int16_t */
    TY_INT32,        /* int32_t */
    TY_INT64,        /* int64_t (alias for TY_INT) */
    TY_UINT8,        /* uint8_t */
    TY_UINT16,       /* uint16_t */
    TY_UINT32,       /* uint32_t */
    TY_UINT64,       /* uint64_t */
    TY_FLOAT32,      /* float */
    TY_FLOAT64,      /* double (alias for TY_FLOAT) */
    /* Phase X3: Set literal type — sorted int64_t array at runtime */
    TY_SET,          /* set — tur_set_t * */
    /* Phase HRT0: Higher-ranked types */
    TY_FORALL,       /* (forall [a ...] T) — universally quantified type (rank-2+) */
    TY_EXISTS,       /* (exists [a ...] T) — existentially quantified type */
    /* Phase G2: unresolved GADT type variable (skolem that escaped its arm scope) */
    TY_TYVAR,        /* free type variable — field whose type is a GADT type param not yet known */
    /* LT3: lref<T> — linear owning pointer; must be consumed exactly once */
    TY_LREF,         /* lref<T> — CK_LINEAR; silent drop is an error */
    /* IT0: Union types (-Xunion-types) */
    TY_UNION,        /* (A | B | C) — anonymous closed union type */
    /* IT2: Intersection types (-Xintersection-types) */
    TY_INTERSECTION, /* (A & B & C) — anonymous closed intersection type */
    /* IT4: Top type (gradual typing) — available with -Xunion-types or -Xintersection-types */
    TY_ANY,          /* any — top type; every type is a subtype of any */
    /* ET3: Handler type — a handler for a named algebraic effect */
    TY_HANDLER,      /* handler<Effect, ValueType, ResultType> */
    /* CT0: Contract type — { x : T | p }; a refinement-checked wrapper around T */
    TY_CONTRACT,     /* { var : base_type | predicate } */
    /* SS0a: Session type protocol constructors (-Xsessions) */
    TY_SESSION,      /* Session[P] -- linear channel endpoint carrying protocol P */
    TY_SEND,         /* Send[T, Q] -- send T then continue as Q */
    TY_RECV,         /* Recv[T, Q] -- receive T then continue as Q */
    TY_CLOSE,        /* Close -- protocol complete; must be consumed by close */
    TY_CHOOSE,       /* Choose[P, Q] -- internal choice (this endpoint picks branch) */
    TY_BRANCH,       /* Branch[P, Q] -- external choice (peer picks; this endpoint selects) */
    TY_SESSION_REC,  /* Rec[label, F] -- recursive protocol mu-type (distinct from HKT-P2 TY_REC) */
    TY_TIMEOUT,      /* SS3c: Timeout[Q, P] -- success continuation Q; timeout continuation P */
    TY_SESSION_PAIR, /* SS1: internal pair [Session[P], Session[dual(P)]] returned by make-session */
    TY_SESSION_RECV_PAIR, /* SS2: internal pair [T, Session[Q]] returned by recv */
    TY_SESSION_OFFER,     /* SS2: internal result of offer; matched with Left/Right patterns */
    /* SS5: Multi-party global protocol types (-Xsessions) */
    TY_GLOBAL,            /* Global[...] -- a multi-party global protocol (compile-time only) */
    TY_ROLE,              /* Role[G, R]  -- an endpoint of protocol G playing role R */
    /* DV0: Dynamic var reference type (-Xdynamic-vars) */
    TY_DYNVAR,       /* dynvar<T> -- internal marker for a dynamic var binding; wraps declared value type T.
                        Only held in DynVarEntry during elaboration; never appears as the type of a user expression. */
    /* GF1: Generator type */
    TY_GENERATOR,    /* generator<T> -- heap pointer to C state-machine struct; _next returns void* */
    /* SYM0 (runtime-symbols-plan): first-class interned symbol type (-Xsymbols).
     * A :Sym value is a non-null pointer to a static `struct __tur_sym` record.
     * Two keywords with the same name are pointer-identical; eq is `==` and
     * hashing reads a precomputed field. */
    TY_SYM,          /* :Sym -- interned runtime symbol (const struct __tur_sym *) */
    /* Variadic HKT rows (docs/reported/variadic-hkt-rows-missing.md, Layer 2):
     * a compile-time-only *row of types* -- an ordered list of element types,
     * surface-spelled `#row{T1 T2 ...}`.  hkt_kind == KIND_TYPEROW.  Used as a
     * type argument to a row-parameterised constructor (e.g. an ECS Query over
     * `#row{Pos Vel}`); never the type of a runtime value, so it erases at
     * codegen like TY_TYPECLASS / TY_GLOBAL. */
    TY_TYPEROW,
} TypeKind;

/* SS5: Global protocol interaction tree (compile-time only, arena-allocated).
 * Forward-declared here so Type can reference them; defined in elab_internal.h. */
typedef struct GlobalInteraction GlobalInteraction;
typedef struct GlobalBranch GlobalBranch;

/* Phase G0: Constructor field descriptor for ADTs */
typedef struct CtorField {
    TypeKind kind;          /* field type kind */
    TypeKind inner_kind;    /* for ref/rc: inner type; TY_UNKNOWN otherwise */
    /* TP1: non-NULL when the field was declared as a type variable (e.g. `a` in
     * `(defdata Opt2 [a] (Yep a))`).  Carries a TY_TYVAR type node so that
     * tooling / future type-inference phases can inspect the parameter name.
     * The runtime representation is always TY_INT (int64_t carrier). */
    struct Type *full_type;
} CtorField;

/* Phase SZ6: Type-level size index term.
 * A `SizeTerm` is a compile-time natural-number expression over the `Size`
 * GADT: a constant (`Static n`), a size variable (a GADT type-parameter name),
 * or an `Add`/`Mul` of two sub-terms.  Size indices are *erased* in codegen
 * (zero runtime cost); they live only in the elaborator, where SZ7 compares
 * them for static size checking.  Arena-allocated. */
typedef enum SizeTermKind {
    SZT_CONST,  /* (Static n) — a literal natural number */
    SZT_VAR,    /* a size variable, e.g. `n` in (SizedVec (Add (Static 1) n) a) */
    SZT_ADD,    /* (Add a b) */
    SZT_MUL,    /* (Mul a b) */
} SizeTermKind;

typedef struct SizeTerm {
    SizeTermKind kind;
    int64_t      konst;            /* SZT_CONST */
    const char  *var;              /* SZT_VAR (interned/borrowed name) */
    struct SizeTerm *lhs, *rhs;    /* SZT_ADD / SZT_MUL */
} SizeTerm;

/* Phase G2: Skolem equality binding — one entry per GADT type parameter per arm */
#define MAX_SKOLEM_BINDINGS 8
typedef struct SkolemBinding {
    const char *name;      /* type parameter name (e.g. "a") */
    TypeKind    kind;      /* concrete resolved TypeKind (e.g. TY_INT) */
    /* TP3: full Type for ADT/struct bindings (e.g. `a → Foo`); NULL for primitives */
    struct Type *full_type;
    /* SZ6: when this parameter is a size index, the captured type-level size
     * term from the constructor's return type (e.g. `(Add (Static 1) n)`).
     * NULL for non-size bindings; additive, so existing resolution is
     * unchanged. */
    struct SizeTerm *size_index;
} SkolemBinding;

/* Phase G2: Per-arm skolem environment (stack-allocated in elab_match) */
typedef struct SkolemEnv {
    SkolemBinding bindings[MAX_SKOLEM_BINDINGS];
    uint8_t       n;
} SkolemEnv;

/* Phase G0: Constructor descriptor for ADTs */
typedef struct CtorDef {
    const char   *name;        /* constructor name (interned) */
    uint32_t      n_fields;
    CtorField    *fields;      /* arena-allocated array */
    struct AdtDef *adt;        /* back-pointer to parent ADT */
    uint32_t      tag;         /* integer discriminant tag (0-based) */
    /* Phase G1: explicit return-type annotation for defgadt constructors.
     * NULL for plain defdata constructors.  The Form type is forward-declared
     * in forms.h which is already included above. */
    const struct Form *result_type_form; /* raw parsed annotation, e.g. (Tag int) */
    /* Phase G2: raw field-type annotation forms for skolem-aware resolution.
     * field_forms[i] is the type-annotation form for field i.
     * NULL for plain defdata constructors. */
    const struct Form **field_forms;
} CtorDef;

/* Phase G0: ADT (sum type) descriptor */
typedef struct AdtDef {
    const char *name;            /* ADT name (interned) */
    uint32_t    n_ctors;
    CtorDef   **ctors;           /* arena-allocated pointer array */
    bool        is_copy;         /* :copy annotation */
    bool        needs_drop_glue; /* any ctor has rc/ref/weak fields */
    /* Phase G1: GADT flag and type parameters */
    bool        is_gadt;         /* true for defgadt, false for defdata */
    const char **type_params;    /* arena-allocated array of type param names (interned) */
    uint8_t     n_type_params;
    /* TP1/TP2: arena-alloc'd Kind array, one per type_params entry.
     * Initialised to KIND_STAR; TP4 refines based on usage in CtorField.full_type. */
    Kind       *type_param_kinds;
    /* Owning compilation unit (diag file id of the defdata/defgadt form).
     * Mirrors StructDef.origin_file_id so the orphan-instance check can credit
     * an ADT type-argument to the module that defines it. 0 = unknown. */
    uint16_t    origin_file_id;
} AdtDef;

/* Phase 11: Struct field descriptor.
 * Stored inline in StructDef.fields[]. */
typedef struct StructField {
    TypeKind kind;               /* field type kind */
    TypeKind inner_kind;         /* for rc/ref/weak, the inner type; TY_UNKNOWN otherwise */
    const char *name;            /* field name (interned string, NUL-terminated) */
    /* Phase 16 v2: effect-row annotation for :fn fields (NULL = no annotation) */
    struct EffectRow *effect_row;
    /* F8 (cross-plan-followups): full Type for compound field annotations
     * (TY_EXISTS, TY_APP, TY_FORALL).  NULL when the field's type is a
     * simple keyword/sym that the kind/inner_kind summary already
     * captures fully; consumers that only need the summary kind keep
     * working unchanged.  Field-access elaboration uses this to return
     * the full type at use sites (e.g. so `(open (.field s) ...)` sees
     * the right TY_EXISTS payload instead of falling into the
     * TY_PTR_VOID branch of EX_EXISTS_OPEN). */
    struct Type *full_type;
} StructField;

/* Phase 11: Struct type descriptor.
 * Created by elab_defstruct; pointed to from Type.as.struct_.def. */
typedef struct StructDef {
    const char *name;       /* struct name (interned string, NUL-terminated) */
    uint32_t n_fields;
    StructField *fields;    /* field array (malloc'd) */
    bool is_copy;           /* :copy annotation */
    bool is_linear;         /* LT4: :linear annotation -- exactly-once (CK_LINEAR) */
    bool is_affine;         /* :affine annotation -- at-most-once (SK_AFFINE / CK_UNIQUE) */
    /* end-to-end-monomorphization (M3->M7, Vec typed-pointer vertical slice):
     * :heap marks a parameterized type whose natural monomorphic ABI is a typed
     * POINTER (`T__A *`) to a heap-allocated header, not a by-value struct -- the
     * parametric-type ABI matrix's "typed pointer" class (Vec/Map/Set/MutableMap/
     * Cons/GVec). type_c_name lowers a :heap-tagged TY_STRUCT/TY_APP to `Name *`.
     * Inert until a type is tagged; see
     * docs/upcoming/v2/vec-typed-pointer-vertical-slice-plan.md. */
    bool is_heap;
    bool needs_drop_glue;   /* true if any field is rc/ref/weak */
    /* SI4-C: opaque newtype -- always int64_t in C; name only used for REPL type tags. */
    bool is_opaque;
    /* Phase HKT-P4: file that defined this struct (for orphan instance check).
     * file_id mirrors Span.file_id; 0 means unknown/builtin. */
    uint16_t origin_file_id;
    /* Phase TM0: type parameters for parameterized structs (e.g. Map[K V], Vec[A]). */
    const char **type_params;   /* arena-allocated array of type param name strings */
    uint8_t     n_type_params;
    /* TP4: arena-alloc'd Kind array, one per type_params entry.
     * Initialised to KIND_STAR; infer_struct_type_param_kinds refines based on
     * usage in StructField.full_type.  NULL when n_type_params == 0. */
    Kind       *type_param_kinds;
    /* Phase D: sum of field sizes exceeds 16 bytes -- pass as const T* at C ABI level.
     * Only meaningful for non-parameterized structs (n_type_params == 0); for generic
     * structs the per-instantiation RegisteredStructApp.pass_by_ptr flag is used. */
    bool        pass_by_ptr;
} StructDef;

/* Phase 11: canonical default copy-kind by kind (typeclass path is primary; this
 * keeps concrete move/copy semantics deterministic for elaboration/codegen). */
static inline CopyKind typekind_default_copy_kind(TypeKind k) {
    switch (k) {
        case TY_NIL:
        case TY_BOOL:
        case TY_INT:
        case TY_FLOAT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_FN:
        case TY_REF_IMMUT:
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
            return CK_COPY;
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_MUT:
        case TY_EXCEPTION:
        case TY_CONT:
        case TY_STRUCT:   /* default move; actual copy_kind set via type_struct() */
        case TY_ADT:      /* default move; actual copy_kind set via type_adt() */
        case TY_NEVER:     /* never type is move-only (no values exist) */
        case TY_REC:       /* recursive type is move-only in v1 */
            return CK_MOVE;
        /* LT3: lref<T> is always linear — exactly-once ownership */
        case TY_LREF:
            return CK_LINEAR;
        case TY_SET:       /* heap-allocated sorted array — pointer-copied in v1 */
            return CK_COPY;
        case TY_APP:       /* type application — opaque int64_t handle, copy by value */
            return CK_COPY;
        /* Phase N: fixed-width numeric types are all Copy */
        case TY_INT8:
        case TY_INT16:
        case TY_INT32:
        case TY_INT64:
        case TY_UINT8:
        case TY_UINT16:
        case TY_UINT32:
        case TY_UINT64:
        case TY_FLOAT32:
        case TY_FLOAT64:
            return CK_COPY;
        /* Phase HRT0: Quantified types — move-only (no concrete values) */
        case TY_FORALL:
        case TY_EXISTS:
            return CK_MOVE;
        /* IT0: Union types — move-only by default; actual semantics depend on members */
        case TY_UNION:
            return CK_MOVE;
        /* IT2: Intersection types — move-only by default; actual semantics depend on members */
        case TY_INTERSECTION:
            return CK_MOVE;
        /* IT4: any — top type is copy (it's an opaque container) */
        case TY_ANY:
            return CK_COPY;
        /* ET3: handler values are copyable (they are function-pointer structs) */
        case TY_HANDLER:
            return CK_COPY;
        /* CT0: contract type copies like its base type (defaults to copy for safety) */
        case TY_CONTRACT:
            return CK_COPY;
        /* SS0a: Session channels are linear (exactly-once protocol discipline).
         * Protocol descriptor types (Send, Recv, etc.) are move-only; they are
         * type-level constructs that never become runtime values by themselves. */
        case TY_SESSION:
            return CK_LINEAR;
        case TY_SEND:
        case TY_RECV:
        case TY_CLOSE:
        case TY_CHOOSE:
        case TY_BRANCH:
        case TY_SESSION_REC:
        case TY_TIMEOUT:
        case TY_SESSION_PAIR:
        case TY_SESSION_RECV_PAIR:
        case TY_SESSION_OFFER:
            return CK_MOVE;
        /* SS5: Global protocol types -- compile-time only, linear endpoints */
        case TY_GLOBAL:
            return CK_MOVE;
        case TY_ROLE:
            return CK_LINEAR;
        /* DV0: Dynamic var reference is copy (it's an opaque elaboration-time marker) */
        case TY_DYNVAR:
            return CK_COPY;
        /* GF1: Generator is a heap pointer — copy by value (pointer copy) */
        case TY_GENERATOR:
            return CK_COPY;
        /* SYM0: a symbol is an interned pointer into .rodata — freely copyable */
        case TY_SYM:
            return CK_COPY;
        /* Variadic HKT rows: a type-level row is compile-time only -- it never
         * names a runtime value, so the copy/move discipline is moot; COPY. */
        case TY_TYPEROW:
            return CK_COPY;
        case TY_UNKNOWN:
        default:
            return CK_MOVE;
    }
}
#define MAX_FN_ARITY 16

/* Phase 13: Maximum lifetime parameters per type */
#define MAX_TYPE_LIFETIMES 4

/* Type uses a union to store either a simple kind or function info. */
typedef struct Type {
    TypeKind kind;
    /* Phase 11: Copy/Move trait */
    CopyKind copy_kind;
    /* ST0: Substructural discipline (SK_STRUCTURAL by default) */
    SubstructKind substruct;
    /* Phase 13: Lifetime annotations */
    /* Lifetimes attached to this type (for &T, &mut T, function types with lifetime params) */
    LifetimeId lifetimes[MAX_TYPE_LIFETIMES];
    uint8_t   n_lifetimes;
    /* Phase 15: Typeclasses */
    /* For concrete types, the typeclass instances they implement (e.g., int has Eq, Show) */
    TypeClassInstance **typeclass_instances;
    uint8_t n_typeclass_instances;
    /* Phase TP3: kind annotation -- uint16_t encoding (see Kind typedef above).
     * KIND_STAR for concrete types; KIND_ARROW{N} for N-ary type constructors. */
    Kind hkt_kind;
    /* c-fn-ptr-element-and-size-precision-gap fix: CNumSpelling for integer
     * carriers (0 == CNUM_DEFAULT).  Only consulted by type_c_name in the
     * precise cfnptr typedef path; erased everywhere else. */
    uint8_t c_num_spelling;
    union {
        struct {
            TypeKind arg_kinds[MAX_FN_ARITY];
            TypeKind result_kind;
            uint8_t arity;
            /* Future-proofing for v3 effects: effect row slot.
             * NULL in v0/v1; treated as empty effect set. */
            struct EffectRow *effect_row;
            /* Phase HRT1: full type info for polymorphic (rank-2+) parameters.
             * NULL for ordinary functions; set when any arg is a forall type.
             * Points to an arena-allocated array of n=arity Type pointers.
             * arg_full_types[i] is NULL for monomorphic args, non-NULL for poly. */
            struct Type **arg_full_types;
            struct Type  *result_full_type; /* NULL or full result type for poly results */
            bool arg_linear[MAX_FN_ARITY];     /* LT2: true if the i-th param is ^linear */
            bool arg_unique[MAX_FN_ARITY];     /* UT0: true if the i-th param is ^unique */
            bool arg_unique_mut[MAX_FN_ARITY]; /* UT2: true if the i-th param is ^unique ^mut */
            bool arg_affine[MAX_FN_ARITY];     /* ST0: true if the i-th param is ^affine */
            bool arg_relevant[MAX_FN_ARITY];   /* ST0: true if the i-th param is ^relevant */
            /* LB1: true if the i-th param is ^borrow -- it reads its linear/affine
             * argument without consuming it.  A linear binding passed to a ^borrow
             * param is borrowed (the single-consumption obligation is preserved for
             * a later consuming op).  Borrowing an already-consumed value is still a
             * TUR-E0101.  See docs/reported/stdlib-linear-handle-borrows.md. */
            bool arg_borrow[MAX_FN_ARITY];
            /* A#1: true if the i-th param is ^fat -- it consumes its argument via
             * the fat-closure calling convention (thunk = slot 0, env = the heap
             * struct).  A bare non-capturing fn passed here is auto-shimmed into a
             * fat closure at the call site (see EX_FN_TO_FAT); a non-fn argument is
             * a typed error.  Distinguishes fat consumers (reactor cb, free-bind
             * kont) from raw-C-callback params (hamt fn) that share the same kind. */
            bool arg_fat[MAX_FN_ARITY];
            /* Phase CCL (fn-first-class-application): true if the i-th param is a
             * first-class `:fn` poly-closure carrier (tur_poly_fn_t {env, fn}),
             * i.e. a hand-written `:fn` parameter outside a typeclass method.
             * Distinct from a rank-2 forall (whose full type is carried in
             * arg_full_types[i]); a function/lambda/closure argument is boxed
             * into the carrier via EX_POLY_WRAP at the call site. */
            bool arg_poly_fn[MAX_FN_ARITY];
            /* A#1 (return position): true when the result type carries the
             * ^fat marker.  A bare non-capturing fn returned from this
             * function is auto-shimmed into a fat closure (EX_FN_TO_FAT) at
             * every tail/return leaf, so a fat-call consumer that reads the
             * returned value sees a valid { thunk, env } layout instead of a
             * bare function pointer.  Symmetric with arg_fat[]. */
            bool result_fat;
            /* AR6: variadic rest-param support (& rest :type) */
            bool is_variadic;                  /* true if this fn has a & rest parameter */
            TypeKind rest_kind;                /* type of the rest cons-list elements (fast-path) */
            /* Typed variadic rest: full Type for the rest element when it is a
             * user-defined type (opaque / struct / ADT / type application).
             * NULL for primitive rest (`& rest :int`, etc.), in which case the
             * fast-path TypeKind comparison on rest_kind is used. */
            struct Type *rest_full_type;
            /* LS4: index of the parameter whose lifetime the borrow return is
             * tied to (the returned &'a T aliases this argument's storage), or
             * -1 when the return is not a lifetime-tied borrow.  Used by the
             * inter-procedural borrow-escape check at call sites. */
            int8_t result_borrow_arg;
            /* CRU Phase 3 / Option B (first-class closure type): true when this
             * TY_FN denotes a *closure value* -- a fat box { thunk, env... } --
             * rather than a bare function reference (captureless fn / C function
             * pointer address).  A boxed TY_FN is always called through the fat
             * protocol (thunk = slot 0, env = the box) for all arities; a bare
             * TY_FN coerces to a boxed one of the same signature via the
             * EX_FN_TO_FAT auto-shim, never the reverse.  See
             * docs/upcoming/closure-first-class-type-plan.md.  B-0 only plumbs
             * the bit; nothing sets it true yet. */
            bool boxed;
            /* typed-c-abi-function-pointers: true when this TY_FN denotes a
             * bare C-ABI function pointer `R (*)(A...)` -- spelled `(c-fn
             * [A...] R)` in source -- rather than a Turmeric closure value.
             * A cfnptr is *never* boxed; it carries no implicit environment
             * and lowers to the concrete `R (*)(A...)` typedef (via
             * register_fn_ptr_typedef) in every position.  At the type level
             * it is distinct from an ordinary `(fn ...)`: a *capturing* closure
             * (a boxed fat box) cannot satisfy a cfnptr parameter, while a
             * *captureless* bare fn of the same signature coerces in (a
             * captureless fn IS a bare code pointer at the C ABI).  See
             * docs/reported/typed-c-abi-function-pointers.md. */
            bool cfnptr;
            /* sized-types-cross-param-unification: per-parameter raw type
             * annotation Form*, retained so call-site elaboration can
             * re-extract the size-index template (e.g. `(SizedVec n)`)
             * and unify shared size variables across multiple parameters.
             * NULL when no per-param Forms were recorded (e.g. extern-c,
             * thunks, or non-defn fn values).  Arena-allocated array of
             * length `arity`; individual entries may be NULL. */
            const struct Form **param_type_forms;
            /* sized-types-cross-param-unification (non-GADT extension):
             * raw declared-return-type Form, retained so a call expression
             * can infer its `size_index` from a phantom size literal in the
             * callee's return type (e.g. `(Dense (Static 2) A)`).  Mirrors
             * how CtorDef.result_type_form lets sz8_infer_ctor_size_index
             * seed a sized-GADT constructor's index; this extends the same
             * treatment to plain defn-shaped non-GADT carriers (defopaque,
             * defstruct phantom indices).  NULL when no return annotation
             * was recorded. */
            const struct Form *result_type_form;
        } fn;
        /* Phase 5: ref<T> stores the inner type T */
        struct {
            TypeKind inner;   /* The type T that ref<T> owns */
        } ref;
        /* ptr-generic-parameterised-type: typed raw pointer ptr<T>.
         * Carried on kind == TY_PTR_VOID; `inner` is the full pointee type T
         * (arena-allocated), or NULL for the legacy untyped ptr<void> spelling.
         * Folding the typed form onto TY_PTR_VOID (rather than a new TypeKind)
         * keeps every existing raw-pointer code path working unchanged while
         * letting codegen lower ptr<T> to `T *` in C. */
        struct {
            struct Type *inner; /* pointee type T, or NULL for ptr<void> */
            /* c-fn-ptr-element-and-size-precision-gap fix: true for the
             * `ptr<const-T>` spelling -- type_c_name emits `const T *` so a
             * typed c-fn parameter matches an external C callback's
             * const-qualified pointer slot (e.g. `const unsigned char *`).
             * A pure C-spelling hint; the runtime carrier is an ordinary
             * pointer, so a const ptr is ABI-identical to a non-const one. */
            bool is_const;
        } ptr;
        /* Phase 9: rc<T> and weak<T> store the inner type T */
        struct {
            TypeKind inner;   /* The type T that rc<T> or weak<T> points to */
            /* Phase DS3: when inner == TY_STRUCT and the rc<T> was parsed from
             * an `rc<Name>` defstruct field annotation, this is the StructDef *
             * for `Name`.  NULL otherwise.  Used so that `(.field rc-of-s)` and
             * `(set! (.field rc-of-s) v)` can resolve struct fields without
             * losing the def through the rc wrapper. */
            struct StructDef *struct_def;
        } rc;
        /* Phase 12: Borrow types store the referenced type T */
        struct {
            TypeKind target;  /* The type T being referenced by &T or &mut T */
        } ref_borrow;
        /* Phase 15: Typeclass types */
        struct {
            TypeClass *typeclass;  /* For TY_TYPECLASS - the typeclass itself */
        } typeclass;
        struct {
            TypeClassInstance *instance;  /* For TY_TYPECLASS_INST - a specific instance */
        } typeclass_inst;
        /* Phase 17: Exception types */
        struct {
            TypeKind payload_type;  /* The type of the exception payload */
        } exn;
        /* Phase 18 / CC4: Continuation types. `flavor` selects the runtime that
         * (k v) application sugar resumes against (cps-transform-plan):
         *   CONT_CLONEABLE -- cloneable/call-cc* (tur_cloneable_cont_resume)
         *   CONT_ESCAPE    -- call/cc / escape   (tur_escape_resume)
         *   CONT_SERIAL    -- serial-shift       (tur_serial_cont_resume)  */
        struct {
            TypeKind returns;  /* The type T that cont<T> returns */
            int      flavor;   /* ContFlavor; 0 = CONT_CLONEABLE (default) */
        } cont;
        /* Phase 11: Struct types */
        struct {
            StructDef *def;    /* The struct type descriptor */
        } struct_;
        /* Phase HKT-P1: Type application — (type-app F A) */
        struct {
            struct Type *fn;   /* The type constructor being applied (kind * -> * or * -> * -> *) */
            struct Type *arg;  /* The type argument (kind *) */
        } app;
        /* Phase HKT-P2: Recursive type binder — (defrec Name [params] body) */
        struct {
            const char *name;  /* Interned binder name (e.g. "Fix"); compare by pointer */
            struct Type *body; /* Body type (NULL in v1 when body is not yet evaluated) */
        } rec;
        /* Phase G0: ADT types */
        struct {
            AdtDef *def;
        } adt_;
        /* Phase HRT0: Universally/existentially quantified types.
         * Phase EX1b: constraint storage for `(exists [a] [(C a) ...] body)`. */
        struct {
            const char **var_names; /* bound variable names (interned, arena-allocated) */
            Kind        *var_kinds; /* kinds of bound variables (arena-allocated) */
            uint8_t      n_vars;
            struct Type *body;      /* body type (arena-allocated) */
            /* EX1b: Optional typeclass constraints on the bound variables.
             * Each entry: (typeclass, var_idx) meaning "(typeclass var_names[var_idx])".
             * n_constraints==0 means no constraints (back-compat with HRT0). */
            TypeClass  **constraint_classes; /* arena-allocated; length n_constraints */
            uint8_t     *constraint_var_idx; /* arena-allocated; length n_constraints */
            uint8_t      n_constraints;
            /* EXG6: `:linear` attribute on `(exists :linear [a] [(C a) ...] body)`.
             * When set, the existential bypasses rc_cb_alloc and the let binding
             * for a pack of this type is treated as a linear value (use-exactly-
             * once via LT1's substructural machinery). */
            bool         is_linear;
        } forall_;
        /* IT0: Union types — (A | B | C) */
        struct {
            struct Type **members;  /* arena-allocated array of member type pointers */
            uint8_t       n_members; /* number of union members (>= 2) */
        } union_;
        /* IT2: Intersection types — (A & B & C) */
        struct {
            struct Type **members;  /* arena-allocated array of member type pointers */
            uint8_t       n_members; /* number of intersection members (>= 2) */
        } intersection_;
        /* Variadic HKT rows (Layer 2): a row of element types `#row{T1 T2 ...}`.
         * Mirrors the union_/intersection_ representation: an arena-allocated
         * array of element Type pointers plus a count.  An empty row (n == 0)
         * is legal (the unit row). */
        struct {
            struct Type **elements;   /* arena-allocated array of element type pointers */
            uint8_t       n_elements; /* number of element types (>= 0) */
            /* P0 typed-field rows: when non-NULL, a parallel array of interned
             * field-name strings of length n_elements. NULL = bare positional
             * row (the existing ECS form `#row{Pos Vel}`). Typed-field rows
             * `#row{k1 : T1 k2 : T2}` populate this so two rows that differ
             * only in field names are distinct types. */
            const char  **field_names;
        } typerow_;
        /* Phase HRT/G2: Named type variable -- parameter typed with a GADT type var */
        struct {
            const char *name;  /* interned type var name (e.g. "a"), or NULL for anonymous escaped skolem */
        } tyvar_;
        /* ET3/FH4.1: Handler type — handler<EffectRow, ValueType, ResultType> */
        struct {
            const char *effect_name;  /* FH4.1: single-effect name, or NULL for a
                                       * multi-effect (composed) handler.  Kept for
                                       * source compatibility; the authoritative
                                       * handled set is handled_row. */
            struct EffectRow *handled_row; /* FH4.1: the set of effects this handler
                                       * handles (an ERK_UNRESOLVED name-set built at
                                       * parse time; a one-element row in the single-
                                       * effect case, the union under composition).
                                       * May be NULL for legacy single-effect types. */
            TypeKind    value_kind;   /* the type of the value passed to the effect */
            TypeKind    result_kind;  /* the result type of the handle expression */
            CopyKind    cont_kind;    /* LC0: ownership discipline for the continuation k */
        } handler_;
        /* CT0: Contract type — { x : T | p } */
        struct {
            struct Type      *base_type;  /* the underlying type T */
            const char       *var_name;   /* bound variable x in { x : T | p } */
            const struct Form *predicate; /* predicate expression p (a Form*) */
        } contract_;
        /* SS0a: Session protocol type arguments (-Xsessions) */
        struct {
            struct Type *fst;   /* TY_SESSION: protocol P
                                   TY_SEND/TY_RECV: message type T
                                   TY_CHOOSE/TY_BRANCH: left branch P
                                   TY_SESSION_REC: body type */
            struct Type *snd;   /* TY_SEND/TY_RECV: continuation Q
                                   TY_CHOOSE/TY_BRANCH: right branch Q
                                   NULL for TY_SESSION, TY_CLOSE, TY_SESSION_REC */
            const char  *label; /* TY_SESSION_REC: binder label (interned) */
        } session_;
        /* SS5: Global protocol type (compile-time only) */
        struct {
            const char           *name;     /* interned protocol name */
            const char          **roles;    /* arena-allocated array of interned role names */
            int                   n_roles;
            GlobalInteraction    *body;     /* interaction tree (arena-allocated) */
        } global_;
        /* SS5: Role endpoint type -- in-progress multi-party channel endpoint */
        struct {
            struct Type          *global_type; /* TY_GLOBAL type node */
            const char           *role_name;   /* interned role name */
            GlobalInteraction    *current_step; /* current position in interaction tree */
        } role_;
        /* DV0: Dynamic var type (-Xdynamic-vars) */
        struct {
            struct Type *value_type; /* the declared element type of the dynamic var */
        } dynvar_;
        /* GF1: Generator type -- heap pointer to C state-machine struct */
        struct {
            TypeKind element_kind; /* the TypeKind of values yielded by this generator */
        } generator_;
    } as;
} Type;

/* DV0: Dynamic var type constructor (-Xdynamic-vars).
 * Wraps the declared value type.  Only stored in DynVarEntry during elaboration;
 * user-visible expressions have the value type directly. */
Type type_dynvar(Arena *a, Type value_type);

/* Helper to check if a type has lifetime annotations */
static inline bool type_has_lifetime(Type t) {
    return t.n_lifetimes > 0;
}

/* Helper to get the first lifetime from a type */
static inline LifetimeId type_first_lifetime(Type t) {
    return t.n_lifetimes > 0 ? t.lifetimes[0] : LIFETIME_NONE;
}

/* Phase T19-B: Thread-safety marker helpers.
 *
 * type_is_send(t): true iff a value of type `t` can be transferred to another
 *   thread (ownership move across thread boundary is safe).
 * type_is_sync(t): true iff a value of type `t` can be shared across threads
 *   (immutable or internally synchronized).
 *
 * Rules (conservative defaults derived from TypeKind):
 *   - Primitives, TY_NIL, TY_BOOL, TY_INT, TY_FLOAT, TY_CSTR, TY_PTR_VOID,
 *     TY_FN, TY_TYPECLASS*, TY_EXCEPTION, TY_STRUCT (default): Send + Sync.
 *   - TY_REF, TY_RC, TY_WEAK:   neither Send nor Sync (single-threaded RC).
 *   - TY_CONT:                   neither Send nor Sync (captures C stack).
 *   - TY_REF_IMMUT, TY_REF_MUT: neither Send nor Sync (borrows; not owned).
 *
 * Struct field propagation and Arc<T>/Mutex<T>/Chan<T> Send+Sync derivation
 * are handled at call sites (T19-C) when those types are introduced.
 */
static inline bool type_is_send(Type t) {
    switch (t.kind) {
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_CONT:
        case TY_CLONEABLE_CONT:
        case TY_REF_IMMUT:
        case TY_REF_MUT:
            return false;
        case TY_NEVER:
            /* Never type has no values, so vacuously Send */
            return true;
        default:
            return true;
    }
}

static inline bool type_is_sync(Type t) {
    /* Sync implies Send; use the same conservative rule set in v1. */
    return type_is_send(t);
}

/* LT0 / UT0: Predicate helpers for ownership/linearity classification */
static inline bool ty_is_linear(Type t) {
    return t.copy_kind == CK_LINEAR;
}
/* UT0: ty_is_unique -- true iff the type is uniquely owned (at-most-one alias) */
static inline bool ty_is_unique(Type t) {
    return t.copy_kind == CK_UNIQUE;
}
static inline bool ty_is_move(Type t) {
    /* CK_UNIQUE == CK_MOVE (alias); kept for backward compat */
    return t.copy_kind == CK_UNIQUE;
}
static inline bool ty_is_copy(Type t) {
    return t.copy_kind == CK_COPY;
}
/* ST0: Substructural discipline predicates */
static inline bool ty_is_affine(Type t) {
    return t.substruct == SK_AFFINE;
}
static inline bool ty_is_relevant(Type t) {
    return t.substruct == SK_RELEVANT;
}
static inline bool ty_is_sublinear(Type t) {
    return t.substruct == SK_LINEAR;
}

/* Phase G1: type_requires_refinement -- true iff a value of this type may need
 * GADT-arm type refinement before its fields can be safely accessed.
 * A TY_ADT with is_gadt=true is rank-2 or higher in the GADT sense: matching
 * on its constructor introduces skolem equalities that refine the type
 * parameters.  Supports future tooling and the HRT rank-checking path. */
static inline bool type_requires_refinement(Type t) {
    if (t.kind != TY_ADT) return false;
    return t.as.adt_.def && t.as.adt_.def->is_gadt;
}

/* Convert TypeKind to string representation for debugging */
const char *typekind_to_string(TypeKind k);

/* ----- Phase SZ6/SZ7: type-level size index terms ----- */

/* Parse a type-position Form into a SizeTerm, or return NULL if `f` is not a
 * size expression.  Recognises `(Static n)`, `(Add a b)`, `(Mul a b)`, and a
 * bare symbol (treated as a size variable).  `is_size_var(name, ctx)` decides
 * whether a bare symbol is a size variable; pass NULL to treat every bare
 * non-numeric symbol as a variable.  Arena-allocated. */
struct Form;
SizeTerm *size_term_from_form(Arena *a, const struct Form *f,
                              bool (*is_size_var)(const char *, void *),
                              void *ctx);

/* Substitute size variable `var` with `replacement` throughout `t`, returning a
 * fresh term (or `t` unchanged when `var` does not occur). */
SizeTerm *size_term_subst(Arena *a, const SizeTerm *t,
                          const char *var, const SizeTerm *replacement);

/* Fold a closed size term (no variables) to its integer value.
 * Returns true and writes *out on success; false if the term mentions any
 * size variable (i.e. is not statically known). */
bool size_term_eval(const SizeTerm *t, int64_t *out);

/* Structural/normalised equality: true iff the two terms are provably equal by
 * constant folding + syntactic normalisation (commutative Add/Mul, identity
 * elimination).  Used by SZ7 static size checking. */
bool size_term_equal(const SizeTerm *a, const SizeTerm *b);

/* Render a size term for diagnostics, e.g. "(+ 1 n)" or "5".  Writes into buf
 * (size cap) and returns buf. */
const char *size_term_to_string(const SizeTerm *t, char *buf, size_t cap);

/* Convert type name string to TypeKind */
TypeKind typekind_from_name(const char *name);

static inline Type type_simple(TypeKind kind, CopyKind copy_kind) {
    Type t = {0};
    t.kind = kind;
    t.copy_kind = copy_kind;
    t.hkt_kind = KIND_STAR;
    return t;
}

#define TYPE_UNKNOWN  (type_simple(TY_UNKNOWN, CK_MOVE))
#define TYPE_NIL      (type_simple(TY_NIL, CK_COPY))
#define TYPE_BOOL     (type_simple(TY_BOOL, CK_COPY))
#define TYPE_INT      (type_simple(TY_INT, CK_COPY))
#define TYPE_FLOAT    (type_simple(TY_FLOAT, CK_COPY))
/* Phase N: fixed-width numeric type macros */
#define TYPE_INT8     (type_simple(TY_INT8,   CK_COPY))
#define TYPE_INT16    (type_simple(TY_INT16,  CK_COPY))
#define TYPE_INT32    (type_simple(TY_INT32,  CK_COPY))
#define TYPE_INT64    (type_simple(TY_INT64,  CK_COPY))
#define TYPE_UINT8    (type_simple(TY_UINT8,  CK_COPY))
#define TYPE_UINT16   (type_simple(TY_UINT16, CK_COPY))
#define TYPE_UINT32   (type_simple(TY_UINT32, CK_COPY))
#define TYPE_UINT64   (type_simple(TY_UINT64, CK_COPY))
#define TYPE_FLOAT32  (type_simple(TY_FLOAT32, CK_COPY))
#define TYPE_FLOAT64  (type_simple(TY_FLOAT64, CK_COPY))
#define TYPE_CSTR     (type_simple(TY_CSTR, CK_COPY))
#define TYPE_PTR_VOID (type_simple(TY_PTR_VOID, CK_COPY))
#define TYPE_NEVER    (type_simple(TY_NEVER, CK_MOVE))

/* ptr-generic-parameterised-type: typed raw pointer ptr<T>.
 * Lives on TY_PTR_VOID with a non-NULL pointee; inner == NULL is ptr<void>. */
static inline Type type_ptr(struct Type *inner) {
    Type t = {0};
    t.kind = TY_PTR_VOID;
    t.copy_kind = CK_COPY;       /* raw pointer: bitwise copyable, like ptr<void> */
    t.hkt_kind = KIND_STAR;
    t.as.ptr.inner = inner;
    return t;
}
/* SYM0: interned runtime symbol type (-Xsymbols) */
#define TYPE_SYM      (type_simple(TY_SYM, CK_COPY))

/* Phase 5: ref<T> type constructor */
static inline Type type_ref(TypeKind inner) {
    Type t;
    t.kind = TY_REF;
    t.copy_kind = CK_UNIQUE;  /* ref<T> is uniquely owned (UT0) */
    t.as.ref.inner = inner;
    t.n_lifetimes = 0;
    return t;
}

/* LT3: lref<T> type constructor — linear owning pointer */
static inline Type type_lref(TypeKind inner) {
    Type t;
    t.kind = TY_LREF;
    t.copy_kind = CK_LINEAR;    /* lref<T> is exactly-once */
    t.substruct  = SK_LINEAR;   /* ST0: lref<T> has the linear substructural discipline */
    t.as.ref.inner = inner;
    t.n_lifetimes = 0;
    return t;
}

/* SS0a: Session type constructors (-Xsessions).
 * These take arena-allocated Type* arguments; callers own allocation.
 * Protocol descriptors (Send, Recv, etc.) are move-only type-level constructs.
 * Only TY_SESSION gets CK_LINEAR + SK_LINEAR (it is the channel endpoint). */

static inline Type type_session(struct Type *proto) {
    Type t = {0};
    t.kind = TY_SESSION;
    t.copy_kind = CK_LINEAR;
    t.substruct = SK_LINEAR;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = proto;
    return t;
}

static inline Type type_send(struct Type *msg, struct Type *cont) {
    Type t = {0};
    t.kind = TY_SEND;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = msg;
    t.as.session_.snd = cont;
    return t;
}

static inline Type type_recv(struct Type *msg, struct Type *cont) {
    Type t = {0};
    t.kind = TY_RECV;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = msg;
    t.as.session_.snd = cont;
    return t;
}

static inline Type type_close(void) {
    Type t = {0};
    t.kind = TY_CLOSE;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    return t;
}

static inline Type type_choose(struct Type *left, struct Type *right) {
    Type t = {0};
    t.kind = TY_CHOOSE;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = left;
    t.as.session_.snd = right;
    return t;
}

static inline Type type_branch(struct Type *left, struct Type *right) {
    Type t = {0};
    t.kind = TY_BRANCH;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = left;
    t.as.session_.snd = right;
    return t;
}

/* TY_SESSION_REC: recursive protocol mu-type.
 * label is an interned string (e.g. "echo"); body is the unrolled protocol. */
static inline Type type_session_rec(const char *label, struct Type *body) {
    Type t = {0};
    t.kind = TY_SESSION_REC;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.label = label;
    t.as.session_.fst = body;
    return t;
}

/* SS3c: TY_TIMEOUT -- Timeout[Q, P] protocol node.
 * fst = Q: success continuation (message was received in time).
 * snd = P: timeout continuation (no message before deadline).
 * Timeout is self-dual: both endpoints experience the same timing outcome. */
static inline Type type_timeout(struct Type *success_proto, struct Type *timeout_proto) {
    Type t = {0};
    t.kind = TY_TIMEOUT;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = success_proto;
    t.as.session_.snd = timeout_proto;
    return t;
}

/* SS1: TY_SESSION_PAIR -- internal pair returned by make-session.
 * fst = first session endpoint type (Session[P])
 * snd = second session endpoint type (Session[dual(P)])
 * Never a runtime value; always immediately destructured by vector let. */
static inline Type type_session_pair(struct Type *first_sess, struct Type *second_sess) {
    Type t = {0};
    t.kind = TY_SESSION_PAIR;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = first_sess;
    t.as.session_.snd = second_sess;
    return t;
}

/* SS2: TY_SESSION_RECV_PAIR -- internal pair returned by recv.
 * fst = received value type (e.g. int); snd = continuation session type Session[Q].
 * Never a runtime value; always immediately destructured by vector let. */
static inline Type type_session_recv_pair(struct Type *val_type, struct Type *cont_type) {
    Type t = {0};
    t.kind = TY_SESSION_RECV_PAIR;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = val_type;
    t.as.session_.snd = cont_type;
    return t;
}

/* SS2: TY_SESSION_OFFER -- internal result of offer.
 * fst = Session[P] (Left branch type); snd = Session[Q] (Right branch type).
 * Always consumed by a match expression; never a runtime value on its own. */
static inline Type type_session_offer(struct Type *left_type, struct Type *right_type) {
    Type t = {0};
    t.kind = TY_SESSION_OFFER;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.session_.fst = left_type;
    t.as.session_.snd = right_type;
    return t;
}

/* Phase 9: rc<T> type constructor */
static inline Type type_rc(TypeKind inner) {
    Type t;
    t.kind = TY_RC;
    t.copy_kind = CK_MOVE;  /* rc<T> is move-only by default */
    t.as.rc.inner = inner;
    t.as.rc.struct_def = NULL;
    t.n_lifetimes = 0;
    return t;
}

/* Phase DS3: rc<Struct> with the struct def carried alongside so that
 * field access through the rc can resolve the layout. */
static inline Type type_rc_struct(struct StructDef *def) {
    Type t;
    t.kind = TY_RC;
    t.copy_kind = CK_MOVE;
    t.as.rc.inner = TY_STRUCT;
    t.as.rc.struct_def = def;
    t.n_lifetimes = 0;
    return t;
}

/* Phase 9: weak<T> type constructor */
static inline Type type_weak(TypeKind inner) {
    Type t;
    t.kind = TY_WEAK;
    t.copy_kind = CK_MOVE;  /* weak<T> is move-only */
    t.as.rc.inner = inner;  /* Reuse the same field as rc */
    t.as.rc.struct_def = NULL;
    t.n_lifetimes = 0;
    return t;
}

/* Construct a function type from TypeKinds. */
static inline Type type_fn(TypeKind arg_kinds[], uint8_t arity, TypeKind result_kind) {
    Type t;
    t.kind = TY_FN;
    t.copy_kind = typekind_default_copy_kind(TY_FN);
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    t.as.fn.arity = arity;
    for (uint8_t i = 0; i < arity; i++) {
        t.as.fn.arg_kinds[i] = arg_kinds[i];
    }
    t.as.fn.result_kind = result_kind;
    t.as.fn.effect_row = NULL;
    t.as.fn.arg_full_types = NULL;
    t.as.fn.result_full_type = NULL;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_linear[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_unique[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_unique_mut[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_affine[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_relevant[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_borrow[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_fat[i] = false;
    for (uint8_t i = 0; i < MAX_FN_ARITY; i++) t.as.fn.arg_poly_fn[i] = false;  /* CCL: must initialise or UBSan fires on bool read (macOS miscompile) */
    t.as.fn.result_fat = false;  /* A#1: must initialise or UBSan fires on bool read */
    t.as.fn.boxed = false;  /* CRU B-1: must initialise or UBSan fires on bool read */
    t.as.fn.cfnptr = false; /* typed-c-abi-function-pointers: default to a Turmeric closure type */
    t.as.fn.is_variadic = false;  /* AR6: default non-variadic */
    t.as.fn.result_borrow_arg = -1; /* LS4: no lifetime-tied borrow return by default */
    t.as.fn.rest_kind   = TY_INT; /* AR6: default rest type */
    t.as.fn.rest_full_type = NULL; /* typed-variadic: NULL = primitive rest */
    t.as.fn.param_type_forms = NULL; /* sized-types-cross-param-unification: filled by defn elab */
    t.as.fn.result_type_form = NULL; /* SZ8 non-GADT: filled by defn elab when a return ann was recorded */
    return t;
}

/* Phase 12: Borrow type constructors */
/* &T - immutable borrow */
static inline Type type_ref_immut(TypeKind target) {
    Type t;
    t.kind = TY_REF_IMMUT;
    t.copy_kind = CK_COPY;  /* Borrows are copyable (they're just pointers) */
    t.as.ref_borrow.target = target;
    t.n_lifetimes = 0;
    return t;
}

/* &mut T - mutable borrow */
static inline Type type_ref_mut(TypeKind target) {
    Type t;
    t.kind = TY_REF_MUT;
    t.copy_kind = CK_MOVE;  /* Mutable borrows are move-only (exclusive access) */
    t.as.ref_borrow.target = target;
    t.n_lifetimes = 0;
    return t;
}

/* Phase 13: Lifetime-annotated borrow type constructors */
/* &'a T - immutable borrow with lifetime */
static inline Type type_ref_immut_lifetime(TypeKind target, LifetimeId lifetime) {
    Type t = type_ref_immut(target);
    if (lifetime != LIFETIME_NONE) {
        t.lifetimes[0] = lifetime;
        t.n_lifetimes = 1;
    }
    return t;
}

/* &'a mut T - mutable borrow with lifetime */
static inline Type type_ref_mut_lifetime(TypeKind target, LifetimeId lifetime) {
    Type t = type_ref_mut(target);
    if (lifetime != LIFETIME_NONE) {
        t.lifetimes[0] = lifetime;
        t.n_lifetimes = 1;
    }
    return t;
}

/* Phase 15: Typeclass type constructors */
/* Create a typeclass type (e.g., Eq, Ord) */
static inline Type type_typeclass(TypeClass *tc) {
    Type t;
    t.kind = TY_TYPECLASS;
    t.copy_kind = CK_COPY;  /* Typeclasses are copyable */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.typeclass.typeclass = tc;
    return t;
}

/* Create a typeclass instance type */
static inline Type type_typeclass_inst(TypeClassInstance *inst) {
    Type t;
    t.kind = TY_TYPECLASS_INST;
    t.copy_kind = CK_COPY;  /* Typeclass instances are copyable */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.typeclass_inst.instance = inst;
    return t;
}

/* Phase HKT-P1: Type application constructor */
/* Create a TY_APP type representing (F A) where F is a type constructor and A is a type argument.
 * Both fn and arg are copied into newly-allocated memory on the arena.
 * The result kind is computed from fn's kind using kind_of_type_app. */
Type type_app(Arena *a, Type fn, Type arg, Span span);

/* Lift the substructural discipline (:linear / :affine) from a TY_APP's head
 * StructDef onto the application node.  Call after constructing a TY_APP whose
 * `fn` carries an opaque/struct head -- without this, the application inherits
 * the default CK_COPY and the linear-discipline checker silently treats a
 * value of type `(WriteCap T)` as plain copyable.  See
 * docs/reported/parametric-linear-opaque-not-enforced.md. */
void propagate_app_discipline(Type *app, const Type *fn);

/* IT0: Union type constructor.
 * Create a TY_UNION type with the given array of member types.
 * members[] is copied from the provided arena-allocated pointers.
 * Nested TY_UNION members are flattened automatically. */
Type type_union_build(Arena *a, Type **members, uint8_t n_members);

/* IT2: Intersection type constructor.
 * Create a TY_INTERSECTION type with the given array of member types.
 * members[] is copied from the provided arena-allocated pointers.
 * Nested TY_INTERSECTION members are flattened automatically. */
Type type_intersection_build(Arena *a, Type **members, uint8_t n_members);

/* Variadic HKT rows (Layer 2): row-of-types constructor.
 * Create a TY_TYPEROW from `n_elements` element type pointers; the element
 * array is copied onto the arena (the caller's `elements` array need not
 * outlive the call, though the pointed-to Types must).  hkt_kind is set to
 * KIND_TYPEROW.  Unlike unions, element order is significant and duplicates
 * are preserved -- a row is a list, not a set.  n_elements == 0 is the unit
 * row.  Rows do NOT nest-flatten (a row containing a row stays nested). */
Type type_typerow(Arena *a, Type **elements, uint8_t n_elements);
/* P0 typed-field row: parallel field_names array. NULL field_names matches
 * bare-positional type_typerow(). */
Type type_typerow_named(Arena *a, Type **elements, const char **field_names,
                       uint8_t n_elements);

/* Variadic HKT rows: order-insensitive (permutation) row equality.
 * Returns true iff `a` and `b` are both TY_TYPEROW with the same multiset of
 * element types (same length, and each element of `a` pairs with a distinct,
 * type_eq-equal element of `b`).  This backs the data-frame "column-permutation
 * is a no-op" case; ordinary `type_eq` on rows stays order-SENSITIVE. */
bool type_typerow_eq_perm(Type a, Type b);

/* Variadic HKT rows (Layer 5): row algebra -- the type-level operations the
 * ECS query / relational layers build on. All are pure and compile-time. */
/* Membership: true if `row` contains an element type_eq to `elem`. */
bool type_typerow_contains(Type row, Type elem);
/* Concatenation (`++`): x ++ y, order-preserving, duplicates kept (clamped 255). */
Type type_typerow_concat(Arena *a, Type x, Type y);
/* Set-union (query join): x then y-not-in-x, order-preserving, deduplicated. */
Type type_typerow_union(Arena *a, Type x, Type y);
/* Intersection: x's elements also in y, x's order, deduplicated. */
Type type_typerow_intersect(Arena *a, Type x, Type y);
/* L6 follow-up D: canonical (sorted-by-type_name) copy of a row -- the opt-in
 * surface for permutation-aware equality. (row-canon #row{a b}) and
 * (row-canon #row{b a}) reduce to the same TY_TYPEROW, so ordinary type_eq
 * agrees. Compile-time only. */
Type type_typerow_canonical(Arena *a, Type x);

/* Phase 17: Exception type constructor */
/* Create an exception type wrapping a payload of the given type */
static inline Type type_exception(TypeKind payload_type) {
    Type t;
    t.kind = TY_EXCEPTION;
    t.copy_kind = CK_MOVE;  /* Exceptions are move-only (one-shot) */
    t.n_lifetimes = 0;
    t.as.exn.payload_type = payload_type;
    return t;
}

/* Phase 18: Continuation type constructor */
/* Create a continuation type cont<T> that returns T */
/* CC4 continuation flavor: which runtime (k v) resumes against. */
typedef enum { CONT_CLONEABLE = 0, CONT_ESCAPE = 1, CONT_SERIAL = 2 } ContFlavor;

static inline Type type_cont(TypeKind returns) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_CONT;
    t.copy_kind = CK_MOVE;  /* Continuations are move-only (one-shot) */
    t.n_lifetimes = 0;
    t.as.cont.returns = returns;
    t.as.cont.flavor = CONT_CLONEABLE;
    t.hkt_kind = KIND_STAR;
    return t;
}

/* CC4: a continuation typed with an explicit flavor (escape/cloneable/serial). */
static inline Type type_cont_flavored(TypeKind returns, ContFlavor flavor) {
    Type t = type_cont(returns);
    t.as.cont.flavor = flavor;
    return t;
}

/* Map a surface type name to a continuation flavor, or -1 if not a cont name. */
static inline int cont_flavor_from_name(const char *n) {
    if (strcmp(n, "cont") == 0)        return CONT_CLONEABLE;
    if (strcmp(n, "escape-cont") == 0) return CONT_ESCAPE;
    if (strcmp(n, "serial-cont") == 0) return CONT_SERIAL;
    return -1;
}

/* Phase B2: Cloneable continuation type constructor */
/* Create a cloneable continuation type cloneable_cont<T> that returns T */
static inline Type type_cloneable_cont(TypeKind returns) {
    Type t;
    t.kind = TY_CLONEABLE_CONT;
    t.copy_kind = CK_COPY;  /* Cloneable continuations can be copied (multi-shot) */
    t.n_lifetimes = 0;
    t.as.cont.returns = returns;
    return t;
}

/* Phase 11: Struct type constructor */
/* Create a TY_STRUCT type referencing the given StructDef.
 * copy_kind is CK_LINEAR for :linear structs, CK_COPY for :copy, CK_MOVE otherwise.
 * An :affine struct is CK_UNIQUE (at-most-one alias) and carries SK_AFFINE on the
 * substructural axis so it may be silently dropped but never duplicated. */
static inline Type type_struct(StructDef *def) {
    Type t = {0};
    t.kind = TY_STRUCT;
    t.copy_kind = def->is_linear ? CK_LINEAR
                : def->is_affine ? CK_UNIQUE
                : (def->is_copy ? CK_COPY : CK_MOVE);
    if (def->is_affine) t.substruct = SK_AFFINE;
    t.hkt_kind = KIND_STAR;
    t.as.struct_.def = def;
    return t;
}

/* Phase G0: ADT type constructor */
/* Create a TY_ADT type referencing the given AdtDef.
 * copy_kind is taken from def->is_copy. */
static inline Type type_adt(AdtDef *def) {
    Type t = {0};
    t.kind = TY_ADT;
    t.copy_kind = def->is_copy ? CK_COPY : CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.adt_.def = def;
    return t;
}

/* Phase HRT/G2: Create a named type variable type for parameters like :a */
static inline Type type_tyvar_named(const char *name) {
    Type t = { .kind = TY_TYVAR, .copy_kind = CK_COPY };
    t.as.tyvar_.name = name;
    return t;
}

/* CT0: Contract type constructor — { x : T | p } */
static inline Type type_contract(Arena *a, struct Type *base, const char *var, const struct Form *pred) {
    (void)a; /* base is already arena-allocated at call sites */
    Type t = {0};
    t.kind = TY_CONTRACT;
    t.copy_kind = base ? base->copy_kind : CK_COPY;
    t.hkt_kind = KIND_STAR;
    t.as.contract_.base_type = base;
    t.as.contract_.var_name = var;
    t.as.contract_.predicate = pred;
    return t;
}

/* ET3: Handler type constructor */
static inline Type type_make_handler(const char *effect_name, TypeKind value_kind, TypeKind result_kind) {
    Type t = {0};
    t.kind = TY_HANDLER;
    t.copy_kind = CK_COPY;
    t.hkt_kind = KIND_STAR;
    t.as.handler_.effect_name = effect_name;
    t.as.handler_.value_kind = value_kind;
    t.as.handler_.result_kind = result_kind;
    return t;
}

/* SS5: Global protocol type constructor (compile-time only, no runtime representation). */
static inline Type type_global(const char *name, const char **roles, int n_roles,
                               GlobalInteraction *body) {
    Type t = {0};
    t.kind = TY_GLOBAL;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.global_.name = name;
    t.as.global_.roles = roles;
    t.as.global_.n_roles = n_roles;
    t.as.global_.body = body;
    return t;
}

/* SS5: Role endpoint type constructor -- linear channel endpoint in a global protocol. */
static inline Type type_role(struct Type *global_type, const char *role_name,
                             GlobalInteraction *current_step) {
    Type t = {0};
    t.kind = TY_ROLE;
    t.copy_kind = CK_LINEAR;
    t.substruct = SK_LINEAR;
    t.hkt_kind = KIND_STAR;
    t.as.role_.global_type = global_type;
    t.as.role_.role_name = role_name;
    t.as.role_.current_step = current_step;
    return t;
}

int          type_eq(Type a, Type b);
/* ET3-D: Subtype check. Returns true if sub is a subtype of super_. */
bool         type_is_subtype(Type sub, Type super_);
/* LT2: Check arg_linear compatibility between two function types.
 * Returns 1 if compatible (all arg_linear flags match), 0 on mismatch.
 * Non-function types always return 1. */
int          fn_type_subtype(Type actual, Type expected);
const char  *type_name(Type t);                   /* "int", "bool", … */
const char  *type_c_name(Type t);                 /* "int64_t", "bool", … */
void         type_codegen_reset_struct_apps(void);
void         type_codegen_emit_struct_apps(Buf *out);
/* Direction (1) of polymorphic-ok-in-typeclass-instance-method...md:
 * exposed so emit_expr.c can resolve a parametric struct field's actual
 * type at the field-access site (the field-expr's Type may still be a
 * TY_TYVAR after parse; the receiver's type-arg list is the substitution). */
bool         type_extract_struct_app(const Type *t, struct StructDef **out_def,
                                     Type *out_args, uint8_t *out_n);
Type         substitute_struct_app_type(const Type *t,
                                        const struct StructDef *def,
                                        const Type *args);
/* TS4P1: ADT-app (polymorphic ADT monomorphisation) registry. */
void         type_codegen_reset_adt_apps(void);
void         type_codegen_emit_adt_apps(Buf *out);
const char  *type_register_adt_app(Type t);
char        *type_adt_app_ctor_suffix(Type t);
/* Phase E: Typed function-pointer typedef registry for unboxed fn struct fields. */
const char  *register_fn_ptr_typedef(const Type *fn_type);
void         type_codegen_reset_fn_ptr_typedefs(void);
void         type_codegen_emit_fn_ptr_typedefs(Buf *out);
/* Phase D: true if t is a struct type whose sizeof exceeds 16 bytes,
 * meaning it should be passed as const T* rather than by value. */
bool         type_struct_pass_by_ptr(Type t);
/* end-to-end-monomorphization: true when t is a (possibly applied) :heap struct
 * -- its monomorphic ABI is a typed pointer `T__A *`. */
bool         type_is_heap_struct(Type t);
/* end-to-end-monomorphization: true when t is a :heap struct with a concrete
 * monomorphic layout (typed-pointer ABI `T__int *`); false for abstract-element
 * heap types that flow as the int64 carrier. */
bool         type_is_concrete_heap_struct(Type t);
/* end-to-end-monomorphization: the by-value struct C name (`Vec__int`) for a
 * struct/struct-app, WITHOUT the trailing " *" the heap pointer lowering adds. */
const char  *type_struct_value_c_name(Type t);
/* SC7 (carrier-duality): true for a parametric struct with a single `:int`
 * field (e.g. `(defstruct Schema [A] (raw :int))`).  Such a phantom wrapper is
 * a transparent newtype over int64 -- one C representation everywhere, so HKT
 * dispatch can chain it (.fmap (.fmap s g) h) without rep-mixing. */
bool         type_is_transparent_int_newtype(Type t);
/* Phase HRT0: compute the rank of a type (0 = monotype, 1 = rank-1, ≥2 = higher-ranked) */
int          type_rank(const Type *t);

/* Phase HKT-P2: One-step unrolling of a TY_REC type.
 * Returns t->as.rec.body (the body with the recursive variable bound).
 * In v1, the body is not substituted; returns the raw body pointer.
 * Returns NULL if t is not TY_REC or body is not yet evaluated. */
Type *type_rec_unfold(Type *t);
void         type_print(Buf *b, Type t);

/* Phase HKT H0: Kind utilities */
bool         kind_eq(Kind a, Kind b);             /* true if a == b */
const char  *kind_to_string(Kind k);              /* "*", "* -> *", … */
Kind         kind_parse(const char *s);           /* parse "*" / "* -> *"; default KIND_STAR */

/* Phase TP3: kind helpers for n-ary type constructors (arbitrary arity).
 * kind_for_arity(n) returns (Kind)n -- no cap, any n is valid.
 *   0 -> KIND_STAR, 1 -> KIND_ARROW, 2 -> KIND_ARROW2, ... N -> arity-N kind. */
Kind         kind_for_arity(uint32_t n);
/* kind_apply_one(k) returns the kind after applying one type argument:
 * (Kind)(k-1) for any arrow kind (k >= 1, k != KIND_ROW).
 * KIND_STAR and KIND_ROW are returned unchanged (caller validates). */
Kind         kind_apply_one(Kind k);

/* Phase HKT-P2: Recursive type guards */
bool         type_is_guarded_recursive(Type t, const char *rec_name);

/* Phase 11: Copy/Move trait helpers */
/* Returns true if the type is Copy (bitwise duplication allowed) */
static inline bool type_is_copy(Type t) {
    return t.copy_kind == CK_COPY;
}

/* Returns true if the type is Move-only (ownership transfer) */
static inline bool type_is_move(Type t) {
    return t.copy_kind == CK_MOVE;
}

/* Returns true if the type is Unsized (size unknown at compile time) */
static inline bool type_is_unsized(Type t) {
    return t.copy_kind == CK_UNSIZED;
}

#endif
