#include "exn.h"
#include "arena.h"
#include "forms.h"  /* For Span */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local storage for the exception handler chain
 * In v1 (single-threaded), we use a simple global variable
 * In future multi-threaded versions, this would be __thread or pthread_key_t */
static ExceptionHandler *global_handler_chain = NULL;

/* ============================================================================
 * Exception handler chain management
 * ============================================================================ */

ExceptionHandler *exn_current_handler(void) {
    return global_handler_chain;
}

void exn_set_current_handler(ExceptionHandler *handler) {
    global_handler_chain = handler;
}

ExceptionHandler *exn_push_handler(void) {
    ExceptionHandler *new_handler = (ExceptionHandler *)malloc(sizeof(ExceptionHandler));
    if (!new_handler) {
        fprintf(stderr, "exn_push_handler: out of memory\n");
        abort();
    }
    new_handler->active = 1;
    new_handler->caught = NULL;
    new_handler->parent = global_handler_chain;
    global_handler_chain = new_handler;
    return new_handler;
}

ExceptionHandler *exn_pop_handler(void) {
    ExceptionHandler *old = global_handler_chain;
    if (old) {
        global_handler_chain = old->parent;
    }
    return old;
}

/* ============================================================================
 * Exception creation and destruction
 * ============================================================================ */

/* Allocate a new exception structure */
static tur_exception *exception_alloc(void) {
    tur_exception *exn = (tur_exception *)malloc(sizeof(tur_exception));
    if (!exn) {
        fprintf(stderr, "exception_alloc: out of memory\n");
        abort();
    }
    memset(exn, 0, sizeof(tur_exception));
    return exn;
}

/* Free an exception and its payload
 * For primitive types (int, bool), the payload is just a value on the stack
 * For heap types (cstr, ref, rc), the payload is a pointer that needs freeing
 * For TY_NIL, no payload to free */
void tur_exception_free(tur_exception *exn) {
    if (!exn) return;
    
    /* Free payload based on its type */
    switch (exn->payload_type) {
        case TY_CSTR:
        case TY_PTR_VOID:
            free(exn->payload);
            break;
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
            /* These are already pointers - free the contained value */
            free(exn->payload);
            break;
        /* Primitive types and other types: payload is on stack, no free needed */
        default:
            break;
    }
    
    /* Free chained cause */
    if (exn->cause) {
        tur_exception_free(exn->cause);
    }
    
    free(exn);
}

/* ============================================================================
 * Exception throwing and catching
 * ============================================================================ */

/* Copy payload to heap if needed
 * For primitive types that are passed by value, we need to heap-allocate
 * so the exception can own the payload */
static void *copy_payload_to_heap(TypeKind type, void *payload) {
    if (!payload) return NULL;
    
    switch (type) {
        case TY_INT: {
            int64_t *p = (int64_t *)malloc(sizeof(int64_t));
            if (p) *p = *(int64_t *)payload;
            return p;
        }
        case TY_BOOL: {
            bool *p = (bool *)malloc(sizeof(bool));
            if (p) *p = *(bool *)payload;
            return p;
        }
        /* Pointer types are already heap-allocated or literals */
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
            return payload;  /* Already a pointer, don't copy */
        default:
            /* For unknown types, try to just return the payload as-is */
            return payload;
    }
}

/* Throw an exception
 * Wraps the payload and longjmps to the nearest handler */
void tur_throw(TypeKind payload_type, void *payload, Span where, const char *file, int line) {
    tur_exception *exn = exception_alloc();
    exn->payload_type = payload_type;
    exn->payload = copy_payload_to_heap(payload_type, payload);
    exn->where = where;
    exn->file = file ? file : "<unknown>";
    exn->line = line;
    exn->cause = NULL;
    
    ExceptionHandler *handler = global_handler_chain;
    if (handler) {
        /* Store the exception in the handler */
        if (handler->caught) {
            tur_exception_free(handler->caught);
        }
        handler->caught = exn;
        handler->active = 0;  /* Mark as caught */
        
        /* Long jump to the catch handler */
        longjmp(handler->jmp_buf, 1);
    } else {
        /* No handler - print error and exit */
        fprintf(stderr, "Uncaught exception thrown at %s:%d\n", file, line);
        tur_exception_free(exn);
        exit(1);
    }
}

/* Re-throw a caught exception */
void tur_rethrow(tur_exception *exn) {
    if (!exn) {
        fprintf(stderr, "rethrow: no exception to rethrow\n");
        abort();
    }
    
    ExceptionHandler *handler = global_handler_chain;
    if (handler) {
        /* Store the exception in the parent handler (if any) or current */
        ExceptionHandler *target = handler->parent ? handler->parent : handler;
        if (target->caught) {
            tur_exception_free(target->caught);
        }
        target->caught = exn;
        target->active = 0;
        
        if (handler->parent) {
            /* Pop current handler and jump to parent */
            global_handler_chain = handler->parent;
            free(handler);
            longjmp(target->jmp_buf, 1);
        } else {
            /* Same handler, just jump again */
            longjmp(handler->jmp_buf, 1);
        }
    } else {
        /* No handler - print error and exit */
        fprintf(stderr, "Uncaught exception rethrown at %s:%d\n", exn->file, exn->line);
        tur_exception_free(exn);
        exit(1);
    }
}

/* Check if an exception matches a catch clause type */
bool tur_exception_matches(tur_exception *exn, TypeKind expected_type) {
    if (!exn) return false;
    
    /* Direct type match */
    if (exn->payload_type == expected_type) {
        return true;
    }
    
    /* For TY_NIL (void), match anything (catch-all) */
    if (expected_type == TY_NIL) {
        return true;
    }
    
    return false;
}

/* ============================================================================
 * Initialization and cleanup
 * ============================================================================ */

void exn_init(void) {
    /* Nothing to do for v1 - global_handler_chain starts as NULL */
}

void exn_cleanup(void) {
    /* Free any remaining exception handlers */
    while (global_handler_chain) {
        ExceptionHandler *h = global_handler_chain;
        global_handler_chain = h->parent;
        if (h->caught) {
            tur_exception_free(h->caught);
        }
        free(h);
    }
}
