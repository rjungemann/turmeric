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

/* Phase R2: Panic */
int tur_panic_in_progress = 0;

void tur_panic(const char *msg) {
    if (tur_panic_in_progress) {
        fprintf(stderr, "double panic: aborting\n");
        abort();
    }
    tur_panic_in_progress = 1;
    fprintf(stderr, "panic at %s:%d: %s\n", __FILE__, __LINE__, msg ? msg : "(no message)");
    abort();
}

/* Phase R5: Panic strategy - ABORT variant for #[no-unwind] */
void tur_panic_abort(const char *msg) {
    /* Abort immediately without unwinding - used for #[no-unwind] functions */
    fprintf(stderr, "panic (no unwind): %s\n", msg ? msg : "(no message)");
    abort();
}

/* Phase R2: Panic with typed payload */

#include <setjmp.h>
#include <string.h>

/* Global panic payload storage for the current panic (used with setjmp/longjmp) */
static tur_panic_payload *global_panic_payload = NULL;
static jmp_buf global_panic_jmpbuf;
static int global_panic_jmpbuf_valid = 0;

/* Allocate and initialize a panic payload */
static tur_panic_payload *panic_payload_new(TypeKindInt type_tag, void *payload, const char *file, int line) {
    tur_panic_payload *p = (tur_panic_payload *)malloc(sizeof(tur_panic_payload));
    if (!p) {
        fprintf(stderr, "panic: failed to allocate panic payload\n");
        abort();
    }
    p->type_tag = type_tag;
    p->value = payload;
    p->file = file;
    p->line = line;
    return p;
}

/* Free a panic payload */
static void panic_payload_free(tur_panic_payload *p) {
    if (p) {
        free(p->value);  /* Free the boxed payload */
        free(p);
    }
}

/* Panic with a typed payload. The payload is boxed on the heap. */
void tur_panic_with(TypeKindInt type_tag, void *payload, const char *file, int line) {
    if (tur_panic_in_progress) {
        fprintf(stderr, "double panic: aborting\n");
        free(payload);  /* Clean up the allocated payload */
        abort();
    }
    
    tur_panic_in_progress = 1;
    
    /* If we're inside a catch_unwind boundary (setjmp established), 
     * longjmp back to it instead of aborting. */
    if (global_panic_jmpbuf_valid) {
        /* Store the payload for retrieval by the catch boundary */
        global_panic_payload = panic_payload_new(type_tag, payload, file, line);
        longjmp(global_panic_jmpbuf, 1);
    }
    
    /* Otherwise, print error and abort */
    fprintf(stderr, "panic at %s:%d\n", file ? file : "(unknown)", line);
    free(payload);
    abort();
}

/* Accessors for panic payload */
TypeKindInt tur_panic_payload_type(tur_panic_payload *p) {
    return p ? p->type_tag : 0;
}

void *tur_panic_payload_value(tur_panic_payload *p) {
    return p ? p->value : NULL;
}

const char *tur_panic_payload_file(tur_panic_payload *p) {
    return p ? p->file : NULL;
}

int tur_panic_payload_line(tur_panic_payload *p) {
    return p ? p->line : 0;
}

/* Downcast a panic payload to a specific type */
void *tur_panic_payload_downcast(tur_panic_payload *p, TypeKindInt target_type) {
    if (!p || p->type_tag != target_type) {
        return NULL;
    }
    return p->value;
}

/* Catch any panic at a boundary using setjmp/longjmp */
bool tur_catch_unwind(tur_thunk_fn thunk, void *env, tur_result *out) {
    if (global_panic_jmpbuf_valid) {
        /* Nested catch_unwind not supported in v1 - just call thunk directly */
        thunk(env, out);
        return false;
    }
    
    global_panic_jmpbuf_valid = 1;
    
    /* Set up the setjmp buffer */
    if (setjmp(global_panic_jmpbuf) == 0) {
        /* Normal path - call the thunk */
        thunk(env, out);
        global_panic_jmpbuf_valid = 0;
        
        /* Clean up any stored panic payload (shouldn't happen but be safe) */
        if (global_panic_payload) {
            panic_payload_free(global_panic_payload);
            global_panic_payload = NULL;
        }
        
        out->tag = TUR_RESULT_OK;
        return false;  /* No panic occurred */
    } else {
        /* Panic occurred - set up error result */
        global_panic_jmpbuf_valid = 0;
        
        out->tag = TUR_RESULT_ERR;
        out->u.err = global_panic_payload;
        global_panic_payload = NULL;
        
        return true;  /* Panic was caught */
    }
}

/* Catch panics of a specific type at a boundary */
bool tur_catch_panic_of(TypeKindInt expected_type, tur_thunk_fn thunk, void *env, tur_result *out) {
    if (global_panic_jmpbuf_valid) {
        /* Nested catch_panic_of not supported in v1 - just call thunk directly */
        thunk(env, out);
        return false;
    }
    
    global_panic_jmpbuf_valid = 1;
    
    if (setjmp(global_panic_jmpbuf) == 0) {
        /* Normal path - call the thunk */
        thunk(env, out);
        global_panic_jmpbuf_valid = 0;
        
        if (global_panic_payload) {
            panic_payload_free(global_panic_payload);
            global_panic_payload = NULL;
        }
        
        out->tag = TUR_RESULT_OK;
        return false;  /* No panic occurred */
    } else {
        /* Panic occurred - check type */
        global_panic_jmpbuf_valid = 0;
        
        if (global_panic_payload && global_panic_payload->type_tag == expected_type) {
            /* Type matches - set up error result */
            out->tag = TUR_RESULT_ERR;
            out->u.err = global_panic_payload;
            global_panic_payload = NULL;
            return true;  /* Panic of matching type was caught */
        } else {
            /* Type mismatch - re-panic with the original payload */
            if (global_panic_payload) {
                tur_panic_payload *p = global_panic_payload;
                global_panic_payload = NULL;
                tur_panic_with(p->type_tag, p->value, p->file, p->line);
                /* Never reached */
                return false;
            } else {
                /* No payload but setjmp returned non-zero - shouldn't happen */
                fprintf(stderr, "panic: spurious catch_panic_of activation\n");
                abort();
            }
        }
    }
}
