#ifndef TUR_LIFETIMES_H
#define TUR_LIFETIMES_H

#include <stdint.h>
#include <stdbool.h>

/* Lifetime variable representation. Lifetimes are denoted with leading '.
 * Internally, we represent them as small integer IDs. */
typedef uint8_t LifetimeId;

/* Sentinel value for invalid/no lifetime */
#define LIFETIME_NONE 0

/* Maximum number of lifetime parameters per function/struct */
#define MAX_LIFETIMES 8

/* Outlives constraint: 'a: 'b means 'a outlives 'b */
typedef struct LifetimeConstraint {
    LifetimeId parent;   /* The longer lifetime */
    LifetimeId child;    /* The shorter lifetime */
} LifetimeConstraint;

/* Lifetime context for a function or struct definition */
typedef struct LifetimeContext {
    LifetimeId ids[MAX_LIFETIMES];        /* Array of lifetime parameter IDs */
    uint8_t   count;                     /* Number of lifetime parameters */
    LifetimeConstraint constraints[MAX_LIFETIMES * 2];  /* Outlives constraints */
    uint8_t   n_constraints;              /* Number of constraints */
    /* LS0: name->ID resolution.  names[i] is the source spelling (including the
     * leading apostrophe, e.g. "'a") of the lifetime whose ID is ids[i], or NULL
     * for anonymous (elision-created) lifetimes.  Pointers must be stable for the
     * context's lifetime -- pass interned/arena-allocated strings. */
    const char *names[MAX_LIFETIMES];
} LifetimeContext;

/* Initialize a lifetime context */
void lifetime_context_init(LifetimeContext *ctx);

/* Add a lifetime parameter to the context, returns its ID */
LifetimeId lifetime_context_add(LifetimeContext *ctx);

/* LS0: resolve a named lifetime (e.g. "'a") to a stable LifetimeId within this
 * context.  Interning is idempotent: a repeated name returns the same ID, while
 * a distinct name allocates a fresh one (matching how function type variables
 * are implicitly quantified).  `name` must point to storage that outlives the
 * context (interned symbol text or arena memory).  Returns LIFETIME_NONE if the
 * per-function lifetime cap (MAX_LIFETIMES) is exhausted. */
LifetimeId lifetime_context_intern(LifetimeContext *ctx, const char *name);

/* Add an outlives constraint: parent: child */
void lifetime_context_add_constraint(LifetimeContext *ctx, LifetimeId parent, LifetimeId child);

/* Check if lifetime a outlives lifetime b in this context.
 * Cycle-safe: a constraint cycle is treated as "not outliving" rather than
 * recursing forever (use lifetime_has_cycle to reject cycles up front). */
bool lifetime_outlives(const LifetimeContext *ctx, LifetimeId a, LifetimeId b);

/* TY4.2: returns true if the outlives-constraint graph contains a cycle
 * (e.g. 'a: 'b, 'b: 'a).  A cycle means the constraints are unsatisfiable --
 * two distinct lifetimes each required to outlive the other.  When a cycle is
 * found and out_a/out_b are non-NULL, they receive two lifetimes on the cycle
 * for diagnostics. */
bool lifetime_has_cycle(const LifetimeContext *ctx, LifetimeId *out_a, LifetimeId *out_b);

/* Free a lifetime context */
void lifetime_context_free(LifetimeContext *ctx);

/* Lifetimes are purely static - no runtime representation needed */

#endif
