#ifndef TUR_RUNTIME_H
#define TUR_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#endif /* TUR_RUNTIME_H */
