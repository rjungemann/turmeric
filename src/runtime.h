#ifndef TUR_RUNTIME_H
#define TUR_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration for effect row (future-proofing for v3 effects).
 * This matches the forward declaration in types.h. */
struct EffectRow;

/* A defer thunk - a function pointer that takes no arguments and returns void. */
typedef void (*defer_fn_t)(void);

/* Maximum number of defers per frame. This is a compile-time limit.
 * In practice, scopes rarely have more than a handful of defers. */
#define TUR_FRAME_MAX_DEFERS 32

/* A stack-allocated frame for tracking defers in a scope.
 * Each scope (let/defn/while body) gets one of these.
 * 
 * Per effects-plan.md §6.10, this is the unified defer model that makes
 * the S1/S2/S3 strategy choice a runtime policy decision.
 * 
 * Fields:
 *   defers:     Array of defer thunk function pointers
 *   n:          Number of active defers in the list
 *   parent:     Pointer to the enclosing frame (for scope unwind)
 *   may_capture: Future-proofing for v3 effects - whether this frame may
 *                be captured by a continuation (always false in v0/v1)
 *   effect_row: Future-proofing for v3 effects - effect row for this frame
 *                (NULL in v0/v1)
 */
typedef struct tur_frame {
    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];
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
 * Returns 0 on success, -1 if the frame is full (shouldn't happen in practice). */
static inline int tur_frame_push_defer(tur_frame *f, defer_fn_t thunk) {
    if (f->n >= TUR_FRAME_MAX_DEFERS) {
        return -1; /* Frame full */
    }
    f->defers[f->n++] = thunk;
    return 0;
}

/* Fire all defers in LIFO order and reset the frame.
 * This is called at scope exit (normal, return, or error). */
void tur_frame_fire_lifo(tur_frame *f);

/* Fire all defers and walk up the parent chain firing each frame's defers.
 * This is used for unwinding through multiple scope levels (e.g., on return). */
void tur_frame_fire_chain(tur_frame *f);

#endif /* TUR_RUNTIME_H */
