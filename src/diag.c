#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

#define MAX_FILES 64
#define MAX_NOTES 8
#define MAX_SECONDARY_SPANS 4

static const SourceFile *files_[MAX_FILES];
static size_t            file_count_;
static bool              had_error_;
static bool              use_color_ = false;
static bool              json_output_ = false;  /* Phase 8: JSON diagnostics mode */

/* ANSI color codes for diagnostics */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

void diag_init(bool use_color) {
    use_color_ = use_color;
}

bool diag_use_color(void) {
    return use_color_;
}

void diag_register_file(const SourceFile *file) {
    if (file->file_id >= MAX_FILES) {
        fprintf(stderr, "tur: too many source files\n");
        abort();
    }
    files_[file->file_id] = file;
    if (file->file_id >= file_count_) file_count_ = (size_t)file->file_id + 1;
}

bool diag_had_error(void) { return had_error_; }

void diag_reset(void) {
    had_error_ = false;
    file_count_ = 0;
    for (size_t i = 0; i < MAX_FILES; i++) files_[i] = NULL;
}

/* Check if stderr is a TTY (for auto-color detection) */
bool stderr_is_tty(void) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 1
    return isatty(fileno(stderr));
#else
    return false; /* Default to no color on non-POSIX systems */
#endif
}

/* Get the color code for a diagnostic level */
static const char *color_for_level(DiagLevel level) {
    if (!use_color_) return "";
    switch (level) {
        case DIAG_ERROR:   return COLOR_RED;
        case DIAG_WARNING: return COLOR_YELLOW;
        case DIAG_NOTE:    return COLOR_CYAN;
        case DIAG_HELP:    return COLOR_GREEN;
    }
    return "";
}

static const char *level_name(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
        case DIAG_HELP:    return "help";
    }
    return "?";
}

/* Get the underline character for a style */
static char underline_char(UnderlineStyle style) {
    switch (style) {
        case UNDERLINE_PRIMARY:   return '^';
        case UNDERLINE_SECONDARY: return '~';
        case UNDERLINE_GAP:       return '-';
    }
    return '^';
}

/* Get error code string for display */
const char *diag_code_to_string(DiagCode code) {
    switch (code) {
        case TUR_E0001_TYPE_MISMATCH:     return "TUR-E0001";
        case TUR_E0002_ARITY_MISMATCH:    return "TUR-E0002";
        case TUR_E0003_UNBOUND_SYMBOL:   return "TUR-E0003";
        case TUR_E0004_INVALID_SCOPE:    return "TUR-E0004";
        case TUR_E0005_USE_AFTER_MOVE:    return "TUR-E0005";
        case TUR_E0006_OPERATOR_LOOKUP_FAILED: return "TUR-E0006";
        case TUR_E0007_CAPTURE_ERROR:     return "TUR-E0007";
        case TUR_E0009_EFFECT_ROW_MISMATCH: return "TUR-E0009";
        case TUR_E0010_NOT_SEND:            return "TUR-E0010";
        case TUR_E0011_NOT_SYNC:            return "TUR-E0011";
        case TUR_E0012_KIND_MISMATCH:       return "TUR-E0012";
        case TUR_E0013_ORPHAN_INSTANCE:     return "TUR-E0013";
        case TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED: return "TUR-E0015";
        case TUR_E0014_NOT_CLONE:                        return "TUR-E0014";
        case TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET:    return "TUR-E0016";
        case TUR_E0017_CONT_ESCAPE_ASYNC:                return "TUR-E0017";
        case TUR_E0018_NOT_SERIALIZABLE:                 return "TUR-E0018";
        case TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET:       return "TUR-E0019";
        case TUR_E0021_PRIVATE_EFFECT:                   return "TUR-E0021";
        case TUR_W0030_STRICT_EFFECTS_UNANNOTATED: return "TUR-W0030";
        case TUR_W0031_EFFECT_OVER_ANNOTATED:      return "TUR-W0031";
        case TUR_W0032_ROW_VAR_ALWAYS_CONCRETE:    return "TUR-W0032";
        /* LT1: Linear type errors */
        case TUR_E0100_LINEAR_DROPPED:             return "TUR-E0100";
        case TUR_E0101_LINEAR_USE_AFTER_CONSUME:   return "TUR-E0101";
        case TUR_E0102_LINEAR_COPY:                return "TUR-E0102";
        case TUR_E0103_LINEAR_IN_RC:               return "TUR-E0103";
        /* ST0: Substructural type errors */
        case TUR_E0150_AFFINE_USED_TWICE:          return "TUR-E0150";
        case TUR_E0151_RELEVANT_DROPPED:           return "TUR-E0151";
        /* UT1: Uniqueness type errors */
        case TUR_E0200_UNIQUE_ALIASED:             return "TUR-E0200";
        case TUR_E0201_UNIQUE_COPY:                return "TUR-E0201";
        case TUR_E0202_UNIQUE_IN_RC:               return "TUR-E0202";
        /* IT1: Union type errors */
        case TUR_E0300_UNION_TYPE_MISMATCH:        return "TUR-E0300";
        case TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH: return "TUR-E0301";
        default:                          return "";
    }
}

/* Phase HKT-P5: DiagCode lookup by string (inverse of diag_code_to_string). */
DiagCode diag_code_from_string(const char *s) {
    if (!s) return DIAG_CODE_NONE;
    if (strcmp(s, "TUR-E0001") == 0) return TUR_E0001_TYPE_MISMATCH;
    if (strcmp(s, "TUR-E0002") == 0) return TUR_E0002_ARITY_MISMATCH;
    if (strcmp(s, "TUR-E0003") == 0) return TUR_E0003_UNBOUND_SYMBOL;
    if (strcmp(s, "TUR-E0004") == 0) return TUR_E0004_INVALID_SCOPE;
    if (strcmp(s, "TUR-E0005") == 0) return TUR_E0005_USE_AFTER_MOVE;
    if (strcmp(s, "TUR-E0006") == 0) return TUR_E0006_OPERATOR_LOOKUP_FAILED;
    if (strcmp(s, "TUR-E0007") == 0) return TUR_E0007_CAPTURE_ERROR;
    if (strcmp(s, "TUR-E0009") == 0) return TUR_E0009_EFFECT_ROW_MISMATCH;
    if (strcmp(s, "TUR-E0010") == 0) return TUR_E0010_NOT_SEND;
    if (strcmp(s, "TUR-E0011") == 0) return TUR_E0011_NOT_SYNC;
    if (strcmp(s, "TUR-E0012") == 0) return TUR_E0012_KIND_MISMATCH;
    if (strcmp(s, "TUR-E0013") == 0) return TUR_E0013_ORPHAN_INSTANCE;
    if (strcmp(s, "TUR-E0015") == 0) return TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED;
    if (strcmp(s, "TUR-E0014") == 0) return TUR_E0014_NOT_CLONE;
    if (strcmp(s, "TUR-E0016") == 0) return TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET;
    if (strcmp(s, "TUR-E0017") == 0) return TUR_E0017_CONT_ESCAPE_ASYNC;
    if (strcmp(s, "TUR-E0018") == 0) return TUR_E0018_NOT_SERIALIZABLE;
    if (strcmp(s, "TUR-E0019") == 0) return TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET;
    if (strcmp(s, "TUR-E0021") == 0) return TUR_E0021_PRIVATE_EFFECT;
    if (strcmp(s, "TUR-W0030") == 0) return TUR_W0030_STRICT_EFFECTS_UNANNOTATED;
    if (strcmp(s, "TUR-W0031") == 0) return TUR_W0031_EFFECT_OVER_ANNOTATED;
    if (strcmp(s, "TUR-W0032") == 0) return TUR_W0032_ROW_VAR_ALWAYS_CONCRETE;
    /* LT1: Linear type errors */
    if (strcmp(s, "TUR-E0100") == 0) return TUR_E0100_LINEAR_DROPPED;
    if (strcmp(s, "TUR-E0101") == 0) return TUR_E0101_LINEAR_USE_AFTER_CONSUME;
    if (strcmp(s, "TUR-E0102") == 0) return TUR_E0102_LINEAR_COPY;
    if (strcmp(s, "TUR-E0103") == 0) return TUR_E0103_LINEAR_IN_RC;
    /* ST0: Substructural type errors */
    if (strcmp(s, "TUR-E0150") == 0) return TUR_E0150_AFFINE_USED_TWICE;
    if (strcmp(s, "TUR-E0151") == 0) return TUR_E0151_RELEVANT_DROPPED;
    /* UT1: Uniqueness type errors */
    if (strcmp(s, "TUR-E0200") == 0) return TUR_E0200_UNIQUE_ALIASED;
    if (strcmp(s, "TUR-E0201") == 0) return TUR_E0201_UNIQUE_COPY;
    if (strcmp(s, "TUR-E0202") == 0) return TUR_E0202_UNIQUE_IN_RC;
    /* IT1: Union type errors */
    if (strcmp(s, "TUR-E0300") == 0) return TUR_E0300_UNION_TYPE_MISMATCH;
    if (strcmp(s, "TUR-E0301") == 0) return TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH;
    return DIAG_CODE_NONE;
}

/* Phase HKT-P5: Long-form explanation table (modelled on rustc --explain).
 * Each entry maps a DiagCode to a multi-paragraph explanation string. */
typedef struct DiagExplanation {
    DiagCode    code;
    const char *text;
} DiagExplanation;

static const DiagExplanation diag_explanations_[] = {
    { TUR_E0001_TYPE_MISMATCH,
      "TUR-E0001: Type mismatch\n"
      "\n"
      "The type of an expression does not match the type expected in that\n"
      "position.\n"
      "\n"
      "Example:\n"
      "  (defn add [x :int y :int] :int (+ x y))\n"
      "  (add 1 true)   ; error: expected :int, got :bool\n"
      "\n"
      "Ensure that argument types, return types, and binding types are\n"
      "consistent.  Explicit type annotations help the compiler pinpoint the\n"
      "mismatch.\n"
    },
    { TUR_E0002_ARITY_MISMATCH,
      "TUR-E0002: Arity mismatch\n"
      "\n"
      "A function call provides a different number of arguments than the\n"
      "function declares parameters.\n"
      "\n"
      "Example:\n"
      "  (defn add [x :int y :int] :int (+ x y))\n"
      "  (add 1)   ; error: expected 2 args, got 1\n"
      "\n"
      "Count the parameters in the function definition and match the number\n"
      "of arguments at the call site.\n"
    },
    { TUR_E0003_UNBOUND_SYMBOL,
      "TUR-E0003: Unbound symbol\n"
      "\n"
      "A name is used that has not been defined in any enclosing scope.\n"
      "\n"
      "Common causes:\n"
      "  - Typo in the name.\n"
      "  - Forgetting to import or require the file that defines the name.\n"
      "  - Using a name before it is defined (Turmeric is sequential, not\n"
      "    mutually recursive by default).\n"
    },
    { TUR_E0004_INVALID_SCOPE,
      "TUR-E0004: Invalid scope\n"
      "\n"
      "A form is used in a position where it is not allowed.  For example,\n"
      "using a top-level-only form (defn, defstruct) inside an expression\n"
      "context, or a control-flow form outside a function body.\n"
    },
    { TUR_E0005_USE_AFTER_MOVE,
      "TUR-E0005: Use after move\n"
      "\n"
      "A value was moved (transferred to another binding or passed to a\n"
      "function that consumes it) and then used again after the move.\n"
      "\n"
      "Example:\n"
      "  (let [r (ref 42)]\n"
      "    (let [s r]     ; r is moved into s\n"
      "      (deref r)))  ; error: r was already moved\n"
      "\n"
      "Clone or copy the value before moving if you need to use it again,\n"
      "or restructure the code so ownership is only held in one place at a\n"
      "time.\n"
    },
    { TUR_E0007_CAPTURE_ERROR,
      "TUR-E0007: Capture error\n"
      "\n"
      "A closure or effect-handler case attempts to capture a variable that\n"
      "cannot safely be captured in that context.\n"
      "\n"
      "Effect handler case bodies are emitted as separate C functions with\n"
      "no access to the enclosing stack frame, so borrow-typed (&T / &mut T)\n"
      "variables from the enclosing scope cannot be captured by them.\n"
    },
    { TUR_E0009_EFFECT_ROW_MISMATCH,
      "TUR-E0009: Effect-row mismatch\n"
      "\n"
      "The effect row of a function call or expression does not match the\n"
      "effect row expected by the enclosing context.\n"
      "\n"
      "Ensure that any algebraic effects used inside a function are listed\n"
      "in its effect annotation (e.g., #{IO Write}) and that `handle`\n"
      "blocks cover all effects that are performed inside them.\n"
    },
    { TUR_E0010_NOT_SEND,
      "TUR-E0010: Type is not Send\n"
      "\n"
      "An attempt was made to transfer a value of a non-Send type across a\n"
      "thread boundary (e.g., passing it to (spawn ...) or storing it in a\n"
      "channel).\n"
      "\n"
      "Non-Send types include:\n"
      "  - rc<T>  (reference-counted; not thread-safe)\n"
      "  - weak<T> (weak reference into an rc; not thread-safe)\n"
      "  - Borrow types (&T, &mut T) whose lifetimes do not cross threads.\n"
      "\n"
      "Use arc<T> (atomic reference count) or plain values for data that\n"
      "must cross thread boundaries.\n"
    },
    { TUR_E0011_NOT_SYNC,
      "TUR-E0011: Type is not Sync\n"
      "\n"
      "An attempt was made to share a reference to a non-Sync type across\n"
      "thread boundaries (e.g., placing it behind a shared atomic).\n"
      "\n"
      "Non-Sync types include types with interior mutability that is not\n"
      "protected by a synchronization primitive such as mutex<T> or\n"
      "rwlock<T>.\n"
      "\n"
      "Wrap the value in mutex<T> or rwlock<T> to make shared mutable access\n"
      "thread-safe.\n"
    },
    { TUR_E0012_KIND_MISMATCH,
      "TUR-E0012: Kind mismatch\n"
      "\n"
      "A type expression is used where a type constructor of a different\n"
      "kind is expected.\n"
      "\n"
      "Kinds describe the 'type of a type':\n"
      "  *        — a fully applied, concrete type  (e.g., int, bool, vec<int>)\n"
      "  * -> *   — a unary type constructor        (e.g., vec, option, rc)\n"
      "  * -> * -> *  — a binary type constructor   (e.g., result, either)\n"
      "\n"
      "Example:\n"
      "  (defclass Functor [^f]\n"
      "    (fmap [container fn] :int))\n"
      "  (definstance Functor [int])\n"
      "  ; error: Functor expects a type constructor (kind * -> *),\n"
      "  ; but int has kind *.\n"
      "\n"
      "Pass a type constructor (vec, option, rc, …) where kind '* -> *' is\n"
      "required, and a concrete type (int, bool, …) where kind '*' is\n"
      "required.\n"
    },
    { TUR_E0013_ORPHAN_INSTANCE,
      "TUR-E0013: Orphan instance\n"
      "\n"
      "A typeclass instance is defined in a file that owns neither the\n"
      "typeclass nor any of the type arguments of the instance.\n"
      "\n"
      "This mirrors the Rust / Haskell 'orphan rule': to avoid conflicting\n"
      "instances when multiple modules are combined, every instance must be\n"
      "defined in the same module as either the typeclass or at least one\n"
      "of the concrete types it is instantiated for.\n"
      "\n"
      "In Turmeric v1 (single-file compilation) this diagnostic is advisory\n"
      "(a warning, not an error) because all definitions share a single file.\n"
      "It will be promoted to a hard error once the module system lands\n"
      "(see P19-6).\n"
      "\n"
      "To silence the warning, move the instance definition into the file\n"
      "that defines the typeclass or the file that defines one of the types\n"
      "used as type arguments.\n"
    },
    { TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
      "TUR-E0015: Typeclass constraint not satisfied\n"
      "\n"
      "A typeclass instance declaration includes a constraint that cannot be\n"
      "satisfied because no matching typeclass instance exists.\n"
      "\n"
      "Example:\n"
      "  (defclass Clone [a] (clone [x : a] : a))\n"
      "  (definstance Clone [int] (clone [x] x))\n"
      "  (definstance Clone [Pair a b] [Clone a] ...)  ; error if Clone[a] missing\n"
      "\n"
      "In Phase PTC2 (v1), constraints are only validated for primitive types\n"
      "(int, bool, cstr, nil, float, ptr<void>). Constraints on user-defined\n"
      "types are stored but not validated until PTC3 lands.\n"
      "\n"
      "Define an instance of the required typeclass for the constrained type,\n"
      "or remove the constraint if it is not needed.\n"
    },
    { TUR_E0014_NOT_CLONE,
      "TUR-E0014: Captured binding does not implement Clone\n"
      "\n"
      "A cloneable-shift captures a local variable whose type does not have a\n"
      "Clone instance.  Because cloneable continuations can be resumed multiple\n"
      "times, every value captured across the shift boundary must be deep-\n"
      "cloneable so that each clone of the continuation gets an independent copy.\n"
      "\n"
      "Example:\n"
      "  (let [x (make-non-cloneable)]\n"
      "    (cloneable-reset\n"
      "      (cloneable-shift (fn [k] k) x)))  ; error: x is not Clone\n"
      "\n"
      "Add a (definstance Clone [YourType] ...) to make the type cloneable,\n"
      "or restructure the code so the non-cloneable value is not captured.\n"
    },
    { TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
      "TUR-E0016: cloneable-shift outside cloneable-reset\n"
      "\n"
      "A cloneable-shift form was used outside of any enclosing cloneable-reset\n"
      "boundary.  cloneable-shift captures the current continuation up to the\n"
      "nearest enclosing cloneable-reset; without one, there is no continuation\n"
      "to capture.\n"
      "\n"
      "Example:\n"
      "  (defn bad [] :int\n"
      "    (cloneable-shift (fn [k] 42) 0))  ; error: no enclosing cloneable-reset\n"
      "\n"
      "Wrap the cloneable-shift in a cloneable-reset:\n"
      "  (cloneable-reset (cloneable-shift (fn [k] 42) 0))\n"
    },
    { TUR_E0017_CONT_ESCAPE_ASYNC,
      "TUR-E0017: Continuation escape into async scope\n"
      "\n"
      "An effect-handler continuation `k` was captured by an async block.  Effect\n"
      "handler continuations are bound to the fiber that performed the effect; if `k`\n"
      "is resumed from a different fiber (spawned by async), the fiber identity check\n"
      "in tur_cont_resume will fail at runtime with a fatal error.\n"
      "\n"
      "Example of the problem:\n"
      "  (defeffect GetK [] :int)\n"
      "  (handle (perform (GetK))\n"
      "    (GetK [] k)\n"
      "      (await (async (fn [] :int (resume k 42)))))  ; ERROR: k escapes\n"
      "\n"
      "Instead, resume the continuation synchronously inside the handler body:\n"
      "  (defeffect GetK [] :int)\n"
      "  (handle (perform (GetK))\n"
      "    (GetK [] k) (resume k 42))\n"
      "\n"
      "Or, if you need async work before resuming, perform the async computation and\n"
      "pass the result back synchronously:\n"
      "  (handle (perform (GetK))\n"
      "    (GetK [] k)\n"
      "      (let [v (await (async (fn [] :int 42)))]\n"
      "        (resume k v)))\n"
    },
    /* Phase 21: Serializable continuations */
    { TUR_E0018_NOT_SERIALIZABLE,
      "TUR-E0018: captured binding does not implement Serializable\n"
      "\n"
      "A serial-shift form captured a binding whose type does not implement the\n"
      "Serializable typeclass.  serial-shift requires every captured value to be\n"
      "round-trippable through bytes so the continuation can be marshalled to disk\n"
      "or sent over a network and resumed in a fresh process.\n"
      "\n"
      "Example:\n"
      "  (let [handle (open-file \"data.txt\")]\n"
      "    (serial-reset\n"
      "      (serial-shift (fn [k] k) 0)))  ; error: handle : file-handle is not Serializable\n"
      "\n"
      "Solutions:\n"
      "  1. Move non-serializable resources outside the serial-reset boundary.\n"
      "  2. Implement Serializable for the type with custom marshal/unmarshal hooks\n"
      "     (e.g., serialise a file path and re-open on resume).\n"
    },
    { TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,
      "TUR-E0019: serial-shift used outside serial-reset boundary\n"
      "\n"
      "A serial-shift form appeared outside of any enclosing serial-reset.\n"
      "serial-shift captures the current continuation up to the nearest enclosing\n"
      "serial-reset; without one there is no delimited continuation to capture.\n"
      "\n"
      "Wrap the serial-shift in a serial-reset:\n"
      "  (serial-reset\n"
      "    (serial-shift (fn [k] (save-cont! k) 0) 42))\n"
    },
    { TUR_E0021_PRIVATE_EFFECT,
      "TUR-E0021: Private effect accessed outside its defining module\n"
      "\n"
      "An effect declared with ^private is only visible within the module that\n"
      "defined it.  Attempting to perform or handle a private effect from another\n"
      "module is a compile-time error.\n"
      "\n"
      "Example of the problem:\n"
      "  ; module mymod\n"
      "  (defmodule mymod\n"
      "    (defeffect ^private Ask [] :int)\n"
      "    (defn run [] :int (perform (Ask))))\n"
      "\n"
      "  ; module consumer -- ERROR\n"
      "  (handle (perform (Ask))       ; TUR-E0021: Ask is private to mymod\n"
      "    (Ask [] k) (resume k 42))\n"
      "\n"
      "Solutions:\n"
      "  1. Remove ^private from the defeffect declaration to make the effect\n"
      "     public and importable by other modules.\n"
      "  2. Expose a public function in mymod that encapsulates the effect and\n"
      "     provides the handler, so consumers never touch Ask directly.\n"
    },
    { TUR_W0030_STRICT_EFFECTS_UNANNOTATED,
      "TUR-W0030: Unannotated effectful function (--strict-effects)\n"
      "\n"
      "Under --strict-effects, every function whose inferred effect row is non-empty\n"
      "should carry an explicit #{...} annotation. This warning fires when an unannotated\n"
      "function performs one or more effects.\n"
      "\n"
      "Fix: add an effect-row annotation to the function, e.g.:\n"
      "  (defn my-fn [] #{Write} :nil ...)\n"
      "Or handle the effect inside the function so its row is empty.\n",
    },
    { TUR_W0031_EFFECT_OVER_ANNOTATED,
      "TUR-W0031: Over-annotated effect row\n"
      "\n"
      "The declared effect row contains an effect that the function never actually\n"
      "performs. This may indicate a stale annotation after refactoring.\n"
      "\n"
      "Fix: remove the unused effect from the #{...} annotation, e.g.:\n"
      "  (defn my-fn [] #{Write} :nil ...)  ; remove Log if it is never performed\n",
    },
    { TUR_W0032_ROW_VAR_ALWAYS_CONCRETE,
      "TUR-W0032: Row variable is always instantiated to a concrete row\n"
      "\n"
      "A function declares a row variable (e.g., #{e}) but at every call site the\n"
      "variable is always bound to the same concrete effect set. Replacing the row\n"
      "variable with the concrete row makes the annotation more informative and\n"
      "allows the compiler to enforce it strictly.\n"
      "\n"
      "Fix: replace the row variable with the concrete effect set, e.g.:\n"
      "  (defn run-twice [f :(fn [] #{Ask} :int)] #{Ask} :int ...)\n",
    },
    /* LT1: Linear type errors */
    { TUR_E0100_LINEAR_DROPPED,
      "TUR-E0100: Linear value dropped without being consumed\n"
      "\n"
      "A value of linear type (lref<T>, annotated with ^linear, or a ref<T> under\n"
      "-Xsubstructural) went out of scope without being consumed exactly once.\n"
      "Linear values must be explicitly consumed -- passing them to a function,\n"
      "returning them, or binding them to another name.\n"
      "\n"
      "This error is part of the unified substructural type system (-Xsubstructural),\n"
      "which enforces three disciplines:\n"
      "  ^linear   -- must be used exactly once (no weakening, no contraction)\n"
      "  ^affine   -- may be discarded; may not be duplicated (no contraction)\n"
      "  ^relevant -- must be used at least once; may be duplicated (no weakening)\n"
      "\n"
      "Example of the error:\n"
      "  (let [fh (open-file \"data.txt\")]\n"
      "    42)           ; ERROR: fh is dropped without being consumed\n"
      "\n"
      "Fix: consume the value before the scope ends:\n"
      "  (let [fh (open-file \"data.txt\")]\n"
      "    (close-file fh))\n"
      "\n"
      "Enable with: tur -Xlinear myfile.tur\n"
      "         or: tur -Xsubstructural myfile.tur\n",
    },
    { TUR_E0101_LINEAR_USE_AFTER_CONSUME,
      "TUR-E0101: Linear value used after being moved or consumed\n"
      "\n"
      "A value of linear type was used a second time after it had already been\n"
      "consumed. Each linear value may be used exactly once.\n"
      "\n"
      "This is enforced by the substructural type system (-Xsubstructural).\n"
      "^linear is the strictest discipline: no weakening (must use) and no\n"
      "contraction (cannot duplicate). See also TUR-E0150 for ^affine violations.\n"
      "\n"
      "Example of the error:\n"
      "  (let [fh (open-file \"data.txt\")]\n"
      "    (read-file fh)   ; fh consumed here\n"
      "    (close-file fh)) ; ERROR: fh already consumed\n"
      "\n"
      "Fix: restructure so each linear value flows through exactly one code path.\n"
      "\n"
      "Enable with: tur -Xlinear myfile.tur\n"
      "         or: tur -Xsubstructural myfile.tur\n",
    },
    { TUR_E0102_LINEAR_COPY,
      "TUR-E0102: Cannot copy a linear value\n"
      "\n"
      "Linear values (lref<T>, ^linear, or ref<T> under -Xsubstructural) cannot\n"
      "be implicitly duplicated. Copying would allow the value to be consumed\n"
      "twice, violating the exactly-once guarantee.\n"
      "\n"
      "This is enforced by the substructural type system (-Xsubstructural).\n"
      "The three disciplines and their duplication rules:\n"
      "  ^linear   -- no contraction: cannot duplicate\n"
      "  ^affine   -- no contraction: cannot duplicate (see TUR-E0150)\n"
      "  ^relevant -- contraction allowed: may duplicate freely\n"
      "\n"
      "If you need to share the underlying resource, consider wrapping it in an\n"
      "abstraction that explicitly transfers ownership (such as passing it through\n"
      "a channel or splitting it into sub-resources).\n"
      "\n"
      "Enable with: tur -Xlinear myfile.tur\n"
      "         or: tur -Xsubstructural myfile.tur\n",
    },
    { TUR_E0103_LINEAR_IN_RC,
      "TUR-E0103: Cannot wrap a linear value in rc<T>\n"
      "\n"
      "Placing a linear value (lref<T> or ^linear) inside rc<T> is forbidden.\n"
      "Shared reference-counting allows multiple owners, which would break the\n"
      "exactly-once consumption guarantee of linear types.\n"
      "\n"
      "If the resource must be shared, it must first be made non-linear (e.g., by\n"
      "consuming it and producing a shared representation).\n"
      "\n"
      "Enable with: turc -Xlinear myfile.tur\n",
    },
    /* UT1: Uniqueness type explanations */
    { TUR_E0200_UNIQUE_ALIASED,
      "TUR-E0200: Value is not unique -- it has been aliased\n"
      "\n"
      "A value was expected to have at most one live reference (^unique), but\n"
      "another binding that refers to the same value is still in scope. A unique\n"
      "value cannot be passed to a ^unique parameter when aliases exist.\n"
      "\n"
      "Example of the error:\n"
      "  (defn consume-unique [^unique v] :int v)\n"
      "  (let [x 42]\n"
      "    (let [y x]             ; y is an alias of x\n"
      "      (consume-unique x))) ; ERROR: x is aliased by y\n"
      "\n"
      "Fix: ensure no other binding refers to x before passing it as ^unique:\n"
      "  (let [x 42]\n"
      "    (consume-unique x))    ; OK: x has no aliases\n"
      "\n"
      "Enable with: turc -Xunique-types myfile.tur\n",
    },
    { TUR_E0201_UNIQUE_COPY,
      "TUR-E0201: Cannot copy unique value\n"
      "\n"
      "A value annotated with ^unique has already been consumed (moved or passed\n"
      "to a function). Unique values have at-most-one-use semantics for consumption\n"
      "-- once transferred, the original binding cannot be used again.\n"
      "\n"
      "Example of the error:\n"
      "  (let [^unique x 42]\n"
      "    (println x)   ; x consumed here\n"
      "    (println x))  ; ERROR: x already consumed\n"
      "\n"
      "Fix: ensure each ^unique value is used at most once:\n"
      "  (let [^unique x 42]\n"
      "    (println x))  ; OK: single use\n"
      "\n"
      "Enable with: turc -Xunique-types myfile.tur\n",
    },
    { TUR_E0202_UNIQUE_IN_RC,
      "TUR-E0202: Cannot wrap a unique value in rc<T>\n"
      "\n"
      "Placing a ^unique value inside rc<T> is forbidden. Shared reference-counting\n"
      "creates multiple owners of the same value, which violates the at-most-one-\n"
      "reference guarantee of uniqueness types.\n"
      "\n"
      "Example of the error:\n"
      "  (let [^unique x 42]\n"
      "    (rc/of x))   ; ERROR: cannot wrap unique value in rc\n"
      "\n"
      "Fix: if shared ownership is needed, remove the ^unique annotation.\n"
      "\n"
      "Enable with: tur -Xunique-types myfile.tur\n",
    },
    /* ST1: Substructural type explanations */
    { TUR_E0150_AFFINE_USED_TWICE,
      "TUR-E0150: Affine value used more than once\n"
      "\n"
      "A value annotated with ^affine was used more than once. Affine values\n"
      "implement no-contraction: they may be discarded (weakening is allowed),\n"
      "but they may not be duplicated or used a second time.\n"
      "\n"
      "This is part of the unified substructural type system (-Xsubstructural).\n"
      "The three disciplines and their usage rules:\n"
      "  ^linear   -- must use exactly once (no weakening, no contraction)\n"
      "  ^affine   -- may discard; may not duplicate (no contraction)\n"
      "  ^relevant -- must use at least once; may duplicate (no weakening)\n"
      "\n"
      "Example of the error:\n"
      "  (let [^affine k (generate-key)]\n"
      "    (initialize k)\n"
      "    (initialize k))  ; ERROR: k used twice\n"
      "\n"
      "Fix: use the affine value at most once:\n"
      "  (let [^affine k (generate-key)]\n"
      "    (initialize k))  ; OK\n"
      "\n"
      "If you need to use the value multiple times, consider ^relevant instead\n"
      "(must-use, but duplication is allowed).\n"
      "\n"
      "Enable with: tur -Xsubstructural myfile.tur\n",
    },
    /* IT1: Union type explanations */
    { TUR_E0300_UNION_TYPE_MISMATCH,
      "TUR-E0300: Union type mismatch\n"
      "\n"
      "A value's type is not a member of the expected union type.\n"
      "\n"
      "Example:\n"
      "  (defn print-either [x : (int | cstr)] : nil (println x))\n"
      "  (print-either true)  ; error: bool is not a member of (int | cstr)\n"
      "\n"
      "Pass a value whose type is one of the union members, or widen the union\n"
      "type to include the actual type.\n"
      "\n"
      "Enable with: turc -Xunion-types myfile.tur\n",
    },
    { TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH,
      "TUR-E0301: Non-exhaustive pattern match on union type\n"
      "\n"
      "A match expression on a union type does not cover all member types.\n"
      "Every arm must be a type-narrowing pattern (varname : Type) covering\n"
      "each union member, or a wildcard '_' or variable capturing the rest.\n"
      "\n"
      "Example:\n"
      "  (defn f [x : (int | cstr | bool)] : nil\n"
      "    (match x\n"
      "      (n : int)  (println n)\n"
      "      (s : cstr) (println s)))\n"
      "  ; error: match on (int | cstr | bool) is missing arm for bool\n"
      "\n"
      "Fix: add arms for all missing member types, or add a wildcard arm:\n"
      "  (match x\n"
      "    (n : int)  (println n)\n"
      "    (s : cstr) (println s)\n"
      "    (b : bool) (println b))\n"
      "\n"
      "Enable with: turc -Xunion-types myfile.tur\n",
    },
    { TUR_E0151_RELEVANT_DROPPED,
      "TUR-E0151: Relevant value dropped without being used\n"
      "\n"
      "A value annotated with ^relevant went out of scope without being used at\n"
      "least once. Relevant values implement no-weakening: they must be observed\n"
      "or consumed before going out of scope, but may be duplicated freely.\n"
      "\n"
      "This is part of the unified substructural type system (-Xsubstructural).\n"
      "The three disciplines and their drop rules:\n"
      "  ^linear   -- must use exactly once; cannot drop unused\n"
      "  ^affine   -- may drop unused; cannot duplicate\n"
      "  ^relevant -- must use at least once; may duplicate (cannot drop unused)\n"
      "\n"
      "Example of the error:\n"
      "  (let [^relevant r (acquire-resource)]\n"
      "    0)            ; ERROR: r dropped without being used\n"
      "\n"
      "Fix: use the value at least once before the scope ends:\n"
      "  (let [^relevant r (acquire-resource)]\n"
      "    (log r)       ; first use\n"
      "    (store r))    ; second use -- OK, duplication allowed\n"
      "\n"
      "If you only need to ensure the value is dropped (not necessarily used),\n"
      "consider ^affine or ^linear instead.\n"
      "\n"
      "Enable with: tur -Xsubstructural myfile.tur\n",
    },
};

#define N_DIAG_EXPLANATIONS \
    ((int)(sizeof(diag_explanations_) / sizeof(diag_explanations_[0])))

/* Phase HKT-P5: Print the long-form explanation for `code` to `out`.
 * Returns true if an explanation was found and printed, false otherwise. */
bool diag_explain(DiagCode code, FILE *out) {
    for (int i = 0; i < N_DIAG_EXPLANATIONS; i++) {
        if (diag_explanations_[i].code == code) {
            fputs(diag_explanations_[i].text, out);
            return true;
        }
    }
    return false;
}


/* Render a multi-line snippet with context, underlines, and colors.
   Phase 8: Enhanced with configurable options and multi-span support. */
static void render_snippet_ex(const SourceFile *f, Span span, const SnippetOpts *opts) {
    if (!f || span.off_start > f->len) return;
    
    const SnippetOpts default_opts = SNIPPET_OPTS_DEFAULT;
    const SnippetOpts *o = opts ? opts : &default_opts;
    
    const char *color = use_color_ ? COLOR_BOLD : "";
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Calculate the range of lines to show */
    uint32_t error_line = span.line;
    uint32_t context = o->context_lines;
    uint32_t start_line = (error_line > context + 1) ? error_line - context - 1 : 1;
    uint32_t end_line = error_line + context;
    
    /* Get the width for line numbers (padding) */
    int line_num_width = 1;
    uint32_t temp = end_line;
    while (temp >= 10) {
        line_num_width++;
        temp /= 10;
    }
    
    /* Print each line with line number and content */
    uint32_t current_line = 1;
    uint32_t line_start = 0;
    
    for (uint32_t i = 0; i <= f->len; i++) {
        if (i == f->len || f->src[i] == '\n') {
            /* Check if we should stop before processing this line */
            if (current_line > end_line) break;
            
            if (current_line >= start_line && current_line <= end_line) {
                /* Skip empty lines at the end of the file */
                uint32_t line_len = i - line_start;
                if (line_len == 0 && i == f->len) {
                    /* Empty line at end of file - skip it */
                } else {
                    /* Print line number and gutter */
                    if (o->show_line_numbers) {
                        if (current_line == error_line) {
                            fprintf(stderr, "%s%*u %s|%s ", color, line_num_width, current_line, reset, color);
                        } else {
                            fprintf(stderr, "%*u | ", line_num_width, current_line);
                        }
                    } else {
                        if (current_line == error_line) {
                            fprintf(stderr, "%s|%s ", color, reset);
                        } else {
                            fprintf(stderr, " | ");
                        }
                    }
                    
                    /* Print the line content */
                    if (f->src[line_start] != '\n') {
                        fwrite(f->src + line_start, 1, line_len, stderr);
                    }
                    fprintf(stderr, "%s\n", reset);
                    
                    /* Print underline for the error line */
                    if (current_line == error_line) {
                        /* Calculate column position (0-based in the line) */
                        uint32_t col_start_0 = span.col_start - 1;
                        uint32_t col_end_0 = span.col_end - 1;
                        
                        /* Print gutter space */
                        if (o->show_line_numbers) {
                            fprintf(stderr, "%*s | ", line_num_width, "");
                        } else {
                            fprintf(stderr, " | ");
                        }
                        
                        /* Print spaces up to the start column */
                        for (uint32_t c = 0; c < col_start_0; c++) {
                            char ch = (c < line_len) ? f->src[line_start + c] : ' ';
                            if (ch == '\t') {
                                fputc('\t', stderr);
                            } else {
                                fputc(' ', stderr);
                            }
                        }
                        
                        /* Print underline with the appropriate style */
                        uint32_t underline_len = (col_end_0 > col_start_0) ? col_end_0 - col_start_0 : 1;
                        fprintf(stderr, "%s", color);
                        char underline_ch = underline_char(o->primary_style);
                        for (uint32_t c = 0; c < underline_len; c++) {
                            fputc(underline_ch, stderr);
                        }
                        fprintf(stderr, "%s\n", reset);
                    }
                }
            }
            
            if (f->src[i] == '\n') {
                line_start = i + 1;
                current_line++;
            }
            
            /* If we just processed the last line, check if we should stop */
            if (i == f->len) break;
        }
    }
}

/* Original snippet rendering (backward compatible) */
static void render_snippet(const SourceFile *f, Span span) {
    render_snippet_ex(f, span, NULL);
}

/* Public snippet rendering function (Phase 8) */
void diag_render_snippet(const SourceFile *f, Span span, const SnippetOpts *opts) {
    render_snippet_ex(f, span, opts);
}

void diag_emitv(DiagLevel level, Span span, const char *fmt, va_list ap) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        diag_emit_json(level, span, DIAG_CODE_NONE, msg);
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    /* Color the level name */
    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Phase 8: Rust-style diagnostics with --> pointing to file */
    fprintf(stderr, "%s%s:%u:%u: %s%s: ", color, path, span.line, span.col_start, level_name(level), reset);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    /* Multi-line source snippet with context */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
}

void diag_emit(DiagLevel level, Span span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_emitv(level, span, fmt, ap);
    va_end(ap);
}

/* Emit diagnostic with error code (Phase 8) */
void diag_emit_with_code(DiagLevel level, Span span, DiagCode code, const char *fmt, ...) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        va_list ap;
        va_start(ap, fmt);
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        diag_emit_json(level, span, code, msg);
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    const char *code_str = diag_code_to_string(code);
    
    va_list ap;
    va_start(ap, fmt);
    
    if (code != DIAG_CODE_NONE) {
        fprintf(stderr, "%s%s:%u:%u: %s [%s]%s: ", color, path, span.line, span.col_start, level_name(level), code_str, reset);
    } else {
        fprintf(stderr, "%s%s:%u:%u: %s%s: ", color, path, span.line, span.col_start, level_name(level), reset);
    }
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    
    va_end(ap);

    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
}

/* Emit diagnostic with related notes (Phase 8) */
void diag_emit_with_notes(DiagLevel level, Span span, const char *message,
                          DiagNote *notes, size_t note_count) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit primary message in JSON format */
    if (json_output_) {
        diag_emit_json(level, span, DIAG_CODE_NONE, message);
        /* Also emit notes in JSON format */
        for (size_t i = 0; i < note_count; i++) {
            diag_emit_json(notes[i].level, notes[i].span, DIAG_CODE_NONE, notes[i].message);
        }
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Print primary error */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, span.line, span.col_start, level_name(level), reset, message);

    /* Print primary snippet */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
    
    /* Print notes */
    for (size_t i = 0; i < note_count; i++) {
        const SourceFile *note_f = NULL;
        if (notes[i].span.file_id < MAX_FILES) note_f = files_[notes[i].span.file_id];
        const char *note_path = note_f ? note_f->path : "<unknown>";
        const char *note_color = color_for_level(notes[i].level);
        
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", note_color, note_path, 
                notes[i].span.line, notes[i].span.col_start,
                level_name(notes[i].level), reset, notes[i].message);
        
        if (note_f && notes[i].span.off_start <= note_f->len) {
            SnippetOpts note_opts = SNIPPET_OPTS_DEFAULT;
            note_opts.primary_style = UNDERLINE_SECONDARY;
            render_snippet_ex(note_f, notes[i].span, &note_opts);
        }
    }
}

/* Emit diagnostic with suggestion (Phase 8) */
void diag_emit_with_suggestion(DiagLevel level, Span span, const char *message,
                               const DiagSuggestion *suggestion) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        diag_emit_json(level, span, DIAG_CODE_NONE, message);
        if (suggestion && suggestion->text) {
            diag_emit_json(DIAG_HELP, span, DIAG_CODE_NONE, suggestion->text);
        }
        return;
    }

    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    const char *help_color = use_color_ ? COLOR_GREEN : "";
    
    /* Print primary error */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, span.line, span.col_start, level_name(level), reset, message);

    /* Print snippet */
    if (f && span.off_start <= f->len) {
        render_snippet(f, span);
    }
    
    /* Print suggestion */
    if (suggestion && suggestion->text) {
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", help_color, path, span.line, span.col_start,
                level_name(DIAG_HELP), reset, suggestion->text);
        
        if (suggestion->replacement) {
            fprintf(stderr, "%s%s:%u:%u: %s%s: try: %s\n", help_color, path, span.line, span.col_start,
                    level_name(DIAG_HELP), reset, suggestion->replacement);
        }
        
        if (suggestion->doc_url) {
            fprintf(stderr, "%s%s:%u:%u: %s%s: see: %s\n", help_color, path, span.line, span.col_start,
                    level_name(DIAG_HELP), reset, suggestion->doc_url);
        }
    }
}

/* Emit multi-span diagnostic (Phase 8) */
void diag_emit_multi_span(DiagLevel level, const char *message,
                         Span primary_span, const char *primary_label,
                         Span *secondary_spans, const char **secondary_labels,
                         size_t secondary_count) {
    if (level == DIAG_ERROR) had_error_ = true;

    /* Phase 8: If JSON output is enabled, emit in JSON format */
    if (json_output_) {
        diag_emit_json(level, primary_span, DIAG_CODE_NONE, message);
        for (size_t i = 0; i < secondary_count; i++) {
            diag_emit_json(DIAG_NOTE, secondary_spans[i], DIAG_CODE_NONE,
                           secondary_labels ? secondary_labels[i] : "");
        }
        return;
    }

    const SourceFile *f = NULL;
    if (primary_span.file_id < MAX_FILES) f = files_[primary_span.file_id];
    const char *path = f ? f->path : "<unknown>";

    const char *color = color_for_level(level);
    const char *reset = use_color_ ? COLOR_RESET : "";
    
    /* Print primary error with label */
    fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", color, path, primary_span.line, primary_span.col_start,
            level_name(level), reset, message);
    
    /* Print primary snippet with label */
    if (f && primary_span.off_start <= f->len) {
        /* Render snippet */
        SnippetOpts opts = SNIPPET_OPTS_DEFAULT;
        render_snippet_ex(f, primary_span, &opts);
        
        /* Print label on its own line if we have one */
        if (primary_label) {
            int line_num_width = 1;
            uint32_t temp = primary_span.line + opts.context_lines;
            while (temp >= 10) { line_num_width++; temp /= 10; }
            
            fprintf(stderr, "%*s | %*s%s%s\n", line_num_width, "", 
                    (int)(primary_span.col_start - 1), "", color, primary_label);
            fprintf(stderr, "%s", reset);
        }
    }
    
    /* Print secondary spans */
    for (size_t i = 0; i < secondary_count; i++) {
        const SourceFile *sec_f = NULL;
        if (secondary_spans[i].file_id < MAX_FILES) sec_f = files_[secondary_spans[i].file_id];
        const char *sec_path = sec_f ? sec_f->path : "<unknown>";
        const char *note_color = color_for_level(DIAG_NOTE);
        
        fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n", note_color, sec_path,
                secondary_spans[i].line, secondary_spans[i].col_start,
                level_name(DIAG_NOTE), reset, secondary_labels ? secondary_labels[i] : "");
        
        if (sec_f && secondary_spans[i].off_start <= sec_f->len) {
            SnippetOpts sec_opts = SNIPPET_OPTS_DEFAULT;
            sec_opts.primary_style = UNDERLINE_SECONDARY;
            render_snippet_ex(sec_f, secondary_spans[i], &sec_opts);
        }
    }
}

/* JSON diagnostics support (Phase 8) */

void diag_set_json_output(bool enabled) {
    json_output_ = enabled;
}

/* Escape a string for JSON output */
static void json_escape_string(Buf *b, const char *s) {
    if (!s) {
        buf_puts(b, "null");
        return;
    }
    buf_putc(b, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\b': buf_puts(b, "\\b");  break;
            case '\f': buf_puts(b, "\\f");  break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    buf_printf(b, "\\u%04x", (unsigned char)*p);
                } else {
                    buf_putc(b, *p);
                }
        }
    }
    buf_putc(b, '"');
}

/* Emit a diagnostic in JSON format */
void diag_emit_json(DiagLevel level, Span span, DiagCode code, const char *message) {
    const SourceFile *f = NULL;
    if (span.file_id < MAX_FILES) f = files_[span.file_id];
    const char *file = f ? f->path : "<unknown>";
    const char *severity = level_name(level);
    const char *code_str = diag_code_to_string(code);
    
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\n");
    buf_printf(&b, "  \"severity\": ");
    json_escape_string(&b, severity);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"code\": ");
    json_escape_string(&b, code != DIAG_CODE_NONE ? code_str : "");
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"message\": ");
    json_escape_string(&b, message);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"file\": ");
    json_escape_string(&b, file);
    buf_printf(&b, ",\n");
    buf_printf(&b, "  \"line\": %u,\n", span.line);
    buf_printf(&b, "  \"col\": %u,\n", span.col_start);
    buf_printf(&b, "  \"endLine\": %u,\n", span.line);
    buf_printf(&b, "  \"endCol\": %u\n", span.col_end);
    buf_puts(&b, "}\n");
    
    fwrite(b.data, 1, b.len, stderr);
    buf_free(&b);
}


