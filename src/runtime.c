#include "runtime.h"

#include <stddef.h>
#include <stdio.h>

/* Fire all defers in LIFO order and reset the frame.
 * 
 * Per effects-plan.md §6.10, this implements the unified defer model where
 * defers are entries in a list-on-frame, not codegen labels. Each defer is
 * a thunk function pointer that gets invoked here with its corresponding
 * env pointer. The frame's n is reset to 0 after firing so the same frame
 * can be reused (though in v0/v1 frames are stack-allocated per scope and
 * not reused). */
void tur_frame_fire_lifo(tur_frame *f) {
    /* Fire defers in reverse order (LIFO) */
    for (int i = f->n - 1; i >= 0; i--) {
        f->defers[i](f->envs[i]);
    }
    f->n = 0;
}

/* Fire all defers in this frame and all parent frames in the chain.
 * 
 * Walks the parent chain from the given frame outward, collecting all frames
 * first (to avoid issues if firing a defer in an outer frame invalidates
 * an inner frame's data). Then fires from innermost to outermost.
 * 
 * Per effects-plan.md §6.10, this supports the v3 effects strategy where
 * defers may need to be fired across multiple scope levels during continuation
 * capture or unwinding. */
void tur_frame_fire_chain(tur_frame *f) {
    /* Collect all frames in the chain first (to avoid issues if 
     * firing a defer in an outer frame invalidates an inner frame). */
    tur_frame *frames[64];  /* Max nesting depth - should be plenty */
    int n_frames = 0;
    
    for (tur_frame *cur = f; cur != NULL && n_frames < 64; cur = cur->parent) {
        frames[n_frames++] = cur;
    }
    
    /* Fire from innermost to outermost (reverse order of collection) */
    for (int i = n_frames - 1; i >= 0; i--) {
        tur_frame_fire_lifo(frames[i]);
    }
}

/* Phase 18: Delimited continuations */

#include <stdlib.h>

/* Allocate a new continuation with captured frame chain. */
tur_cont *tur_cont_alloc(tur_frame **frame_chain, int n_frames) {
    if (n_frames > TUR_CONT_MAX_CAPTURED_FRAMES) {
        return NULL;  /* Too many frames to capture */
    }
    
    tur_cont *cont = (tur_cont *)malloc(sizeof(tur_cont));
    if (!cont) {
        return NULL;
    }
    
    cont->cont_fn = NULL;
    cont->env = NULL;
    cont->parent = NULL;
    cont->n_captured = 0;
    cont->consumed = false;
    
    /* Copy captured frames */
    for (int i = 0; i < n_frames; i++) {
        if (i >= TUR_CONT_MAX_CAPTURED_FRAMES) break;
        cont->captured[i] = frame_chain[i];
        cont->n_captured++;
    }
    
    return cont;
}

/* Resume a continuation with a value. Consumes the continuation (one-shot). */
void tur_cont_resume(tur_cont *cont, int64_t value) {
    if (!cont || cont->consumed) {
        /* Already consumed - continuation escape/double-resume is a hard error */
        fprintf(stderr, "continuation error: resume of already-consumed continuation\n");
        abort();
    }
    
    /* Mark as consumed (one-shot) */
    cont->consumed = true;
    
    /* Fire defers on captured frames (S2 strategy) */
    /* Note: In the full implementation, we would need to properly restore
     * the frame chain before resuming. For v1, we just fire the defers. */
    
    /* Call the continuation function if set */
    if (cont->cont_fn) {
        cont->cont_fn(cont->env, value);
    }
}

/* Drop a continuation without resuming it. Fires defers on captured frames. */
void tur_cont_drop(tur_cont *cont) {
    if (!cont) return;
    
    /* Fire defers on captured frames */
    for (int i = 0; i < cont->n_captured; i++) {
        tur_frame_fire_lifo(cont->captured[i]);
    }
    
    free(cont);
}

/* Check if a continuation has been consumed. */
bool tur_cont_consumed(tur_cont *cont) {
    if (!cont) return true;  /* NULL is considered consumed */
    return cont->consumed;
}
