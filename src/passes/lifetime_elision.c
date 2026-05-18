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

/* Apply lifetime elision rules to a function signature.
 * 
 * Rule 1: Each lifetime in an input type becomes a distinct lifetime parameter.
 * Rule 2: If there's exactly one input lifetime, assign it to all output lifetimes.
 * Rule 3: For method calls with &self or &mut self, use &self's lifetime (deferred).
 * 
 * This function:
 * 1. Scans all input types for lifetime annotations
 * 2. Creates lifetime parameters for each unique input lifetime
 * 3. If there's exactly one input lifetime, assigns it to output lifetimes that don't have one
 * 
 * Returns: number of lifetime parameters created */
uint8_t lifetime_elision_apply(LifetimeContext *ctx,
                              Type *param_types, uint8_t n_params,
                              Type *return_type) {
    lifetime_context_init(ctx);
    
    /* Rule 1: Collect all unique lifetimes from input types */
    LifetimeId input_lifetimes[MAX_LIFETIMES];
    uint8_t n_input = 0;
    
    for (uint8_t i = 0; i < n_params; i++) {
        type_collect_lifetimes(&param_types[i], input_lifetimes, &n_input, MAX_LIFETIMES);
    }
    
    /* If no input lifetimes, we're done */
    if (n_input == 0) {
        return 0;
    }
    
    /* Create lifetime parameters for each unique input lifetime */
    for (uint8_t i = 0; i < n_input; i++) {
        if (input_lifetimes[i] != LIFETIME_NONE) {
            /* Register this lifetime as a parameter */
            /* For now, we just track it; full implementation deferred */
            lifetime_context_add(ctx);
        }
    }
    
    /* Rule 2: If there's exactly one input lifetime, assign it to output */
    if (n_input == 1 && return_type != NULL) {
        LifetimeId the_lifetime = input_lifetimes[0];
        if (the_lifetime != LIFETIME_NONE) {
            /* Check if return type already has a lifetime */
            if (return_type->n_lifetimes == 0 && type_has_any_lifetime(return_type)) {
                /* Assign the input lifetime to the return type */
                return_type->lifetimes[0] = the_lifetime;
                return_type->n_lifetimes = 1;
            }
        }
    }
    
    return ctx->count;
}
