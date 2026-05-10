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
} LifetimeContext;

/* Initialize a lifetime context */
void lifetime_context_init(LifetimeContext *ctx);

/* Add a lifetime parameter to the context, returns its ID */
LifetimeId lifetime_context_add(LifetimeContext *ctx);

/* Add an outlives constraint: parent: child */
void lifetime_context_add_constraint(LifetimeContext *ctx, LifetimeId parent, LifetimeId child);

/* Check if lifetime a outlives lifetime b in this context */
bool lifetime_outlives(const LifetimeContext *ctx, LifetimeId a, LifetimeId b);

/* Free a lifetime context */
void lifetime_context_free(LifetimeContext *ctx);

/* Lifetimes are purely static - no runtime representation needed */

#endif
