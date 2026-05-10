#include "borrow_check.h"

#include <stdio.h>

#include "diag.h"
#include "types.h"
#include "lifetime_elision.h"

void borrow_check_ctx_init(BorrowCheckCtx *ctx, const LifetimeContext *lifetime_ctx, const struct Scope *scope) {
    ctx->lifetime_ctx = lifetime_ctx;
    ctx->scope = scope;
    ctx->error_count = 0;
}

/* Check if a type is a borrow type (&T or &mut T) */
static bool type_is_borrow(Type t) {
    return t.kind == TY_REF_IMMUT || t.kind == TY_REF_MUT;
}

/* Check if a type is a reference type (ref<T>, rc<T>, weak<T>) */
static bool type_is_reference(Type t) __attribute__((unused));
static bool type_is_reference(Type t) {
    return t.kind == TY_REF || t.kind == TY_RC || t.kind == TY_WEAK;
}

/* Check if an expression is a borrow expression */
static bool expr_is_borrow(const Expr *e) {
    return e->kind == EX_BORROW_IMMUT || e->kind == EX_BORROW_MUT;
}

/* Recursively check an expression for borrow violations */
static bool borrow_check_expr_recursive(BorrowCheckCtx *ctx, const Expr *e);

/* Check a function call for borrow validity */
static bool borrow_check_call(BorrowCheckCtx *ctx, const Expr *e) {
    /* For now, we just validate the types - full inter-procedural checking deferred */
    
    /* Check each argument */
    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
        const Expr *arg = e->as.call_.args[i];
        if (!borrow_check_expr_recursive(ctx, arg)) {
            return false;
        }
        
        /* Check if argument is a borrow and the function accepts it */
        if (expr_is_borrow(arg) || type_is_borrow(arg->type)) {
            /* The function should have a parameter type that accepts borrows */
            /* This validation is deferred until full type system integration */
        }
    }
    
    return true;
}

/* Global flag to enable/disable borrow checking (default: disabled for Phase 14) */
static bool borrow_check_enabled = false;

void borrow_check_set_enabled(bool enabled) {
    borrow_check_enabled = enabled;
}

bool borrow_check_is_enabled(void) {
    return borrow_check_enabled;
}

/* Run borrow checker on an entire program */
bool borrow_check_program(const Expr *program) {
    if (!borrow_check_is_enabled()) {
        return true;  /* Borrow checking disabled - skip */
    }
    
    if (program->kind != EX_PROGRAM) {
        return true;  /* Not a program - skip */
    }
    
    /* Create a borrow check context */
    BorrowCheckCtx ctx;
    borrow_check_ctx_init(&ctx, NULL, NULL);
    
    /* Check each top-level expression */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *item = program->as.program.items[i];
        if (!borrow_check_expr(&ctx, item)) {
            return false;
        }
    }
    
    return ctx.error_count == 0;
}

/* Check a variable reference */
static bool borrow_check_var(BorrowCheckCtx *ctx, const Expr *e) {
    const Binding *b = e->as.var.binding;
    
    /* Check if this is a moved binding */
    if (b->is_moved) {
        diag_emit(DIAG_ERROR, e->span,
                  "use-after-move of '%s' - value was moved", b->name->name);
        ctx->error_count++;
        return false;
    }
    
    /* Check if this is a borrow type */
    if (type_is_borrow(e->type)) {
        /* The borrow should still be valid in the current scope */
        /* This is tracked by the existing Phase 12 scope-based borrow tracking */
    }
    
    return true;
}

/* Recursively check an expression */
static bool borrow_check_expr_recursive(BorrowCheckCtx *ctx, const Expr *e) {
    switch (e->kind) {
        case EX_NIL_LIT:
        case EX_BOOL_LIT:
        case EX_INT_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
            return true;
            
        case EX_VAR:
            return borrow_check_var(ctx, e);
            
        case EX_LET:
            /* Check init expressions */
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.let_.bindings[i].init)) {
                    return false;
                }
            }
            /* Check body */
            return borrow_check_expr_recursive(ctx, e->as.let_.body);
            
        case EX_IF:
            if (!borrow_check_expr_recursive(ctx, e->as.if_.cond)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.if_.then_)) return false;
            if (e->as.if_.else_or_null && !borrow_check_expr_recursive(ctx, e->as.if_.else_or_null)) {
                return false;
            }
            return true;
            
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.do_.items[i])) {
                    return false;
                }
            }
            return true;
            
        case EX_WHILE:
            if (!borrow_check_expr_recursive(ctx, e->as.while_.cond)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.while_.body)) return false;
            return true;
            
        case EX_SET:
            if (!borrow_check_expr_recursive(ctx, e->as.set_.value)) return false;
            /* Check if target is moved */
            if (e->as.set_.target->is_moved) {
                diag_emit(DIAG_ERROR, e->span,
                          "use-after-move of '%s' in set!", e->as.set_.target->name->name);
                ctx->error_count++;
                return false;
            }
            return true;
            
        case EX_DEF:
            if (e->as.def_.init && !borrow_check_expr_recursive(ctx, e->as.def_.init)) {
                return false;
            }
            return true;
            
        case EX_CALL:
            return borrow_check_call(ctx, e);
            
        case EX_FN_DEF:
            /* Check the function body with a new context */
            {
                BorrowCheckCtx fn_ctx;
                borrow_check_ctx_init(&fn_ctx, &e->as.fn_def_.fn->lifetime_ctx, ctx->scope);
                if (!borrow_check_fn(&fn_ctx, e->as.fn_def_.fn)) {
                    ctx->error_count += fn_ctx.error_count;
                    return false;
                }
            }
            return true;
        case EX_FN:
            /* Function expression - check the fn_def */
            {
                FnDef *fn_def = e->as.fn_.fn;
                BorrowCheckCtx fn_ctx;
                borrow_check_ctx_init(&fn_ctx, &fn_def->lifetime_ctx, ctx->scope);
                if (!borrow_check_fn(&fn_ctx, fn_def)) {
                    ctx->error_count += fn_ctx.error_count;
                    return false;
                }
            }
            return true;
            
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT:
            /* Borrow expressions are always valid at their creation point */
            if (!borrow_check_expr_recursive(ctx, e->as.borrow_immut_.expr)) {
                return false;
            }
            return true;
            
        case EX_CLOSURE:
            /* Check closure body */
            if (e->as.closure_.closure && e->as.closure_.closure->fn) {
                BorrowCheckCtx closure_ctx;
                borrow_check_ctx_init(&closure_ctx, &e->as.closure_.closure->fn->lifetime_ctx, ctx->scope);
                if (!borrow_check_fn(&closure_ctx, e->as.closure_.closure->fn)) {
                    ctx->error_count += closure_ctx.error_count;
                    return false;
                }
            }
            return true;
            
        case EX_DEFER:
            /* Check defer body */
            return borrow_check_expr_recursive(ctx, e->as.defer_.body);
        case EX_RETURN:
            /* Check return value */
            if (e->as.return_.value) {
                return borrow_check_expr_recursive(ctx, e->as.return_.value);
            }
            return true;
            
        case EX_REF:
        case EX_DEREF:
        case EX_RC_OF:
        case EX_RC_CLONE:
        case EX_RC_DROP:
        case EX_RC_PTR:
        case EX_RC_COUNT:
        case EX_WEAK:
        case EX_WEAK_UPGRADE:
        case EX_WEAK_PRED:
            /* These are reference operations - check the inner expression */
            if (e->kind == EX_REF) {
                return borrow_check_expr_recursive(ctx, e->as.ref_.expr);
            } else if (e->kind == EX_DEREF) {
                return borrow_check_expr_recursive(ctx, e->as.deref_.expr);
            } else if (e->kind == EX_RC_OF) {
                return borrow_check_expr_recursive(ctx, e->as.rc_of_.expr);
            } else if (e->kind == EX_RC_CLONE) {
                return borrow_check_expr_recursive(ctx, e->as.rc_clone_.expr);
            } else if (e->kind == EX_RC_DROP) {
                return borrow_check_expr_recursive(ctx, e->as.rc_drop_.expr);
            } else if (e->kind == EX_RC_PTR) {
                return borrow_check_expr_recursive(ctx, e->as.rc_ptr_.expr);
            } else if (e->kind == EX_RC_COUNT) {
                return borrow_check_expr_recursive(ctx, e->as.rc_count_.expr);
            } else if (e->kind == EX_WEAK) {
                return borrow_check_expr_recursive(ctx, e->as.weak_.expr);
            } else if (e->kind == EX_WEAK_UPGRADE) {
                return borrow_check_expr_recursive(ctx, e->as.weak_upgrade_.expr);
            } else if (e->kind == EX_WEAK_PRED) {
                return borrow_check_expr_recursive(ctx, e->as.weak_pred_.expr);
            }
            return true;
            
        case EX_BUILTIN:
            /* Check builtin arguments */
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.builtin.args[i])) {
                    return false;
                }
            }
            return true;
            
        case EX_TYPECLASS_DEF:
        case EX_INSTANCE_DEF:
            /* Typeclass definitions are compile-time only - no runtime behavior to check */
            return true;
            
        case EX_EXTERN_C:
        case EX_INLINE_C:
            /* External and inline C are trusted */
            return true;
            
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.program.items[i])) {
                    return false;
                }
            }
            return true;
        /* Phase 17: Exceptions */
        case EX_THROW:
            /* Check the payload expression */
            return borrow_check_expr_recursive(ctx, e->as.throw_.payload);
        case EX_TRY:
            /* Check try body and all handlers */
            if (!borrow_check_expr_recursive(ctx, e->as.try_.body)) {
                return false;
            }
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.try_.clauses[i].handler)) {
                    return false;
                }
            }
            if (e->as.try_.finally_body) {
                if (!borrow_check_expr_recursive(ctx, e->as.try_.finally_body)) {
                    return false;
                }
            }
            return true;
        /* Phase 18: Delimited continuations */
        case EX_RESET:
            /* Check the reset body */
            return borrow_check_expr_recursive(ctx, e->as.reset_.body);
        case EX_SHIFT:
            /* Check k function and body */
            if (!borrow_check_expr_recursive(ctx, e->as.shift_.k_fn)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.shift_.body)) return false;
            return true;
        case EX_SHIFT0:
            /* Check k function and body */
            if (!borrow_check_expr_recursive(ctx, e->as.shift0_.k_fn)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.shift0_.body)) return false;
            return true;
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions don't have borrows to check at this level */
            return true;
        case EX_PERFORM:
            /* Check perform arguments */
            for (uint8_t i = 0; i < e->as.perform_.perform->n_args; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.perform_.perform->args[i])) {
                    return false;
                }
            }
            return true;
        case EX_HANDLE:
            /* Check the handled body and all case handlers */
            if (!borrow_check_expr_recursive(ctx, e->as.handle_.handle->body)) {
                return false;
            }
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                if (!borrow_check_expr_recursive(ctx, e->as.handle_.handle->cases[i].body)) {
                    return false;
                }
            }
            return true;
        case EX_RESUME:
            /* Check continuation and value */
            if (!borrow_check_expr_recursive(ctx, e->as.resume_.resume->k)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.resume_.resume->value)) return false;
            return true;
        case EX_DISCONTINUE:
            /* Check continuation and exception */
            if (!borrow_check_expr_recursive(ctx, e->as.discontinue_.discontinue->k)) return false;
            if (!borrow_check_expr_recursive(ctx, e->as.discontinue_.discontinue->exception)) return false;
            return true;
    }
    
    return true;
}

bool borrow_check_expr(BorrowCheckCtx *ctx, const Expr *e) {
    return borrow_check_expr_recursive(ctx, e);
}

bool borrow_check_fn(BorrowCheckCtx *ctx, const FnDef *fn) {
    /* Check function signature first */
    if (!borrow_check_fn_signature(fn)) {
        ctx->error_count++;
        return false;
    }
    
    /* Check return type */
    if (!borrow_check_return_type(fn)) {
        ctx->error_count++;
        return false;
    }
    
    /* Check function body */
    BorrowCheckCtx body_ctx;
    borrow_check_ctx_init(&body_ctx, &fn->lifetime_ctx, ctx->scope);
    
    if (fn->body && !borrow_check_expr(&body_ctx, fn->body)) {
        ctx->error_count += body_ctx.error_count;
        return false;
    }
    
    return true;
}

bool borrow_check_fn_signature(const FnDef *fn) {
    /* Validate that all borrow parameters have valid types */
    Span fn_span = fn->binding ? fn->binding->span : SPAN_UNKNOWN;
    for (uint8_t i = 0; i < fn->n_params; i++) {
        Type param_type = fn->param_types[i];
        
        /* Check if parameter type is valid */
        if (param_type.kind == TY_UNKNOWN) {
            diag_emit(DIAG_ERROR, fn_span,
                      "function '%s': parameter %u has unknown type", 
                      fn->binding->name->name, i + 1);
            return false;
        }
        
        /* Check for dangling references in parameters */
        if (type_is_borrow(param_type)) {
            /* Borrow parameters are valid as long as they have a lifetime */
            if (param_type.n_lifetimes == 0) {
                /* No explicit lifetime - elision rules should have applied */
                /* This is OK - elision rules assign implicit lifetimes */
            }
        }
    }
    
    return true;
}

bool borrow_check_return_type(const FnDef *fn) {
    Span fn_span = fn->binding ? fn->binding->span : SPAN_UNKNOWN;
    Type return_type = fn->binding->type;
    
    /* If return type is not a function type, get the actual return type */
    if (return_type.kind == TY_FN) {
        return_type.kind = return_type.as.fn.result_kind;
    }
    
    /* Check for dangling references in return type */
    if (type_is_borrow(return_type) && return_type.n_lifetimes == 0) {
        /* Return type is a borrow but has no lifetime annotation */
        /* This means it borrows from one of the input parameters (via elision) */
        /* For now, we allow this - the elision rules should have assigned a lifetime */
    }
    
    /* Check that all lifetimes in the return type are valid */
    if (type_has_any_lifetime(&return_type)) {
        /* Validate that each lifetime in the return type corresponds to a valid input lifetime */
        for (uint8_t i = 0; i < return_type.n_lifetimes; i++) {
            LifetimeId lid = return_type.lifetimes[i];
            if (lid == LIFETIME_NONE) continue;
            
            /* Check if this lifetime appears in any input parameter */
            bool found = false;
            for (uint8_t j = 0; j < fn->n_params; j++) {
                Type param_type = fn->param_types[j];
                LifetimeId param_lifetimes[MAX_TYPE_LIFETIMES];
                uint8_t n_param = 0;
                type_collect_lifetimes(&param_type, param_lifetimes, &n_param, MAX_TYPE_LIFETIMES);
                
                for (uint8_t k = 0; k < n_param; k++) {
                    if (param_lifetimes[k] == lid) {
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            
            if (!found) {
                diag_emit(DIAG_ERROR, fn_span,
                          "function '%s': return type lifetime '%c' does not appear in any input parameter",
                          fn->binding->name->name, 'a' + (lid - 1));
                return false;
            }
        }
    }
    
    return true;
}
