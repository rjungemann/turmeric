#include "cps.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "diag.h"
#include "elab.h"  /* For scope_lookup */
#include "expr.h"

/* Phase 18: CPS transformation for delimited continuations
 * 
 * For v1, we use a simplified approach:
 * - Functions containing shift/reset are marked as "may_capture" 
 * - No full CPS transformation is performed
 * - The emitter handles shift/reset using runtime continuation support
 * 
 * Full CPS transformation (where every function call becomes a tail call
 * into a continuation) will be implemented in a future phase.
 */

/* Check if an expression contains shift or shift0 */
bool cps_expr_contains_shift(const Expr *e) {
    if (!e) return false;
    
    switch (e->kind) {
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (cps_expr_contains_shift(e->as.let_.bindings[i].init)) return true;
            }
            return cps_expr_contains_shift(e->as.let_.body);
        case EX_IF:
            if (cps_expr_contains_shift(e->as.if_.cond)) return true;
            if (cps_expr_contains_shift(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null && cps_expr_contains_shift(e->as.if_.else_or_null)) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (cps_expr_contains_shift(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_WHILE:
            return cps_expr_contains_shift(e->as.while_.cond) || 
                   cps_expr_contains_shift(e->as.while_.body);
        case EX_SET:
            return cps_expr_contains_shift(e->as.set_.value);
        case EX_DEF:
            return e->as.def_.init && cps_expr_contains_shift(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (cps_expr_contains_shift(e->as.builtin.args[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return cps_fn_needs_transform(e->as.fn_def_.fn);
        case EX_FN:
            return cps_fn_needs_transform(e->as.fn_.fn);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (cps_expr_contains_shift(e->as.call_.args[i])) return true;
            }
            return false;
        case EX_CLOSURE:
            return cps_fn_needs_transform(e->as.closure_.closure->fn);
        case EX_DEFER:
            return cps_expr_contains_shift(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value && cps_expr_contains_shift(e->as.return_.value);
        case EX_THROW:
            return cps_expr_contains_shift(e->as.throw_.payload);
        case EX_PANIC:
            return cps_expr_contains_shift(e->as.panic_.payload);
        case EX_TRY:
            if (cps_expr_contains_shift(e->as.try_.body)) return true;
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                if (cps_expr_contains_shift(e->as.try_.clauses[i].handler)) return true;
            }
            return e->as.try_.finally_body && cps_expr_contains_shift(e->as.try_.finally_body);
        /* Phase 19: Algebraic effects - these lower to shift/reset */
        case EX_DEFECT:
            return false; /* Effect definitions don't contain shift */
        case EX_PERFORM:
            /* perform lowers to shift - always needs CPS marking */
            return true;
        case EX_HANDLE:
            /* handle lowers to reset - always needs CPS marking */
            if (cps_expr_contains_shift(e->as.handle_.handle->body)) return true;
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                if (cps_expr_contains_shift(e->as.handle_.handle->cases[i].body)) return true;
            }
            return true; /* handle itself needs marking even if body doesn't */
        case EX_RESUME:
            /* resume is a tail call into a continuation - needs marking */
            return true;
        case EX_DISCONTINUE:
            /* discontinue is like throw but with a continuation - needs marking */
            return cps_expr_contains_shift(e->as.discontinue_.discontinue->exception);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (cps_expr_contains_shift(e->as.program.items[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

/* Check if a function definition needs CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_shift(fd->body);
}

/* Helper: mark a function as may_capture (for future CPS) */
static void mark_fn_may_capture(FnDef *fd) {
    if (fd) {
        fd->may_capture = true;
    }
}

/* Helper: transform an expression, marking functions that contain shift */
static Expr *cps_mark_expr(Arena *a, Expr *e) {
    if (!e) return NULL;
    
    switch (e->kind) {
        case EX_NIL_LIT:
        case EX_BOOL_LIT:
        case EX_INT_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
        case EX_VAR:
            return e;
        
        case EX_LET: {
            LetBinding *new_bindings = arena_alloc(a, e->as.let_.n * sizeof(LetBinding));
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                new_bindings[i].binding = e->as.let_.bindings[i].binding;
                new_bindings[i].init = cps_mark_expr(a, e->as.let_.bindings[i].init);
            }
            Expr *new_body = cps_mark_expr(a, e->as.let_.body);
            Expr *out = expr_new(a, EX_LET, e->type, e->span);
            out->as.let_.bindings = new_bindings;
            out->as.let_.n = e->as.let_.n;
            out->as.let_.body = new_body;
            return out;
        }
        
        case EX_IF: {
            Expr *new_cond = cps_mark_expr(a, e->as.if_.cond);
            Expr *new_then = cps_mark_expr(a, e->as.if_.then_);
            Expr *new_else = e->as.if_.else_or_null ? 
                cps_mark_expr(a, e->as.if_.else_or_null) : NULL;
            Expr *out = expr_new(a, EX_IF, e->type, e->span);
            out->as.if_.cond = new_cond;
            out->as.if_.then_ = new_then;
            out->as.if_.else_or_null = new_else;
            return out;
        }
        
        case EX_DO: {
            Expr **new_items = arena_alloc(a, e->as.do_.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                new_items[i] = cps_mark_expr(a, e->as.do_.items[i]);
            }
            Expr *out = expr_new(a, EX_DO, e->type, e->span);
            out->as.do_.items = new_items;
            out->as.do_.n = e->as.do_.n;
            return out;
        }
        
        case EX_WHILE: {
            Expr *new_cond = cps_mark_expr(a, e->as.while_.cond);
            Expr *new_body = cps_mark_expr(a, e->as.while_.body);
            Expr *out = expr_new(a, EX_WHILE, e->type, e->span);
            out->as.while_.cond = new_cond;
            out->as.while_.body = new_body;
            return out;
        }
        
        case EX_SET: {
            Expr *new_value = cps_mark_expr(a, e->as.set_.value);
            Expr *out = expr_new(a, EX_SET, e->type, e->span);
            out->as.set_.target = e->as.set_.target;
            out->as.set_.value = new_value;
            return out;
        }
        
        case EX_DEF:
            if (e->as.def_.init) {
                Expr *new_init = cps_mark_expr(a, e->as.def_.init);
                Expr *out = expr_new(a, EX_DEF, e->type, e->span);
                out->as.def_.binding = e->as.def_.binding;
                out->as.def_.init = new_init;
                return out;
            }
            return e;
        
        case EX_BUILTIN: {
            Expr **new_args = arena_alloc(a, e->as.builtin.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                new_args[i] = cps_mark_expr(a, e->as.builtin.args[i]);
            }
            Expr *out = expr_new(a, EX_BUILTIN, e->type, e->span);
            out->as.builtin.spec = e->as.builtin.spec;
            out->as.builtin.args = new_args;
            out->as.builtin.n = e->as.builtin.n;
            return out;
        }
        
        case EX_FN_DEF: {
            FnDef *fd = e->as.fn_def_.fn;
            if (cps_fn_needs_transform(fd)) {
                mark_fn_may_capture(fd);
            }
            FnDef *new_fd = arena_alloc(a, sizeof(FnDef));
            *new_fd = *fd;
            new_fd->body = cps_mark_expr(a, fd->body);
            Expr *out = expr_new(a, EX_FN_DEF, e->type, e->span);
            out->as.fn_def_.fn = new_fd;
            return out;
        }
        
        case EX_FN: {
            FnDef *fn = e->as.fn_.fn;
            if (cps_fn_needs_transform(fn)) {
                mark_fn_may_capture(fn);
            }
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *fn;
            new_fn->body = cps_mark_expr(a, fn->body);
            Expr *out = expr_new(a, EX_FN, e->type, e->span);
            out->as.fn_.fn = new_fn;
            return out;
        }
        
        case EX_CALL: {
            Expr **new_args = arena_alloc(a, e->as.call_.n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                new_args[i] = cps_mark_expr(a, e->as.call_.args[i]);
            }
            Expr *out = expr_new(a, EX_CALL, e->type, e->span);
            out->as.call_.fn_binding = e->as.call_.fn_binding;
            out->as.call_.fn_expr = e->as.call_.fn_expr
                ? cps_mark_expr(a, e->as.call_.fn_expr) : NULL;
            out->as.call_.args = new_args;
            out->as.call_.n_args = e->as.call_.n_args;
            return out;
        }
        
        case EX_CLOSURE: {
            struct Closure *closure = e->as.closure_.closure;
            if (cps_fn_needs_transform(closure->fn)) {
                mark_fn_may_capture(closure->fn);
            }
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *closure->fn;
            new_fn->body = cps_mark_expr(a, closure->fn->body);
            
            struct Closure *new_closure = arena_alloc(a, sizeof(struct Closure));
            *new_closure = *closure;
            new_closure->fn = new_fn;
            
            Expr *out = expr_new(a, EX_CLOSURE, e->type, e->span);
            out->as.closure_.closure = new_closure;
            return out;
        }
        
        case EX_RETURN: {
            Expr *new_value = e->as.return_.value ? 
                cps_mark_expr(a, e->as.return_.value) : NULL;
            Expr *out = expr_new(a, EX_RETURN, e->type, e->span);
            out->as.return_.value = new_value;
            return out;
        }
        
        case EX_THROW: {
            Expr *new_payload = cps_mark_expr(a, e->as.throw_.payload);
            Expr *out = expr_new(a, EX_THROW, e->type, e->span);
            out->as.throw_.payload = new_payload;
            return out;
        }

        case EX_PANIC: {
            Expr *new_payload = cps_mark_expr(a, e->as.panic_.payload);
            Expr *out = expr_new(a, EX_PANIC, e->type, e->span);
            out->as.panic_.payload = new_payload;
            return out;
        }
        
        case EX_TRY: {
            Expr *new_body = cps_mark_expr(a, e->as.try_.body);
            TryCatchClause *new_clauses = arena_alloc(a, e->as.try_.n_clauses * sizeof(TryCatchClause));
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                new_clauses[i] = e->as.try_.clauses[i];
                new_clauses[i].handler = cps_mark_expr(a, e->as.try_.clauses[i].handler);
            }
            Expr *new_finally = e->as.try_.finally_body ? 
                cps_mark_expr(a, e->as.try_.finally_body) : NULL;
            
            Expr *out = expr_new(a, EX_TRY, e->type, e->span);
            out->as.try_.body = new_body;
            out->as.try_.clauses = new_clauses;
            out->as.try_.n_clauses = e->as.try_.n_clauses;
            out->as.try_.finally_body = new_finally;
            return out;
        }
        
        case EX_DEFER: {
            Expr *new_body = cps_mark_expr(a, e->as.defer_.body);
            Binding **new_captures = NULL;
            if (e->as.defer_.n_captures > 0) {
                new_captures = arena_alloc(a, e->as.defer_.n_captures * sizeof(Binding *));
                for (uint8_t i = 0; i < e->as.defer_.n_captures; i++) {
                    new_captures[i] = e->as.defer_.captures[i];
                }
            }
            
            Expr *out = expr_new(a, EX_DEFER, e->type, e->span);
            out->as.defer_.body = new_body;
            out->as.defer_.captures = new_captures;
            out->as.defer_.n_captures = e->as.defer_.n_captures;
            return out;
        }
        
        /* Orthogonal features - pass through */
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
        case EX_REF_PRED:
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT:
        case EX_SET_DEREF:
        case EX_TYPECLASS_DEF:
        case EX_INSTANCE_DEF:
        case EX_EXTERN_C:
        case EX_INLINE_C:
        case EX_MAKE_STRUCT:
            return e;
        
        /* Phase 18: Delimited continuations - pass through but mark */
        case EX_RESET: {
            Expr *new_body = cps_mark_expr(a, e->as.reset_.body);
            Expr *out = expr_new(a, EX_RESET, e->type, e->span);
            out->as.reset_.body = new_body;
            return out;
        }
        
        case EX_SHIFT: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.shift_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.shift_.body);
            Expr *out = expr_new(a, EX_SHIFT, e->type, e->span);
            out->as.shift_.k_fn = new_k_fn;
            out->as.shift_.body = new_body;
            return out;
        }
        
        case EX_SHIFT0: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.shift0_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.shift0_.body);
            Expr *out = expr_new(a, EX_SHIFT0, e->type, e->span);
            out->as.shift0_.k_fn = new_k_fn;
            out->as.shift0_.body = new_body;
            return out;
        }
        
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            return e;
        
        case EX_PERFORM:
            /* perform is already lowered to shift by effect_lower pass */
            /* Just mark the expression */
            return e;
        
        case EX_HANDLE:
            /* handle is already lowered to reset by effect_lower pass */
            /* Just mark the expression */
            return e;
        
        case EX_RESUME: {
            Expr *new_k = cps_mark_expr(a, e->as.resume_.resume->k);
            Expr *new_value = cps_mark_expr(a, e->as.resume_.resume->value);
            ResumeExpr *new_resume = arena_alloc(a, sizeof(ResumeExpr));
            new_resume->k = new_k;
            new_resume->value = new_value;
            Expr *out = expr_new(a, EX_RESUME, e->type, e->span);
            out->as.resume_.resume = new_resume;
            return out;
        }
        
        case EX_DISCONTINUE: {
            Expr *new_k = cps_mark_expr(a, e->as.discontinue_.discontinue->k);
            Expr *new_exception = cps_mark_expr(a, e->as.discontinue_.discontinue->exception);
            DiscontinueExpr *new_dc = arena_alloc(a, sizeof(DiscontinueExpr));
            new_dc->k = new_k;
            new_dc->exception = new_exception;
            Expr *out = expr_new(a, EX_DISCONTINUE, e->type, e->span);
            out->as.discontinue_.discontinue = new_dc;
            return out;
        }
        
        case EX_PROGRAM: {
            Expr **new_items = arena_alloc(a, e->as.program.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                new_items[i] = cps_mark_expr(a, e->as.program.items[i]);
            }
            Expr *out = expr_new(a, EX_PROGRAM, e->type, e->span);
            out->as.program.items = new_items;
            out->as.program.n = e->as.program.n;
            return out;
        }
        
        default:
            return e;
    }
}

/* Main entry point: mark functions that contain shift for CPS transformation */
Expr *cps_transform(Arena *a, Expr *program) {
    if (!program) {
        fprintf(stderr, "cps: NULL program\n");
        return NULL;
    }
    
    if (program->kind != EX_PROGRAM) {
        fprintf(stderr, "cps: expected EX_PROGRAM, got %d\n", program->kind);
        return NULL;
    }
    
    /* Check if the program contains any shift expressions */
    if (!cps_expr_contains_shift(program)) {
        return program; /* No CPS transformation needed */
    }
    
    /* Mark all functions that contain shift */
    return cps_mark_expr(a, program);
}
