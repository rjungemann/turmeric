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
 * This pass transforms functions containing shift/shift0 into CPS form.
 * The transformation converts direct-style functions into continuation-passing style,
 * where every function call becomes a tail call into a continuation.
 * 
 * Key concepts:
 * - A function "needs CPS" if it transitively contains shift or shift0.
 * - In CPS form, functions take an additional continuation parameter.
 * - shift captures the current continuation and passes it to its function argument.
 * - reset establishes a new continuation boundary.
 * - Continuations are one-shot (move-only).
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
            /* Check if the closure contains shift */
            return cps_fn_needs_transform(e->as.closure_.closure->fn);
        case EX_DEFER:
            return cps_expr_contains_shift(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value && cps_expr_contains_shift(e->as.return_.value);
        case EX_THROW:
            return cps_expr_contains_shift(e->as.throw_.payload);
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
            /* perform lowers to shift - always needs CPS */
            return true;
        case EX_HANDLE:
            /* handle lowers to reset - always needs CPS */
            if (cps_expr_contains_shift(e->as.handle_.handle->body)) return true;
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                if (cps_expr_contains_shift(e->as.handle_.handle->cases[i].body)) return true;
            }
            return true; /* handle itself needs CPS even if body doesn't */
        case EX_RESUME:
            /* resume is a tail call into a continuation - needs CPS */
            return true;
        case EX_DISCONTINUE:
            /* discontinue is like throw but with a continuation - needs CPS */
            return cps_expr_contains_shift(e->as.discontinue_.discontinue->exception);
        default:
            return false;
    }
}

/* Check if a function definition needs CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_shift(fd->body);
}

/* Helper: create a fresh continuation parameter name */
static const Symbol *fresh_cont_name(Arena *a, SymbolTable *st, uint32_t *counter) {
    char buf[32];
    snprintf(buf, sizeof(buf), "__k%u", (*counter)++);
    return symtab_intern(st, strslice(buf, (uint32_t)strlen(buf)));
}

/* Helper: create a continuation type for a given return type */
static Type type_cont_of(Type return_type) {
    return type_cont(return_type.kind);
}

/* Transform an expression in CPS form.
 * In CPS, every expression that can return a value becomes a call into a continuation.
 * The continuation parameter 'k' is a function that takes the result value.
 */
static Expr *cps_transform_expr(Arena *a, SymbolTable *st, uint32_t *tmp_counter,
                                Expr *e, const Symbol *k_param) {
    if (!e) return NULL;
    
    switch (e->kind) {
        case EX_NIL_LIT:
        case EX_BOOL_LIT:
        case EX_INT_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
        case EX_VAR:
            /* Literals and variables: apply continuation directly */
            return e; /* In CPS, these are values - no transformation needed at this level */
        
        case EX_LET: {
            /* (let [x init] body) -> (let [x init] body) but body might need CPS */
            LetBinding *bindings = arena_alloc(a, e->as.let_.n * sizeof(LetBinding));
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                bindings[i].binding = e->as.let_.bindings[i].binding;
                /* Transform the init expression */
                bindings[i].init = cps_transform_expr(a, st, tmp_counter, 
                                                     e->as.let_.bindings[i].init, k_param);
            }
            Expr *new_body = cps_transform_expr(a, st, tmp_counter, e->as.let_.body, k_param);
            Expr *out = expr_new(a, EX_LET, e->type, e->span);
            out->as.let_.bindings = bindings;
            out->as.let_.n = e->as.let_.n;
            out->as.let_.body = new_body;
            return out;
        }
        
        case EX_IF: {
            /* (if cond then else) -> (if cond (k then) (k else)) */
            /* But in CPS, we need to transform both branches to call k */
            /* This is complex - for v1, we'll just pass through */
            Expr *new_cond = cps_transform_expr(a, st, tmp_counter, e->as.if_.cond, k_param);
            Expr *new_then = cps_transform_expr(a, st, tmp_counter, e->as.if_.then_, k_param);
            Expr *new_else = e->as.if_.else_or_null ? 
                cps_transform_expr(a, st, tmp_counter, e->as.if_.else_or_null, k_param) : NULL;
            Expr *out = expr_new(a, EX_IF, e->type, e->span);
            out->as.if_.cond = new_cond;
            out->as.if_.then_ = new_then;
            out->as.if_.else_or_null = new_else;
            return out;
        }
        
        case EX_DO: {
            /* Transform each item in the do block */
            Expr **new_items = arena_alloc(a, e->as.do_.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                new_items[i] = cps_transform_expr(a, st, tmp_counter, e->as.do_.items[i], k_param);
            }
            Expr *out = expr_new(a, EX_DO, e->type, e->span);
            out->as.do_.items = new_items;
            out->as.do_.n = e->as.do_.n;
            return out;
        }
        
        case EX_WHILE: {
            Expr *new_cond = cps_transform_expr(a, st, tmp_counter, e->as.while_.cond, k_param);
            Expr *new_body = cps_transform_expr(a, st, tmp_counter, e->as.while_.body, k_param);
            Expr *out = expr_new(a, EX_WHILE, e->type, e->span);
            out->as.while_.cond = new_cond;
            out->as.while_.body = new_body;
            return out;
        }
        
        case EX_SET: {
            Expr *new_value = cps_transform_expr(a, st, tmp_counter, e->as.set_.value, k_param);
            Expr *out = expr_new(a, EX_SET, e->type, e->span);
            out->as.set_.target = e->as.set_.target;
            out->as.set_.value = new_value;
            return out;
        }
        
        case EX_DEF:
            /* def is a top-level construct - shouldn't be in CPS context */
            return e;
        
        case EX_BUILTIN: {
            /* Transform arguments */
            Expr **new_args = arena_alloc(a, e->as.builtin.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                new_args[i] = cps_transform_expr(a, st, tmp_counter, e->as.builtin.args[i], k_param);
            }
            Expr *out = expr_new(a, EX_BUILTIN, e->type, e->span);
            out->as.builtin.spec = e->as.builtin.spec;
            out->as.builtin.args = new_args;
            out->as.builtin.n = e->as.builtin.n;
            return out;
        }
        
        case EX_CALL: {
            /* (f a b) -> (f a b k) in CPS form */
            /* For now, just transform the arguments - the continuation passing
             * will be handled at code generation time */
            Expr **new_args = arena_alloc(a, e->as.call_.n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                new_args[i] = cps_transform_expr(a, st, tmp_counter, e->as.call_.args[i], k_param);
            }
            Expr *out = expr_new(a, EX_CALL, e->type, e->span);
            out->as.call_.fn_binding = e->as.call_.fn_binding;
            out->as.call_.args = new_args;
            out->as.call_.n_args = e->as.call_.n_args;
            return out;
        }
        
        case EX_FN: {
            /* Anonymous function - transform its body */
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *e->as.fn_.fn;
            new_fn->body = cps_transform_expr(a, st, tmp_counter, new_fn->body, k_param);
            /* Add continuation parameter */
            new_fn->n_params++;
            Binding **new_params = arena_alloc(a, new_fn->n_params * sizeof(Binding *));
            memcpy(new_params, new_fn->params, (new_fn->n_params - 1) * sizeof(Binding *));
            /* Create continuation parameter binding */
            const Symbol *k_name = fresh_cont_name(a, st, tmp_counter);
            Binding *k_binding = arena_alloc(a, sizeof(Binding));
            k_binding->name = k_name;
            k_binding->type = type_cont_of(new_fn->param_types[new_fn->n_params - 1]);
            k_binding->is_mut = false;
            k_binding->is_global = false;
            k_binding->id = 0;
            k_binding->is_moved = false;
            new_params[new_fn->n_params - 1] = k_binding;
            new_fn->params = new_params;
            
            Expr *out = expr_new(a, EX_FN, e->type, e->span);
            out->as.fn_.fn = new_fn;
            return out;
        }
        
        case EX_FN_DEF: {
            /* Top-level function definition */
            FnDef *fd = e->as.fn_def_.fn;
            if (!cps_fn_needs_transform(fd)) {
                return e; /* No transformation needed */
            }
            
            /* Transform the function to CPS form */
            /* Add continuation parameter */
            fd->n_params++;
            Binding **new_params = arena_alloc(a, fd->n_params * sizeof(Binding *));
            memcpy(new_params, fd->params, (fd->n_params - 1) * sizeof(Binding *));
            
            const Symbol *k_name = fresh_cont_name(a, st, tmp_counter);
            Binding *k_binding = arena_alloc(a, sizeof(Binding));
            k_binding->name = k_name;
            k_binding->type = type_cont_of(fd->body->type);
            k_binding->is_mut = false;
            k_binding->is_global = false;
            k_binding->id = 0;
            k_binding->is_moved = false;
            new_params[fd->n_params - 1] = k_binding;
            fd->params = new_params;
            
            /* Transform the body */
            fd->body = cps_transform_expr(a, st, tmp_counter, fd->body, k_name);
            
            return e;
        }
        
        case EX_CLOSURE: {
            /* Transform the closure's function */
            struct Closure *closure = e->as.closure_.closure;
            if (!cps_fn_needs_transform(closure->fn)) {
                return e;
            }
            
            /* Add continuation parameter to the closure function */
            FnDef *fn = closure->fn;
            fn->n_params++;
            Binding **new_params = arena_alloc(a, fn->n_params * sizeof(Binding *));
            memcpy(new_params, fn->params, (fn->n_params - 1) * sizeof(Binding *));
            
            const Symbol *k_name = fresh_cont_name(a, st, tmp_counter);
            Binding *k_binding = arena_alloc(a, sizeof(Binding));
            k_binding->name = k_name;
            k_binding->type = type_cont_of(fn->body->type);
            k_binding->is_mut = false;
            k_binding->is_global = false;
            k_binding->id = 0;
            k_binding->is_moved = false;
            new_params[fn->n_params - 1] = k_binding;
            fn->params = new_params;
            
            /* Transform the body */
            fn->body = cps_transform_expr(a, st, tmp_counter, fn->body, k_name);
            
            return e;
        }
        
        case EX_RESET: {
            /* (reset body) - body runs with identity continuation */
            /* In CPS: (let [k_identity (fn [v] v)] (reset-body k_identity)) */
            /* For now, just transform the body */
            Expr *new_body = cps_transform_expr(a, st, tmp_counter, e->as.reset_.body, k_param);
            Expr *out = expr_new(a, EX_RESET, e->type, e->span);
            out->as.reset_.body = new_body;
            return out;
        }
        
        case EX_SHIFT: {
            /* (shift k body) - k receives the current continuation */
            /* In CPS: shift passes the current continuation to k */
            /* For now, just transform k and body */
            Expr *new_k = cps_transform_expr(a, st, tmp_counter, e->as.shift_.k_fn, k_param);
            Expr *new_body = cps_transform_expr(a, st, tmp_counter, e->as.shift_.body, k_param);
            Expr *out = expr_new(a, EX_SHIFT, e->type, e->span);
            out->as.shift_.k_fn = new_k;
            out->as.shift_.body = new_body;
            return out;
        }
        
        case EX_SHIFT0: {
            Expr *new_k = cps_transform_expr(a, st, tmp_counter, e->as.shift0_.k_fn, k_param);
            Expr *new_body = cps_transform_expr(a, st, tmp_counter, e->as.shift0_.body, k_param);
            Expr *out = expr_new(a, EX_SHIFT0, e->type, e->span);
            out->as.shift0_.k_fn = new_k;
            out->as.shift0_.body = new_body;
            return out;
        }
        
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions are compile-time only */
            return e;
        
        case EX_PERFORM: {
            /* (perform (E args...)) -> (shift k (dispatch E args k)) */
            /* For now, just transform as shift */
            Expr *out = expr_new(a, EX_SHIFT, e->type, e->span);
            /* k function: (fn [k] (perform_dispatch E args k)) */
            /* This is complex - for now, return a placeholder */
            out->as.shift_.k_fn = e->as.perform_.perform ? 
                cps_transform_expr(a, st, tmp_counter, 
                    expr_new(a, EX_VAR, e->type, e->span), k_param) : NULL;
            out->as.shift_.body = e->as.perform_.perform ? 
                cps_transform_expr(a, st, tmp_counter, 
                    expr_new(a, EX_VAR, e->type, e->span), k_param) : NULL;
            return out;
        }
        
        case EX_HANDLE: {
            /* (handle body cases...) -> (reset (let [handlers...] body)) */
            /* For now, just wrap body in reset */
            HandleExpr *handle = e->as.handle_.handle;
            Expr *new_body = cps_transform_expr(a, st, tmp_counter, handle->body, k_param);
            
            /* Create reset expression */
            Expr *reset = expr_new(a, EX_RESET, new_body->type, e->span);
            reset->as.reset_.body = new_body;
            return reset;
        }
        
        case EX_RESUME: {
            /* (resume k v) -> tail call into k with v */
            Expr *new_k = cps_transform_expr(a, st, tmp_counter, e->as.resume_.resume->k, k_param);
            Expr *new_v = cps_transform_expr(a, st, tmp_counter, e->as.resume_.resume->value, k_param);
            Expr *out = expr_new(a, EX_RESUME, e->type, e->span);
            ResumeExpr *resume = arena_alloc(a, sizeof(ResumeExpr));
            resume->k = new_k;
            resume->value = new_v;
            out->as.resume_.resume = resume;
            return out;
        }
        
        case EX_DISCONTINUE: {
            Expr *new_k = cps_transform_expr(a, st, tmp_counter, e->as.discontinue_.discontinue->k, k_param);
            Expr *new_e = cps_transform_expr(a, st, tmp_counter, e->as.discontinue_.discontinue->exception, k_param);
            Expr *out = expr_new(a, EX_DISCONTINUE, e->type, e->span);
            DiscontinueExpr *dc = arena_alloc(a, sizeof(DiscontinueExpr));
            dc->k = new_k;
            dc->exception = new_e;
            out->as.discontinue_.discontinue = dc;
            return out;
        }
        
        case EX_DEFER:
        case EX_RETURN:
        case EX_THROW:
        case EX_TRY:
            /* For now, just pass through */
            return e;
        
        case EX_PROGRAM: {
            /* Transform all items in the program */
            Expr **new_items = arena_alloc(a, e->as.program.n * sizeof(Expr *));
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                new_items[i] = cps_transform_expr(a, st, tmp_counter, e->as.program.items[i], k_param);
            }
            Expr *out = expr_new(a, EX_PROGRAM, e->type, e->span);
            out->as.program.items = new_items;
            out->as.program.n = e->as.program.n;
            return out;
        }
        
        default:
            /* For any other expression, just return as-is */
            return e;
    }
}

/* Main entry point: transform a program to CPS form */
Expr *cps_transform(Arena *a, Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "cps: expected EX_PROGRAM\n");
        return NULL;
    }
    
    /* Check if the program contains any shift expressions */
    if (!cps_expr_contains_shift(program)) {
        return program; /* No CPS transformation needed */
    }
    
    /* Create a symbol table for the CPS pass */
    /* We need this for creating fresh names */
    /* For now, we'll use a stub since we don't have access to the main symbol table */
    SymbolTable st;
    symtab_init(&st, a);
    
    uint32_t tmp_counter = 0;
    const Symbol *dummy_k = NULL; /* For top-level, we don't have a continuation */
    
    /* Transform the program */
    Expr *result = cps_transform_expr(a, &st, &tmp_counter, program, dummy_k);
    
    symtab_free(&st);
    return result;
}
