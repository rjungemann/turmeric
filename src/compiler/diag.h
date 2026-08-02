#ifndef TUR_DIAG_H
#define TUR_DIAG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "compiler/forms.h"

/* Forward declaration for Buf (defined in buf.h) */
struct Buf;

/* Error codes for diagnostics (Phase 8: diagnostics polish) */
typedef enum DiagCode {
    DIAG_CODE_NONE = 0,
    /* Type errors */
    TUR_E0001_TYPE_MISMATCH,
    TUR_E0002_ARITY_MISMATCH,
    TUR_E0003_UNBOUND_SYMBOL,
    /* Scope errors */
    TUR_E0004_INVALID_SCOPE,
    TUR_E0005_USE_AFTER_MOVE,
    /* Operator errors */
    TUR_E0006_OPERATOR_LOOKUP_FAILED,
    /* Capture errors */
    TUR_E0007_CAPTURE_ERROR,
    /* Effect-row mismatch (P19-2) */
    TUR_E0009_EFFECT_ROW_MISMATCH,
    /* Thread safety (T19-B) */
    TUR_E0010_NOT_SEND,   /* type cannot be sent across thread boundaries */
    TUR_E0011_NOT_SYNC,   /* type cannot be shared across thread boundaries */
    /* Kind mismatch (Phase HKT H1) */
    TUR_E0012_KIND_MISMATCH, /* type constructor kind does not match expected kind */
    /* Orphan instance (Phase HKT H4) */
    TUR_E0013_ORPHAN_INSTANCE, /* typeclass instance defined outside typeclass/type origin file */
    /* Parameterized typeclass constraints (Phase PTC2) */
    TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED, /* constraint cannot be satisfied for type */
    /* Phase B2: Cloneable continuation checks (CPS-CL7) */
    TUR_E0014_NOT_CLONE,                          /* captured binding does not implement Clone */
    TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,      /* cloneable-shift used outside cloneable-reset */
    /* Phase T25: Continuation escape into async scope */
    TUR_E0017_CONT_ESCAPE_ASYNC,                  /* effect handler continuation captured by async block */
    /* Phase 21: Serializable continuations */
    TUR_E0018_NOT_SERIALIZABLE,                   /* captured binding does not implement Serializable */
    TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,         /* serial-shift used outside serial-reset boundary */
    /* Phase D0: ambiguous typeclass method dispatch (erased receiver type) */
    TUR_E0020_AMBIGUOUS_DISPATCH,                 /* .method on int64_t receiver matches multiple instances */
    /* ER5: module effect visibility (PR5-1) */
    TUR_E0021_PRIVATE_EFFECT,                     /* effect is private to its defining module */
    /* CF6 (control-flow-completeness-plan): async Send-across-await soundness */
    TUR_E0022_AWAIT_LIVE_NOT_SEND,                /* non-Send binding in scope at await in async body */
    /* Phase B: mixed-width numeric arithmetic (no implicit coercion) */
    TUR_E0042_MIXED_WIDTH_ARITH, /* distinct numeric kinds cannot be combined without (as ...) */
    /* ER1: strict-effects warnings */
    TUR_W0030_STRICT_EFFECTS_UNANNOTATED,  /* unannotated fn has non-empty inferred row (--strict-effects) */
    TUR_W0031_EFFECT_OVER_ANNOTATED,       /* declared effect never performed */
    TUR_W0032_ROW_VAR_ALWAYS_CONCRETE,    /* row variable is always concrete; suggest replacing with concrete row */
    TUR_W0033_UNREACHABLE_HANDLER,        /* handler clause for Foo is unreachable -- body never performs Foo */
    /* ET2: effect polymorphism warnings */
    TUR_W0034_ROW_VAR_GENERALISED,        /* row variable auto-generalised; consider explicit forall [e] (--strict-effects) */
    /* GATE / ET2: effect row type errors */
    TUR_E0254_INFINITE_EFFECT_ROW,     /* occurs check: binding effect row variable would produce an infinite row */
    /* SZ7 (sized-types-completion-plan): static size checking (-Xsized-types) */
    TUR_E0260_SIZED_TYPE_MISMATCH,     /* two statically-known size indices are not equal/compatible */
    /* ET3: handler typing errors */
    TUR_E0251_HANDLER_OVERLAP,            /* composed handlers handle overlapping effects */
    TUR_E0252_HANDLER_RESULT_MISMATCH,   /* handler clause result type does not match handle expression type */
    /* ET4: effect scope errors */
    TUR_E0250_ROW_VAR_ESCAPES_SCOPE,    /* forall [e] row variable used outside its quantifier scope */
    TUR_E0253_EFFECT_NOT_IN_SCOPE,      /* perform site uses an effect not declared or in scope */
    /* LT1: Linear type errors (-Xlinear) */
    TUR_E0100_LINEAR_DROPPED,      /* linear value dropped without being consumed */
    TUR_E0101_LINEAR_USE_AFTER_CONSUME, /* linear value used after being moved/consumed */
    TUR_E0102_LINEAR_COPY,         /* cannot copy a linear value */
    TUR_E0103_LINEAR_IN_RC,        /* cannot wrap a linear value in rc<T> */
    TUR_E0104_LINEAR_BRANCH_MISMATCH, /* linear value consumed in one branch but not another */
    /* TY4: Lifetime / borrow-escape errors */
    TUR_E0105_BORROW_ESCAPES_SCOPE,   /* a borrow outlives the value it points to */
    TUR_E0106_CYCLIC_LIFETIME,        /* lifetime outlives-constraints form a cycle */
    TUR_E0107_CAPTURED_FIELD_CONSUMED_IN_HANDLER, /* handler case drops an owning field of a captured by-value local (double-drop vs scope auto-drop) */
    /* UT1: Uniqueness type errors (-Xunique-types) */
    TUR_E0200_UNIQUE_ALIASED,      /* value is not unique -- aliased by another binding */
    TUR_E0201_UNIQUE_COPY,         /* cannot copy a unique value (use after consume) */
    TUR_E0202_UNIQUE_IN_RC,        /* cannot wrap a unique value in rc<T> */
    /* ST0: Substructural type errors (-Xsubstructural) */
    TUR_E0150_AFFINE_USED_TWICE,   /* affine value used more than once */
    TUR_E0151_RELEVANT_DROPPED,    /* relevant value dropped without being used */
    /* CONV-S6: product-shape (struct/record-variant) construction errors */
    TUR_E0292_MISSING_FIELD,    /* construction omits a required field */
    TUR_E0293_DUPLICATE_FIELD,  /* construction names the same field twice */
    TUR_E0294_UNKNOWN_FIELD,    /* construction names a field the type lacks */
    TUR_E0296_WITH_NOT_COPY,    /* `with` used on a move-only (non-:copy) type */
    TUR_E0297_WITH_UNKNOWN_FIELD, /* `with` override names a field the type lacks */
    TUR_E0298_WITH_DUPLICATE_FIELD, /* `with` overrides the same field twice */
    TUR_E0299_MIXED_POS_KW,     /* construction mixes positional and keyword args */
    /* GAP 3 (byvalue-adt-int-cast-plan): `::` between a by-value aggregate
     * (non-recursive ADT/struct product) and a one-word carrier (:int/:ptr<void>)
     * has no sound lowering -- the aggregate has no int64 handle to reinterpret. */
    TUR_E0295_BYVALUE_CARRIER_CAST,
    /* IT1: Union type errors (-Xunion-types) */
    TUR_E0300_UNION_TYPE_MISMATCH,   /* value type not a member of union type */
    TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH, /* match on union type missing arm for one or more members */
    /* sealed-opaque experiment: `::` between a `:sealed` defopaque and its
     * representation type, outside the module that declared it. */
    TUR_E0302_SEALED_OPAQUE_CAST,
    /* IT3: Intersection type errors (-Xintersection-types) */
    TUR_E0350_INTERSECTION_UNSATISFIABLE,   /* no value can satisfy all intersection members */
    TUR_E0351_INTERSECTION_MEMBER_MISMATCH, /* value doesn't satisfy an intersection member */
    /* U6: inline-C outside Unsafe annotation */
    TUR_W0036_INLINE_C_MISSING_UNSAFE, /* inline-C block in function not annotated #{Unsafe} */
    /* Phase C: narrow-width param in inline-C body */
    TUR_W0037_INLINE_C_NARROW_PARAM,   /* defn param has narrow numeric type in inline-C body */
    /* Phase R6b: --lint-panic panic call site outside the allow-list */
    TUR_W0038_LINT_PANIC_SITE,
    /* A free top-level defn shares its name with a user-defined typeclass
     * method, silently shadowing the method at every bare call site.
     * See docs/reported/typeclass-methods-share-value-namespace-with-defns.md. */
    TUR_W0039_METHOD_DEFN_CLASH,
    /* Eval-mode unknown call head deferred to runtime-dispatch -- the name is
     * not bound at elaboration time and not in the typed-native registry, so
     * it will fail at runtime if no native is registered for it before the
     * call runs.  Likely a typo.  See
     * docs/archive/eval-mode-unknown-call-deferred-to-runtime.md. */
    TUR_W0040_EVAL_UNKNOWN_CALL_RUNTIME_DISPATCH,
    /* arbitrary-fn-arity Phase 6: a defn/fn declares more positional parameters
     * than the historical soft ceiling of 16.  Not an error -- the mechanical
     * bound is now MAX_FN_ARITY (64) -- but a lint nudge toward the arity style
     * guide (a defstruct options value or a `& rest :type` variadic). */
    TUR_W0041_HIGH_ARITY,
    /* A `defn`/`defmacro` names a reserved special form (`return`, `match`,
     * `handle`, ...).  Head-position dispatch in elab_call matches those names
     * by symbol identity *before* any binding or macro lookup, so the
     * definition is accepted but every bare `(name ...)` call site elaborates
     * as the special form and the definition is unreachable by its bare name.
     * See docs/archive/defn-shadows-return-special-form.md. */
    TUR_W0042_SHADOWS_SPECIAL_FORM,
    /* MS2: Multi-shot continuation capture analysis */
    TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,       /* ^multishot handler captures a unique/linear value */
    TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,  /* ^multishot annotation outside a handler continuation */
    TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC,     /* resume of ^multishot k inside atomically block */
    /* CT0: Contract type errors */
    TUR_E0400_CONTRACT_VIOLATED,   /* contract check failed: predicate is false */
    TUR_E0401_POSTCOND_VIOLATED,   /* postcondition failed: predicate is false for result */
    /* SS0b-SS1: Session type errors (-Xsessions) */
    TUR_E0210_SESSION_NOT_DUAL,         /* session endpoints are not dual protocols */
    TUR_E0211_SESSION_DROPPED,          /* session channel dropped before protocol completion */
    TUR_E0212_SESSION_PROTO_MISMATCH,   /* channel operation not valid for current session protocol */
    /* SS5: Global protocol type errors (-Xsessions) */
    TUR_E0220_GLOBAL_NOT_PROJECTABLE,   /* global protocol G is not projectable onto role R at step */
    TUR_E0221_ROLE_NOT_DECLARED,        /* role R is not declared in global protocol G */
    TUR_E0222_ROLE_IMPL_MISMATCH,       /* role R impl does not match projected local type */
    TUR_E0223_GLOBAL_NOT_WELLFORMED,    /* global protocol G is not well-formed: reason */
    /* DV0-DV1: Dynamic var errors (-Xdynamic-vars) */
    TUR_E0600_DYNVAR_SET_NOT_DYNAMIC,    /* set! or binding target is not a dynamic var */
    TUR_E0601_DYNVAR_SET_NO_BINDING,     /* set! on dynamic var with no active binding frame */
    TUR_E0602_DYNVAR_TYPE_MISMATCH,      /* override value type does not match defdynamic type */
    TUR_E0603_DYNVAR_SUBSTRUCTURAL_TYPE, /* dynamic var declared with a substructural type */
    TUR_E0604_DYNVAR_NOT_TOPLEVEL,       /* defdynamic used outside module toplevel */
    TUR_E0605_DYNVAR_SET_IN_ATOMIC,      /* set! on dynamic var inside an atomically block */
    /* DV0: Dynamic var naming warning */
    TUR_W0600_DYNVAR_NO_EARMUFFS,        /* defdynamic name does not use *earmuffs* convention */
    /* CF3/CF4 (control-flow-completeness-plan): gated / unsupported control-flow.
     * E07xx band reserved in Phase CF0; see docs/control-flow-completeness-plan.md. */
    TUR_E0700_CALLCC_GATED,              /* RETIRED (call-cc-completion CC5): call/cc is now real + ungated on the CPS substrate. Code reserved, no longer emitted. */
    TUR_E0701_ESCAPE_GATED,             /* RETIRED (call-cc-completion CC5): escape is now real + ungated on the CPS substrate. Code reserved, no longer emitted. */
    /* CF5 (control-flow-completeness-plan): always-on generator limitation diagnostics */
    TUR_E0702_YIELD_IN_MATCH_ARM,        /* yield/yield* inside a match arm (1.0 limitation) */
    TUR_E0703_YIELD_IN_RECURSIVE_GEN,    /* yield/yield* inside a recursive generator (1.0 limitation) */
    TUR_E0704_HANDLER_COMPOSE_UNIMPL,    /* first-class handler composition (compose-handlers) not yet implemented (gated) */
    /* poly-defn-shares-inner-closure-body-across-monomorphizations: a generic
     * defn that returns an (fn ...) whose result type is one of the defn's type
     * parameters emits a single shared inner closure body (integer thunk ABI);
     * a floating-point specialization dispatches through the wrong register and
     * silently miscompiles.  Rejected until per-A inner-body specialization lands. */
    TUR_E0705_POLY_CLOSURE_RESULT_TYVAR,
    /* serial-shift-unsupported-context-miscompile: a serial-shift whose
     * delimited context falls outside the DK-lowering grammar (collect_ctx)
     * cannot be reified into a marshalable continuation.  Rejected at codegen
     * instead of silently lowering to a 0 placeholder / __builtin_trap(). */
    TUR_E0706_SERIAL_CONTEXT_NOT_CAPTURABLE,
    /* float-register-class-returns: a function/instance-method whose declared
     * return type and body land in DIFFERENT register classes -- a float
     * (xmm) on one side and a concrete non-float (int64 GP register: int,
     * cstr, bool, opaque/struct/ADT handle) on the other.  Unlike the
     * same-register-class carrier bridges the ABI deliberately tolerates,
     * a float-vs-non-float result is a genuine xmm0-vs-rax miscompile. */
    TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
    /* pointer-vs-scalar-returns: a function/instance-method whose declared
     * return type is concretely `cstr` (a `const char*`) but whose body yields
     * a concrete integer-family scalar (int / bool / intN / uintN).  cstr and
     * the integer family all ride the int64 GP register, so the carrier ABI
     * cannot see the swap -- but a bare integer is never a valid string
     * pointer, so committing to `cstr` and returning an integer is a genuine
     * type-erasure bug, not a tolerable carrier bridge.  Only this commit
     * direction is rejected; the reverse (an integer carrier returning a cstr
     * handle) stays a legitimate bridge. */
    TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
    /* carrier-aware-return-unification Phase 2: a genuinely COMMITTED function
     * (a monomorphic, non-`#{Unsafe}` `defn` -- one that does not participate in
     * the int64 carrier ABI) declares a concrete integer-family return but its
     * body yields a concrete `cstr` (a `const char*` string pointer).  This is
     * the REVERSE of the TUR-E0708 commit direction: a string pointer is never a
     * valid integer.  It is tolerated for generic / `#{Unsafe}` / typeclass code
     * (the carrier-handle bridge), but in a committed monomorphic function there
     * is no carrier to bridge, so it is a real type-erasure bug. */
    TUR_E0709_RETURN_TYPE_MISMATCH,
    /* cloneable-shift-unsupported-context-miscompile (D6a): a cloneable-shift
     * whose delimited context falls outside the native build_cloneable grammar
     * cannot be reified into a multi-shot continuation.  The legacy setjmp
     * fallback silently dropped the context (lowered the continuation as the
     * identity), so `(+ (compute) (cloneable-shift ...))` printed a wrong number.
     * Rejected at codegen instead, mirroring the serial TUR-E0706 fix. */
    TUR_E0710_CLONEABLE_CONTEXT_NOT_CAPTURABLE,
    /* Deprecation band (TUR-D####): syntax accepted for backward
     * compatibility but slated for removal.  Emitted as DIAG_WARNING;
     * promoted to DIAG_ERROR under --Werror=deprecated. */
    /* fn-type-bare-identifier-plan Phase 3: a leading colon on a type
     * inside a (fn ...) type expression is redundant -- position alone
     * marks the param/result slots as types.  Drop the colon:
     * (fn [:int] :int) -> (fn [int] int). */
    TUR_D0001_FN_TYPE_COLON,
    /* fx-row-syntax-rename-plan Phase 2: bare `#{...}` as an effect row is
     * deprecated; prefer `#fx{...}`.  Emitted from elaboration when an
     * F_MAP whose provenance is PROV_FX_LEGACY is consumed as an effect row. */
    TUR_D0002_FX_ROW_LEGACY_HASH,
    /* fx-row-syntax-rename-plan Phase 2: `@{...}` as an effect row is
     * deprecated; prefer `#fx{...}`.  Emitted from elaboration when an
     * F_MAP whose provenance is PROV_FX_AT_LEGACY is consumed as an
     * effect row.  Note: this does not affect bare `@x` deref sugar. */
    TUR_D0003_FX_ROW_LEGACY_AT,
    /* XF (experimental-flag-mechanism-plan): the `--enable=<name>` surface.
     * E0310 fires at CLI/manifest parse on an unknown experiment name;
     * W0060/W0061 fire once per compile at the first use site of an enabled
     * prototype/beta experiment.  W0060/W0061 are emitted directly to stderr
     * by experiment_warn_if_used (mirroring TUR-W0050), not through diag_emit;
     * they are registered here only so `tur explain` can describe them. */
    TUR_E0310_UNKNOWN_EXPERIMENT,
    TUR_W0060_EXPERIMENTAL_PROTOTYPE,
    TUR_W0061_EXPERIMENTAL_BETA,
    /* RT3 (refinement-types-plan): static discharge of `#refine{...}`
     * predicates.  Emitted unconditionally since refinement types graduated in
     * v0.33.0; there is no longer an experiment gate.  E0371/W0372 are the two
     * verdict-carrying codes: the runtime contract check survives in both
     * cases, so neither is a miscompile -- W0372 is "we could not prove it",
     * E0371 is "we found a counterexample".  Under --strict-refine both are
     * hard errors. */
    TUR_E0370_REFINE_ILL_TYPED,   /* refinement predicate is ill-typed */
    TUR_E0371_REFINE_NOT_PROVED,  /* counterexample found; predicate is not entailed */
    TUR_W0372_REFINE_UNKNOWN,     /* no backend decided it; runtime check kept */
    TUR_W0373_REFINE_NONLINEAR,   /* nonlinear subterm treated as uninterpreted */
    TUR_E0374_REFINE_INSTANCE_STRONGER, /* instance method demands more than its class signature */
    TUR_E0375_REFINE_EFFECTFUL,   /* refinement predicate mentions effects */
    TUR_E0376_REFINE_TYPE_PARAM,  /* refinement on a type parameter (unsupported) */
    TUR_W0377_REFINE_INSTANCE_LENIENCY, /* call allowed only because the resolved
                                         * instance demands less than its class */
    TUR_E0378_REFINE_IN_FN_TYPE,  /* refinement written inside a (fn ...) type */
    TUR_I0379_REFINE_ORACLE_MISMATCH, /* RETIRED (Z3 oracle retirement, 0.32.5): the
                                       * dev-only oracle whose disagreements this
                                       * reported is gone. Code reserved, no longer
                                       * emitted. */
    /* A refinement written in TYPE-ARGUMENT position -- `(Box #refine{...})`.
     * The contract is peeled to its base type so the payload behaves like the
     * ordinary value it is; the predicate is NOT enforced on the payload.
     * Warned rather than dropped silently: an annotation that quietly does
     * nothing is how a reader ends up believing a container's contents are
     * checked when they are not.  See
     * docs/archive/contract-type-arg-not-peeled-to-base.md. */
    TUR_W0380_REFINE_TYPE_ARG_UNENFORCED,
    /* WF1/WF2 (checked-write-frames-plan): the `#writes` write-frame annotation.
     * E0381 -- the annotation itself is malformed, or names something that is
     *          not a parameter of this function.  A frame that does not resolve
     *          cannot be checked against anything, so it is an error rather
     *          than a silently ignored decoration.
     * E0382 -- the body WRITES outside the frame it declared.  This is the WF2
     *          checked tier: a declared frame the body exceeds is an error, not
     *          a silent widening, for the same reason `#reads` is -- downstream
     *          code is entitled to believe the declaration. */
    TUR_E0381_WRITES_FRAME_INVALID,
    TUR_E0382_WRITES_FRAME_EXCEEDED,
    /* exports-map-syntax-tighten-plan: `:exports` in build.tur got an
     * effect-row literal (`#fx{...}` or `@{...}`) instead of a map literal
     * (`#map{...}`) or a legacy bare `#{...}` map or a path vector. */
    TUR_E0620_EXPORTS_FX_ROW,
    /* `:tur-version` in build.tur (no-compiler-version-constraint-in-manifest):
     * E0621 -- the running compiler is BELOW the declared floor, so the spice's
     *          source genuinely will not work.  Hard error.
     * E0622 -- the range itself is malformed; a typo must not silently become a
     *          different constraint.  Hard error.
     * W0623 -- the running compiler is ABOVE the declared ceiling.  Only means
     *          "never tested against this compiler", which is usually fine, so
     *          it warns: a hard ceiling would make every release break every
     *          spice until each author bumped a number. */
    TUR_E0621_TUR_VERSION_BELOW_FLOOR,
    TUR_E0622_TUR_VERSION_MALFORMED,
    TUR_W0623_TUR_VERSION_ABOVE_CEILING,
} DiagCode;

typedef enum DiagLevel {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE,
    DIAG_HELP,
} DiagLevel;

#define DIAG_CONTEXT_LINES 2  /* Number of context lines before/after the error line */

/* Reader types for #lang dispatch (Phase S1) */
typedef enum ReaderType {
    READER_UNKNOWN = -1,   /* Unknown/invalid #lang directive */
    READER_TURMERIC,       /* Default s-expression reader */
    READER_CURLY_INFIX,    /* Turmeric + curly-infix (SRFI-105) */
    READER_NEOTERIC,       /* Turmeric + neoteric notation */
    READER_SWEET,          /* Full sweet-expressions */
} ReaderType;

/* The additive `#lang` layer set: a bitset over the LANG_LAYERS[] table
 * (src/compiler/lang_layers.c), one bit per table index.  Rides alongside
 * the base ReaderType, not in place of it.  Empty (0) for a bare file or a
 * `#lang` line with no trailing layer tokens.  See lang_layers.h. */
typedef uint32_t LangLayerSet;

/* Source map for syntax-transforming readers (currently sweet-exp).
 * Each run says "starting at xform_offset in the transformed text,
 * `length` bytes were copied verbatim from orig_offset of the original
 * source."  The runs are kept sorted by xform_offset; bytes that fall
 * in the gaps between runs are reader-inserted (`(`, `)`, etc.) and
 * have no original counterpart. */
typedef struct SweetMapRun {
    uint32_t xform_offset;
    uint32_t orig_offset;
    uint32_t length;
} SweetMapRun;

typedef struct SweetMap {
    SweetMapRun *runs;
    size_t       n_runs;
    size_t       cap_runs;
} SweetMap;

/* Translate a byte offset in the transformed text to the corresponding
 * byte offset in the original source.  When the input offset falls in
 * an inserted gap it returns the end of the preceding run (the closest
 * real position).  Safe to call with map == NULL — returns xform_off. */
size_t sweet_map_translate_offset(const SweetMap *map, size_t xform_off);

typedef struct SourceFile {
    const char *path;
    /* Directory to resolve in-source relative paths (e.g. the
     * `#use-reader-macros "..."` directive) against, used when `path` itself
     * carries no directory component -- notably the `--interpret`/eval blob
     * whose path is the synthetic "<eval>".  NULL means fall back to dirname
     * of `path` (the normal compiled-file case). */
    const char *base_dir;
    const char *src;     /* full source text (transformed if xform_map set) */
    size_t      len;
    uint16_t    file_id;
    ReaderType  reader_type;  /* Phase S1: for enabling syntax features */
    /* Additive `#lang` layer set parsed from the same directive line as
     * reader_type (lang-layers-plan).  Reader layers in this set have their
     * `#`-dispatch registered at reader init; empty for files without layers.
     * A SourceFile built with `{0}`/memset starts with no layers. */
    LangLayerSet lang_layers;
    /* Sweet-exp transformation support: when xform_map is non-NULL, src
     * is the preprocessed s-expression text and orig_src/orig_len point
     * to the user's original source.  Diagnostics render snippets from
     * orig_src and translate span offsets via xform_map. */
    const char     *orig_src;
    size_t          orig_len;
    const SweetMap *xform_map;
} SourceFile;

/* Detect #lang directive from file source (Phase S0).  Base reader only;
 * any trailing layer tokens are consumed (never leaked into the body) but
 * not reported.  A thin wrapper over detect_lang_layered. */
ReaderType detect_lang(const char *src, size_t len, const char **out_rest,
                       size_t *out_rest_len);

/* Detect #lang directive, reporting both the base ReaderType and the
 * additive layer set (lang-layers-plan L0).  `out_layers` receives the set of
 * recognized layer tokens; when a trailing token is NOT a registered layer,
 * `*out_bad`/`*out_bad_len` point at the first offending token (into `src`)
 * and it is omitted from the set -- the caller reports TUR-E0330.  Any of the
 * out-params may be NULL; passing NULL for `out_layers` makes this behave like
 * detect_lang (layers parsed for EOL-consumption but discarded). */
ReaderType detect_lang_layered(const char *src, size_t len,
                               const char **out_rest, size_t *out_rest_len,
                               LangLayerSet *out_layers,
                               const char **out_bad, size_t *out_bad_len);

/* Get reader type from file extension (Phase S0) */
ReaderType reader_type_from_extension(const char *path);

/* Get reader type name as string (Phase S0) */
const char *reader_type_name(ReaderType type);

/* Check if a reader type is implemented (Phase S0) */
bool reader_type_is_implemented(ReaderType type);

/* Underline style for diagnostics */
typedef enum UnderlineStyle {
    UNDERLINE_PRIMARY,   /* ^^^ for primary span */
    UNDERLINE_SECONDARY, /* ~~~ for secondary/related spans */
    UNDERLINE_GAP,       /* - for gaps between spans */
} UnderlineStyle;

/* A diagnostic note with its own span */
typedef struct DiagNote {
    DiagLevel level;
    Span span;
    const char *message;
} DiagNote;

/* A suggestion with optional replacement text */
typedef struct DiagSuggestion {
    const char *text;            /* Suggested fix text */
    const char *replacement;     /* Optional: text to replace with */
    const char *doc_url;        /* Optional: documentation URL */
} DiagSuggestion;

/* Snippet rendering options */
typedef struct SnippetOpts {
    bool show_line_numbers;
    uint32_t context_lines;    /* lines of context before/after */
    UnderlineStyle primary_style;
    UnderlineStyle secondary_style;
} SnippetOpts;

/* Default snippet options */
#define SNIPPET_OPTS_DEFAULT ((SnippetOpts){.show_line_numbers = true, .context_lines = DIAG_CONTEXT_LINES, .primary_style = UNDERLINE_PRIMARY, .secondary_style = UNDERLINE_SECONDARY})

/* Initialize diagnostics - call once at program start */
void diag_init(bool use_color);

/* Check if colors are enabled */
bool diag_use_color(void);

/* Check if stderr is a TTY (for auto-color) */
bool stderr_is_tty(void);

void diag_register_file(const SourceFile *file);

/* Return the filesystem path registered for file_id, or NULL. */
const char *diag_file_path(uint16_t file_id);
const SourceFile *diag_source_file(uint16_t file_id);

/* Core diagnostic emission */
void diag_emit(DiagLevel level, Span span, const char *fmt, ...);
void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap);

/* Enhanced diagnostics with code and notes (Phase 8) */
void diag_emit_with_code(DiagLevel level, Span span, DiagCode code, const char *fmt, ...);
void diag_emit_with_notes(DiagLevel level, Span span, const char *message,
                          DiagNote *notes, size_t note_count);
void diag_emit_with_suggestion(DiagLevel level, Span span, const char *message,
                               const DiagSuggestion *suggestion);

/* Multi-span diagnostics for complex errors */
void diag_emit_multi_span(DiagLevel level, const char *message,
                         Span primary_span, const char *primary_label,
                         Span *secondary_spans, const char **secondary_labels,
                         size_t secondary_count);

bool diag_had_error(void);
void diag_reset(void);

/* Speculative-elaboration capture (bare-fat-result-monomorphization-plan).
 * While a capture frame is active, every diag_emit* call is suppressed
 * (nothing is rendered to stderr) and DIAG_ERROR emissions are counted into
 * the innermost frame.  diag_pop_capture() restores had_error_ to its value
 * at the matching push and returns how many errors were suppressed in the
 * frame.  This lets the elaborator *try* to elaborate a bare-^fat body at the
 * default int result kind, detect that it does not typecheck, and defer it to
 * per-call-site specialization -- without leaking spurious diagnostics. Frames
 * nest (bounded depth); a caller that wants the real diagnostics simply
 * re-runs the elaboration with no capture frame active. */
void     diag_push_capture(void);
uint32_t diag_pop_capture(void);

/* Snippet rendering */
void diag_render_snippet(const SourceFile *f, Span span, const SnippetOpts *opts);

/* Get error code string for display */
const char *diag_code_to_string(DiagCode code);

/* JSON diagnostics support (Phase 8) */
typedef struct JsonDiag {
    const char *severity;   /* "error", "warning", "note", "help" */
    const char *code;       /* error code like "TUR-E0001" */
    const char *message;
    const char *file;
    uint32_t line;
    uint32_t col;
    uint32_t end_line;
    uint32_t end_col;
} JsonDiag;

/* Enable/disable JSON output mode */
void diag_set_json_output(bool enabled);

/* Emit a diagnostic in JSON format */
void diag_emit_json(DiagLevel level, Span span, DiagCode code, const char *message);

/* LSP collection mode: buffer diagnostics for batch JSON output.
 * diag_lsp_begin resets the internal list and activates collection.
 * diag_lsp_flush writes {"diagnostics":[...]} (0-based line/col) to out.
 * diag_lsp_end discards the list and deactivates collection. */
void diag_lsp_begin(void);
void diag_lsp_flush(FILE *out);

/* Write just the diagnostics JSON array [...] into buf (no outer wrapper). */
void diag_lsp_flush_array(struct Buf *buf);

void diag_lsp_end(void);

/* Replace all occurrences of `from_path` with `to_path` in buffered entries.
 * Call after diag_lsp_begin + compilation to fix up temp file paths. */
void diag_lsp_remap_path(const char *from_path, const char *to_path);

/* ---------------------------------------------------------------------------
 * Diagnostic sink (embed API -- libturi-per-embed-env-and-peripherals Gap 3)
 *
 * When a sink is installed, every diagnostic is delivered to it as a
 * structured single-line record BEFORE it would otherwise be rendered to
 * stderr, and the stderr render is suppressed.  This lets a libturi embedder
 * (e.g. the Godot binding) route each script's compile errors to its own
 * output channel attributed to that script, instead of a global stderr
 * firehose.  Pass fn == NULL to remove the sink and restore stderr rendering.
 *
 * `code` is the diagnostic's "TUR-E0001"-style string, or "" when none.
 * `file` is the registered source path, or "" when unknown.  `line`/`col_*`
 * are 1-based (0 when unavailable).  Multi-part diagnostics (notes,
 * suggestions, secondary spans) deliver the primary record first, then one
 * record per subordinate part at its own level.
 *
 * The sink coexists with the speculative-elaboration capture frames (a
 * captured error is still swallowed, not delivered).  LSP-collection and JSON
 * modes take precedence over the sink when active. */
typedef void (*DiagSinkFn)(DiagLevel level, const char *code, const char *file,
                           uint32_t line, uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud);

/* Install (or, with NULL, clear) the diagnostic sink. */
void diag_set_sink(DiagSinkFn fn, void *ud);

/* Return the currently-installed sink (NULL if none) and, when out_ud is
 * non-NULL, its user-data pointer.  Lets a caller save/restore the sink. */
DiagSinkFn diag_get_sink(void **out_ud);

/* Phase HKT-P5: Look up a long-form explanation for a diagnostic code.
 * If an explanation exists, writes it to `out` and returns true.
 * If no explanation is registered for `code`, returns false without writing.
 * `code_str` must be a string like "TUR-E0012" (as returned by
 * diag_code_to_string); `code` is the corresponding DiagCode enum value. */
bool diag_explain(DiagCode code, FILE *out);

/* Phase HKT-P5: Parse a TUR-E#### string into its DiagCode.
 * Returns DIAG_CODE_NONE if the string is not recognised. */
DiagCode diag_code_from_string(const char *s);

/* Phase HKT-P5: Return true if `s` looks like a diagnostic code string
 * of the form "TUR-E" (or W or D) followed by one or more decimal digits. */
bool diag_looks_like_code(const char *s);

#endif
