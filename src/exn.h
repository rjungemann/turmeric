#ifndef TUR_EXN_H
#define TUR_EXN_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include "types.h"
#include "forms.h"  /* For Span - source location tracking */

/* Forward declaration */
typedef struct tur_exception tur_exception;

/* Exception value structure
 * Represents an exception that has been thrown but not yet caught
 * 
 * Phase 17: Exceptions are non-resumable (one-shot)
 * The payload is a void* that points to the actual exception data
 * The type field indicates what type of value the payload is
 * The where field tracks where the exception was thrown (for diagnostics) */
struct tur_exception {
    TypeKind payload_type;      /* The Turmeric type of the payload */
    void *payload;             /* The actual exception value (heap-allocated) */
    Span where;                /* Where the exception was thrown */
    /* For stack trace support (optional in v1, always in debug builds) */
    const char *file;          /* Source file where thrown */
    int line;                  /* Line number where thrown */
    /* Chain for nested exceptions */
    tur_exception *cause;      /* Cause of this exception (NULL if none) */
};

/* Exception handler state
 * Uses setjmp/longjmp for non-resumable exceptions
 * Each try block has its own jmp_buf for catching exceptions */
typedef struct ExceptionHandler {
    jmp_buf jmp_buf;           /* setjmp buffer for catching exceptions */
    int active;               /* 1 if handler is active, 0 if exception was caught */
    tur_exception *caught;     /* The caught exception (NULL if none) */
    struct ExceptionHandler *parent; /* Parent handler (for nested try blocks) */
} ExceptionHandler;

/* Global exception state
 * Thread-local storage for the current exception being propagated
 * and the current exception handler chain */

/* Get the current exception handler chain (thread-local) */
ExceptionHandler *exn_current_handler(void);

/* Set the current exception handler chain (thread-local) */
void exn_set_current_handler(ExceptionHandler *handler);

/* Push a new exception handler onto the chain */
ExceptionHandler *exn_push_handler(void);

/* Pop the current exception handler from the chain */
ExceptionHandler *exn_pop_handler(void);

/* Throw an exception with the given payload
 * This wraps the payload in a tur_exception struct and either:
 * - longjmps to the nearest catch handler, or
 * - if no handler, prints error and exits
 * 
 * payload_type: The Turmeric type of the payload (TY_INT, TY_BOOL, etc.)
 * payload: The actual value (will be copied/heap-allocated if needed)
 * where: Source span where the throw occurred
 * file: Source file name
 * line: Line number
 * 
 * Returns: Never returns normally (always longjmps or exits) */
void tur_throw(TypeKind payload_type, void *payload, Span where, const char *file, int line);

/* Re-throw the current caught exception
 * Used inside catch blocks to re-throw the caught exception */
void tur_rethrow(tur_exception *exn);

/* Check if an exception matches a catch clause
 * Returns true if the exception's payload type matches the expected type */
bool tur_exception_matches(tur_exception *exn, TypeKind expected_type);

/* Free an exception and its payload */
void tur_exception_free(tur_exception *exn);

/* Initialize the exception system (call once at program start) */
void exn_init(void);

/* Cleanup the exception system */
void exn_cleanup(void);

#endif  /* TUR_EXN_H */
