#include "lifetimes.h"

#include <string.h>

void lifetime_context_init(LifetimeContext *ctx) {
    ctx->count = 0;
    ctx->n_constraints = 0;
}

LifetimeId lifetime_context_add(LifetimeContext *ctx) {
    if (ctx->count >= MAX_LIFETIMES) {
        return LIFETIME_NONE;  /* Error: too many lifetime parameters */
    }
    /* Assign a new unique ID (1-based to avoid LIFETIME_NONE) */
    LifetimeId id = ctx->count + 1;
    ctx->ids[ctx->count++] = id;
    return id;
}

void lifetime_context_add_constraint(LifetimeContext *ctx, LifetimeId parent, LifetimeId child) {
    if (ctx->n_constraints >= MAX_LIFETIMES * 2) {
        return;  /* Error: too many constraints */
    }
    if (parent == LIFETIME_NONE || child == LIFETIME_NONE) {
        return;
    }
    /* Check if this constraint already exists */
    for (uint8_t i = 0; i < ctx->n_constraints; i++) {
        if (ctx->constraints[i].parent == parent && ctx->constraints[i].child == child) {
            return;
        }
    }
    ctx->constraints[ctx->n_constraints].parent = parent;
    ctx->constraints[ctx->n_constraints].child = child;
    ctx->n_constraints++;
}

/* Check if there's a transitive path from a to b (a outlives b) */
bool lifetime_outlives(const LifetimeContext *ctx, LifetimeId a, LifetimeId b) {
    if (a == b) return true;
    if (a == LIFETIME_NONE || b == LIFETIME_NONE) return false;
    
    /* Simple O(n^2) check for direct and transitive constraints */
    /* For a production implementation, we'd use a more efficient algorithm */
    for (uint8_t i = 0; i < ctx->n_constraints; i++) {
        if (ctx->constraints[i].parent == a && ctx->constraints[i].child == b) {
            return true;
        }
    }
    /* Check transitive: if a outlives c and c outlives b, then a outlives b */
    for (uint8_t i = 0; i < ctx->n_constraints; i++) {
        if (ctx->constraints[i].parent == a) {
            LifetimeId mid = ctx->constraints[i].child;
            if (lifetime_outlives(ctx, mid, b)) {
                return true;
            }
        }
    }
    return false;
}

void lifetime_context_free(LifetimeContext *ctx) {
    /* Nothing to free - all inline storage */
    ctx->count = 0;
    ctx->n_constraints = 0;
}
