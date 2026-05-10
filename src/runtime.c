#include "runtime.h"

#include <stddef.h>

/* Fire all defers in LIFO order and reset the frame.
 * 
 * Per effects-plan.md §6.10, this implements the unified defer model where
 * defers are entries in a list-on-frame, not codegen labels. Each defer is
 * a thunk function pointer that gets invoked here. The frame's n is reset to
 * 0 after firing so the same frame can be reused (though in v0/v1 frames are
 * stack-allocated per scope and not reused). */
void tur_frame_fire_lifo(tur_frame *f) {
    /* Fire defers in reverse order (LIFO) */
    for (int i = f->n - 1; i >= 0; i--) {
        f->defers[i]();
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
