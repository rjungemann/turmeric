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

/* Cycle-safe transitive reachability from `a` to `b` over the outlives graph.
 * `visited` marks lifetime IDs already on the current search so a constraint
 * cycle terminates instead of recursing forever. */
static bool outlives_dfs(const LifetimeContext *ctx, LifetimeId a, LifetimeId b,
                         bool *visited) {
    if (a == b) return true;
    if (a == LIFETIME_NONE || b == LIFETIME_NONE) return false;
    if (a <= MAX_LIFETIMES) {
        if (visited[a]) return false;   /* already exploring a -- cycle guard */
        visited[a] = true;
    }
    for (uint8_t i = 0; i < ctx->n_constraints; i++) {
        if (ctx->constraints[i].parent == a) {
            LifetimeId mid = ctx->constraints[i].child;
            if (mid == b) return true;
            if (outlives_dfs(ctx, mid, b, visited)) return true;
        }
    }
    return false;
}

/* Check if there's a transitive path from a to b (a outlives b). */
bool lifetime_outlives(const LifetimeContext *ctx, LifetimeId a, LifetimeId b) {
    bool visited[MAX_LIFETIMES + 1];
    memset(visited, 0, sizeof(visited));
    return outlives_dfs(ctx, a, b, visited);
}

/* TY4.2: cycle detection over the outlives-constraint graph.  A cycle exists
 * iff some lifetime can reach itself through one or more constraint edges. */
bool lifetime_has_cycle(const LifetimeContext *ctx, LifetimeId *out_a, LifetimeId *out_b) {
    for (uint8_t i = 0; i < ctx->n_constraints; i++) {
        LifetimeId p = ctx->constraints[i].parent;
        LifetimeId c = ctx->constraints[i].child;
        /* If the child can reach back to the parent, p->...->c->p is a cycle. */
        bool visited[MAX_LIFETIMES + 1];
        memset(visited, 0, sizeof(visited));
        if (outlives_dfs(ctx, c, p, visited)) {
            if (out_a) *out_a = p;
            if (out_b) *out_b = c;
            return true;
        }
    }
    return false;
}

void lifetime_context_free(LifetimeContext *ctx) {
    /* Nothing to free - all inline storage */
    ctx->count = 0;
    ctx->n_constraints = 0;
}
