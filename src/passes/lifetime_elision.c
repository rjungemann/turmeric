#include "lifetime_elision.h"

#include <string.h>

/* Recursively collect lifetimes from a type */
static void type_collect_lifetimes_recursive(const Type *t, LifetimeId *out, uint8_t *n_out, uint8_t max_out) {
    if (t == NULL) return;
    
    /* Add lifetimes from this type */
    for (uint8_t i = 0; i < t->n_lifetimes; i++) {
        LifetimeId lid = t->lifetimes[i];
        if (lid == LIFETIME_NONE) continue;
        
        /* Check if already in output */
        bool found = false;
        for (uint8_t j = 0; j < *n_out; j++) {
            if (out[j] == lid) {
                found = true;
                break;
            }
        }
        if (!found && *n_out < max_out) {
            out[(*n_out)++] = lid;
        }
    }
    
    /* Recurse into inner types */
    switch (t->kind) {
        case TY_REF:
            type_collect_lifetimes_recursive(&(Type){.kind = t->as.ref.inner, .copy_kind = CK_MOVE, .n_lifetimes = 0}, out, n_out, max_out);
            break;
        case TY_RC:
        case TY_WEAK:
            type_collect_lifetimes_recursive(&(Type){.kind = t->as.rc.inner, .copy_kind = CK_MOVE, .n_lifetimes = 0}, out, n_out, max_out);
            break;
        case TY_REF_IMMUT:
        case TY_REF_MUT:
            type_collect_lifetimes_recursive(&(Type){.kind = t->as.ref_borrow.target, .copy_kind = CK_COPY, .n_lifetimes = 0}, out, n_out, max_out);
            break;
        case TY_FN:
            /* Collect from arg types and return type */
            for (uint8_t i = 0; i < t->as.fn.arity; i++) {
                Type arg_type = {(TypeKind)t->as.fn.arg_kinds[i], .copy_kind = CK_COPY, .n_lifetimes = 0};
                type_collect_lifetimes_recursive(&arg_type, out, n_out, max_out);
            }
            type_collect_lifetimes_recursive(&(Type){.kind = t->as.fn.result_kind, .copy_kind = CK_COPY, .n_lifetimes = 0}, out, n_out, max_out);
            break;
        default:
            break;
    }
}

void type_collect_lifetimes(const Type *t, LifetimeId *out, uint8_t *n_out, uint8_t max_out) {
    *n_out = 0;
    type_collect_lifetimes_recursive(t, out, n_out, max_out);
}

bool type_has_any_lifetime(const Type *t) {
    if (t->n_lifetimes > 0) return true;
    
    switch (t->kind) {
        case TY_REF:
            return type_has_any_lifetime(&(Type){.kind = t->as.ref.inner, .copy_kind = CK_MOVE, .n_lifetimes = 0});
        case TY_RC:
        case TY_WEAK:
            return type_has_any_lifetime(&(Type){.kind = t->as.rc.inner, .copy_kind = CK_MOVE, .n_lifetimes = 0});
        case TY_REF_IMMUT:
        case TY_REF_MUT:
            return t->n_lifetimes > 0 || type_has_any_lifetime(&(Type){.kind = t->as.ref_borrow.target, .copy_kind = CK_COPY, .n_lifetimes = 0});
        case TY_FN:
            for (uint8_t i = 0; i < t->as.fn.arity; i++) {
                Type arg_type = {(TypeKind)t->as.fn.arg_kinds[i], .copy_kind = CK_COPY, .n_lifetimes = 0};
                if (type_has_any_lifetime(&arg_type)) return true;
            }
            return type_has_any_lifetime(&(Type){.kind = t->as.fn.result_kind, .copy_kind = CK_COPY, .n_lifetimes = 0});
        default:
            return false;
    }
}

/* True if a type is a borrow (&T / &mut T) at its head. */
static bool type_head_is_borrow(const Type *t) {
    return t && (t->kind == TY_REF_IMMUT || t->kind == TY_REF_MUT);
}

/* Apply the classic lifetime-elision rules to a function signature, binding the
 * elided lifetimes onto the parameter and return Types (not merely counting
 * them).  The rules, applied in order:
 *
 *   Rule 1: each input borrow with no explicit lifetime gets its own fresh
 *           lifetime parameter, bound onto that parameter's Type.
 *   Rule 2: if exactly one input lifetime results, it is assigned to every
 *           elided output (return) lifetime.
 *   Rule 3: if the first parameter is a borrow (the `self`-style receiver),
 *           its lifetime is assigned to every elided output lifetime,
 *           overriding Rule 2.  (Turmeric has no `self` keyword, so "the first
 *           borrow parameter" stands in for the receiver.)
 *
 * Returns the number of lifetime parameters created. */
uint8_t lifetime_elision_apply(LifetimeContext *ctx,
                              Type *param_types, uint8_t n_params,
                              Type *return_type) {
    lifetime_context_init(ctx);

    /* Rule 1: give each borrow parameter that lacks an explicit lifetime a
     * fresh lifetime, and bind it onto the parameter's Type. */
    LifetimeId self_lifetime = LIFETIME_NONE; /* Rule 3: first borrow param's lifetime */
    LifetimeId sole_lifetime = LIFETIME_NONE; /* Rule 2 candidate */
    uint8_t    n_input_lifetimes = 0;

    for (uint8_t i = 0; i < n_params; i++) {
        Type *p = &param_types[i];
        if (!type_head_is_borrow(p)) continue;
        LifetimeId lid;
        if (p->n_lifetimes > 0 && p->lifetimes[0] != LIFETIME_NONE) {
            lid = p->lifetimes[0];          /* explicit -- keep it */
        } else {
            lid = lifetime_context_add(ctx); /* Rule 1: fresh, bound below */
            if (lid == LIFETIME_NONE) break; /* too many lifetimes */
            p->lifetimes[0] = lid;
            p->n_lifetimes = 1;
        }
        n_input_lifetimes++;
        sole_lifetime = lid;
        if (self_lifetime == LIFETIME_NONE) self_lifetime = lid; /* first borrow param */
    }

    if (n_input_lifetimes == 0 || return_type == NULL) {
        return ctx->count;
    }

    /* Only assign an output lifetime when the return type is itself a borrow
     * with no explicit lifetime (an elided output). */
    bool output_elided = type_head_is_borrow(return_type) && return_type->n_lifetimes == 0;
    if (!output_elided) {
        return ctx->count;
    }

    /* Rule 3 takes precedence over Rule 2: a receiver-style first borrow param
     * lends its lifetime to the output.  Otherwise Rule 2 applies only when
     * there is exactly one input lifetime. */
    LifetimeId out_lifetime = LIFETIME_NONE;
    if (self_lifetime != LIFETIME_NONE && type_head_is_borrow(&param_types[0])) {
        out_lifetime = self_lifetime;            /* Rule 3 */
    } else if (n_input_lifetimes == 1) {
        out_lifetime = sole_lifetime;            /* Rule 2 */
    }

    if (out_lifetime != LIFETIME_NONE) {
        return_type->lifetimes[0] = out_lifetime;
        return_type->n_lifetimes = 1;
        /* The elided output lifetime IS the input lifetime (same LifetimeId), so
         * there is no separate outlives edge to record -- they are identical.  An
         * earlier version added a self-constraint (out: out) here, which
         * lifetime_has_cycle correctly reports as a (trivial) cycle and which
         * spuriously failed every borrow-returning function.  Don't add it. */
    }

    return ctx->count;
}
