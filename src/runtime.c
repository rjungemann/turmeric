#include "runtime.h"

#include <stddef.h>

/* Fire all defers in LIFO order and reset the frame. */
void tur_frame_fire_lifo(tur_frame *f) {
    /* Fire defers in reverse order (LIFO) */
    for (int i = f->n - 1; i >= 0; i--) {
        f->defers[i]();
    }
    f->n = 0;
}

/* Fire all defers in this frame and all parent frames in the chain. */
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
