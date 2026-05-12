#ifndef TUR_RUNTIME_H
#define TUR_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration for TypeKind from types.h */
/* We can't forward-declare enums in C, so we use int for type_tag */
typedef int TypeKindInt;

/* Forward declaration for effect row (future-proofing for v3 effects).
 * This matches the forward declaration in types.h. */
struct EffectRow;

/* A defer thunk - a function pointer that takes an env pointer and returns void.
 * 
 * Per effects-plan.md §6.10, this is the unified defer model that makes
 * the S1/S2/S3 strategy choice a runtime policy decision rather than an
 * architectural rewrite. The thunk is registered with tur_frame_push_defer
 * and invoked by tur_frame_fire_lifo at scope exit.
 * 
 * For v1, thunks take a void* env parameter to support captures.
 * For defers without captures, NULL is passed (though the thunk ignores it). */
typedef void (*defer_fn_t)(void *env);

/* Maximum number of defers per frame. This is a compile-time limit.
 * In practice, scopes rarely have more than a handful of defers. */
#define TUR_FRAME_MAX_DEFERS 32

/* A stack-allocated frame for tracking defers in a scope.
 * Each scope (let/defn/while body) gets one of these.
 * 
 * Per effects-plan.md §6.10.1, this is the unified defer model that makes
 * the S1/S2/S3 strategy choice (§6.10.2) a runtime policy decision rather
 * than an architectural rewrite.
 * 
 * Fields:
 *   defers:     Array of defer thunk function pointers (max TUR_FRAME_MAX_DEFERS).
 *               Thunks are registered via tur_frame_push_defer and fired in LIFO
 *               order by tur_frame_fire_lifo.
 *   envs:       Array of env pointers for defers with captures (parallel to defers).
 *               For defers without captures, the corresponding env is NULL.
 *   n:          Number of active defers in the list.
 *   parent:     Pointer to the enclosing frame (for scope unwind chain).
 *               Used by tur_frame_fire_chain to unwind through nested scopes.
 *   may_capture: Future-proofing for v3 effects (§6.10) - whether this frame may
 *                be captured by a continuation. Always false in v0/v1.
 *   effect_row: Future-proofing for v3 effects (§6.10) - effect row for this frame.
 *                NULL in v0/v1, treated as empty effect set.
 */
typedef struct tur_frame {
    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];
    void *envs[TUR_FRAME_MAX_DEFERS];      /* Env pointers for captured defers */
    int n;
    struct tur_frame *parent;
    /* Future-proofing fields - unused in v0/v1 but reserved for v3 effects */
    bool may_capture;
    struct EffectRow *effect_row;
} tur_frame;

/* Initialize a frame. Should be called on stack-allocated frames.
 * parent: The enclosing frame, or NULL if this is the outermost frame. */
static inline void tur_frame_init(tur_frame *f, tur_frame *parent) {
    f->n = 0;
    f->parent = parent;
    f->may_capture = false;
    f->effect_row = NULL;
}

/* Push a defer thunk onto the frame's defer list.
 * The thunk will be called in LIFO order when the frame is fired.
 * env: The environment pointer for captured defers, or NULL if no captures.
 * Returns 0 on success, -1 if the frame is full (shouldn't happen in practice). */
static inline int tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env) {
    if (f->n >= TUR_FRAME_MAX_DEFERS) {
        return -1; /* Frame full */
    }
    f->defers[f->n] = thunk;
    f->envs[f->n] = env;
    f->n++;
    return 0;
}

/* Fire all defers in LIFO order and reset the frame.
 * This is called at scope exit (normal, return, or error). */
void tur_frame_fire_lifo(tur_frame *f);

/* Fire all defers and walk up the parent chain firing each frame's defers.
 * This is used for unwinding through multiple scope levels (e.g., on return). */
void tur_frame_fire_chain(tur_frame *f);

/* Phase 18: Delimited continuations */

/* Maximum number of captured frames per continuation. */
#define TUR_CONT_MAX_CAPTURED_FRAMES 16

/* A continuation frame for delimited continuations (shift/reset).
 * 
 * A continuation captures the execution state at the point of shift,
 * allowing it to be resumed later. Continuations are one-shot (move-only).
 * 
 * Fields:
 *   cont_fn:      Function pointer to the continuation trampoline.
 *   env:          Environment captured by the continuation.
 *   parent:       Parent continuation (for nested reset boundaries).
 *   n_captured:   Number of tur_frame structures captured by this continuation.
 *   captured:     Array of captured tur_frame pointers.
 *   consumed:     Whether this continuation has been resumed (one-shot enforcement).
 */
typedef struct tur_cont {
    void (*cont_fn)(void *env, int64_t value);  /* Function to call to resume */
    void *env;                                     /* Captured environment */
    struct tur_cont *parent;                      /* Parent continuation */
    tur_frame *captured[TUR_CONT_MAX_CAPTURED_FRAMES];
    int n_captured;
    bool consumed;  /* One-shot: true after resume */
} tur_cont;

/* Allocate a new continuation with captured frame chain.
 * frame_chain: Array of tur_frame pointers to capture.
 * n_frames:   Number of frames to capture.
 * Returns:     New tur_cont, or NULL on error. */
tur_cont *tur_cont_alloc(tur_frame **frame_chain, int n_frames);

/* Resume a continuation with a value. Consumes the continuation (one-shot).
 * cont:    The continuation to resume.
 * value:   The value to pass to the continuation.
 * Note:    After calling this, the continuation is marked as consumed and cannot
 *          be resumed again. */
void tur_cont_resume(tur_cont *cont, int64_t value);

/* Drop a continuation without resuming it. Fires defers on captured frames.
 * cont:    The continuation to drop.
 * Note:    This is called when a continuation is garbage collected without being resumed. */
void tur_cont_drop(tur_cont *cont);

/* Check if a continuation has been consumed.
 * cont:    The continuation to check.
 * Returns: true if the continuation has been resumed, false otherwise. */
bool tur_cont_consumed(tur_cont *cont);

/* Phase R2: Panic — print message to stderr, then abort.
 * Re-entry (double panic) calls abort() immediately. */
extern int tur_panic_in_progress;
void tur_panic(const char *msg);

/* Phase R2: Panic with typed payload */
/* TypeKind is an enum from types.h; we use int for type_tag in runtime.h */

/* Payload carried by a panic. This is the runtime representation of a
 * typed panic value, allowing catch-panic-of to filter by type. */
typedef struct tur_panic_payload {
    TypeKindInt type_tag;   /* TypeKind of the panicked value (as int) */
    void *value;            /* Boxed payload value (heap-allocated) */
    const char *file;       /* Source file where panic occurred */
    int line;               /* Source line where panic occurred */
} tur_panic_payload;

/* Panic with a typed payload. The payload is boxed on the heap.
 * type_tag: The TypeKind of the payload (as int).
 * payload:  The boxed value (ownership transferred to tur_panic_with).
 * file:     Source file location.
 * line:     Source line location. */
void tur_panic_with(TypeKindInt type_tag, void *payload, const char *file, int line);

/* Result type for catch-unwind/catch-panic-of: holds either Ok(value) or Err(payload) */
typedef enum tur_result_tag {
    TUR_RESULT_OK,
    TUR_RESULT_ERR,
} tur_result_tag;

/* Generic result type used by catch-unwind */
typedef struct tur_result {
    tur_result_tag tag;
    union {
        int64_t ok_val;           /* For simple integer returns (Phase R2: simplified) */
        void *ok_ptr;            /* For pointer returns */
        tur_panic_payload *err;  /* For panic payloads */
    } u;
} tur_result;

/* Typed callback for thunk in catch-unwind/catch-panic-of */
typedef void (*tur_thunk_fn)(void *env, tur_result *out);

/* Catch any panic at a boundary.
 * thunk:   The function to call. Takes env pointer and result output pointer.
 * env:     Environment to pass to thunk.
 * out:     Output result struct.
 * Returns: true if a panic was caught, false if thunk completed normally.
 * Note:    This uses setjmp/longjmp internally. */
bool tur_catch_unwind(tur_thunk_fn thunk, void *env, tur_result *out);

/* Catch panics of a specific type at a boundary.
 * expected_type: Only catch panics with this TypeKind (as int).
 * thunk:        The function to call.
 * env:          Environment to pass to thunk.
 * out:          Output result struct.
 * Returns:       true if a panic of matching type was caught, false otherwise
 *               (including if a panic of different type occurred, which re-panics). */
bool tur_catch_panic_of(TypeKindInt expected_type, tur_thunk_fn thunk, void *env, tur_result *out);

/* Accessors for panic payload */
TypeKindInt tur_panic_payload_type(tur_panic_payload *p);
void *tur_panic_payload_value(tur_panic_payload *p);
const char *tur_panic_payload_file(tur_panic_payload *p);
int tur_panic_payload_line(tur_panic_payload *p);

/* Downcast a panic payload to a specific type.
 * p:         The panic payload to downcast.
 * target_type: The TypeKind to cast to (as int).
 * Returns:   The boxed value if type matches, NULL otherwise. */
void *tur_panic_payload_downcast(tur_panic_payload *p, TypeKindInt target_type);

#endif /* TUR_RUNTIME_H */
