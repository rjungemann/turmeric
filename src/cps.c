#include "cps.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "diag.h"
#include "elab.h"  /* For scope_lookup */
#include "expr.h"
#include "typeclass.h"

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
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
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
        case EX_PANIC_WITH:
            return cps_expr_contains_shift(e->as.panic_with_.payload);
        case EX_CATCH_UNWIND:
            return cps_expr_contains_shift(e->as.catch_unwind_.thunk);
        case EX_CATCH_PANIC_OF:
            return cps_expr_contains_shift(e->as.catch_panic_of_.thunk);
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
            return cps_expr_contains_shift(e->as.panic_payload_type_.payload);
        case EX_PANIC_PAYLOAD_DOWNS:
            return cps_expr_contains_shift(e->as.panic_payload_downs_.payload);
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

/* Check if a function definition needs (one-shot) CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_shift(fd->body);
}

/* Phase B2: Check if an expression contains cloneable-shift or cloneable-reset. */
bool cps_expr_contains_cloneable_shift(const Expr *e) {
    if (!e) return false;

    switch (e->kind) {
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.let_.bindings[i].init))
                    return true;
            }
            return cps_expr_contains_cloneable_shift(e->as.let_.body);
        case EX_IF:
            if (cps_expr_contains_cloneable_shift(e->as.if_.cond)) return true;
            if (cps_expr_contains_cloneable_shift(e->as.if_.then_)) return true;
            return e->as.if_.else_or_null &&
                   cps_expr_contains_cloneable_shift(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.do_.items[i]))
                    return true;
            }
            return false;
        case EX_WHILE:
            return cps_expr_contains_cloneable_shift(e->as.while_.cond) ||
                   cps_expr_contains_cloneable_shift(e->as.while_.body);
        case EX_FN_DEF:
            return cps_fn_needs_cloneable_transform(e->as.fn_def_.fn);
        case EX_FN:
            return cps_fn_needs_cloneable_transform(e->as.fn_.fn);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.call_.args[i]))
                    return true;
            }
            return false;
        case EX_CLOSURE:
            return cps_fn_needs_cloneable_transform(e->as.closure_.closure->fn);
        case EX_RETURN:
            return e->as.return_.value &&
                   cps_expr_contains_cloneable_shift(e->as.return_.value);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (cps_expr_contains_cloneable_shift(e->as.program.items[i]))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

/* Phase B2: Check if a function definition needs cloneable CPS transformation. */
bool cps_fn_needs_cloneable_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_cloneable_shift(fd->body);
}

/* -------------------------------------------------------------------------
 * CPS-CL1: Liveness analysis for cloneable-shift sites
 * -------------------------------------------------------------------------
 * Conservative approach: collect every local (non-global) Binding referenced
 * via EX_VAR anywhere in the enclosing function body.  This over-captures but
 * is always correct; the env struct may be slightly larger than necessary.
 * -------------------------------------------------------------------------*/

/* Add b to *out (malloc'd, grown as needed) if it is local and not yet present. */
static void add_unique_local(Binding *b, Binding ***out, uint32_t *n, uint32_t *cap) {
    if (!b || b->is_global) return;
    for (uint32_t i = 0; i < *n; i++) {
        if ((*out)[i] == b) return; /* already present */
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *out = realloc(*out, *cap * sizeof(Binding *));
    }
    (*out)[(*n)++] = b;
}

/* Recursively walk e, collecting unique local bindings into *out. */
static void collect_local_vars_expr(const Expr *e,
                                    Binding ***out, uint32_t *n, uint32_t *cap) {
    if (!e) return;
    switch (e->kind) {
        case EX_VAR:
            add_unique_local(e->as.var.binding, out, n, cap);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                collect_local_vars_expr(e->as.let_.bindings[i].init, out, n, cap);
            collect_local_vars_expr(e->as.let_.body, out, n, cap);
            return;
        case EX_IF:
            collect_local_vars_expr(e->as.if_.cond, out, n, cap);
            collect_local_vars_expr(e->as.if_.then_, out, n, cap);
            collect_local_vars_expr(e->as.if_.else_or_null, out, n, cap);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                collect_local_vars_expr(e->as.do_.items[i], out, n, cap);
            return;
        case EX_WHILE:
            collect_local_vars_expr(e->as.while_.cond, out, n, cap);
            collect_local_vars_expr(e->as.while_.body, out, n, cap);
            return;
        case EX_SET:
            collect_local_vars_expr(e->as.set_.value, out, n, cap);
            return;
        case EX_DEF:
            collect_local_vars_expr(e->as.def_.init, out, n, cap);
            return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                collect_local_vars_expr(e->as.builtin.args[i], out, n, cap);
            return;
        case EX_CALL:
            collect_local_vars_expr(e->as.call_.fn_expr, out, n, cap);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                collect_local_vars_expr(e->as.call_.args[i], out, n, cap);
            return;
        case EX_RETURN:
            collect_local_vars_expr(e->as.return_.value, out, n, cap);
            return;
        case EX_THROW:
            collect_local_vars_expr(e->as.throw_.payload, out, n, cap);
            return;
        case EX_PANIC:
            collect_local_vars_expr(e->as.panic_.payload, out, n, cap);
            return;
        case EX_TRY:
            collect_local_vars_expr(e->as.try_.body, out, n, cap);
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++)
                collect_local_vars_expr(e->as.try_.clauses[i].handler, out, n, cap);
            collect_local_vars_expr(e->as.try_.finally_body, out, n, cap);
            return;
        case EX_DEFER:
            collect_local_vars_expr(e->as.defer_.body, out, n, cap);
            return;
        case EX_RESET:
            collect_local_vars_expr(e->as.reset_.body, out, n, cap);
            return;
        case EX_SHIFT:
            collect_local_vars_expr(e->as.shift_.k_fn, out, n, cap);
            collect_local_vars_expr(e->as.shift_.body, out, n, cap);
            return;
        case EX_SHIFT0:
            collect_local_vars_expr(e->as.shift0_.k_fn, out, n, cap);
            collect_local_vars_expr(e->as.shift0_.body, out, n, cap);
            return;
        case EX_CLONEABLE_RESET:
            collect_local_vars_expr(e->as.cloneable_reset_.body, out, n, cap);
            return;
        case EX_CLONEABLE_SHIFT:
            collect_local_vars_expr(e->as.cloneable_shift_.k_fn, out, n, cap);
            collect_local_vars_expr(e->as.cloneable_shift_.body, out, n, cap);
            return;
        /* Closures/fns: do NOT recurse into nested fn bodies — they have their
         * own capture analysis.  The closure itself may reference locals via
         * EX_VAR in the capture list, but EX_CLOSURE doesn't expose that here.
         * Fine for the conservative over-approximation. */
        default:
            return;
    }
}

/* Recursively patch all EX_CLONEABLE_SHIFT nodes in e with the given locals
 * (forward-declared so cps_compute_live_at_shift can call it). */
static void patch_cloneable_shifts(Arena *a, Expr *e,
                                   Binding **locals, uint32_t n_locals);

/* Populate live_captures on every EX_CLONEABLE_SHIFT inside fn_body.
 * Conservative: all local vars in the entire fn body become live_captures. */
static void cps_compute_live_at_shift(Arena *a, Expr *fn_body) {
    if (!fn_body) return;
    if (!cps_expr_contains_cloneable_shift(fn_body)) return;

    Binding **locals = NULL;
    uint32_t n_locals = 0, cap_locals = 0;
    collect_local_vars_expr(fn_body, &locals, &n_locals, &cap_locals);

    if (n_locals == 0) {
        free(locals);
        return;
    }

    /* Copy into arena (shared across all shift sites in this fn). */
    Binding **arena_locals = arena_alloc(a, n_locals * sizeof(Binding *));
    memcpy(arena_locals, locals, n_locals * sizeof(Binding *));
    free(locals);

    patch_cloneable_shifts(a, fn_body, arena_locals, n_locals);
}

/* Recursively patch all EX_CLONEABLE_SHIFT nodes in e with the given locals. */
static void patch_cloneable_shifts(Arena *a, Expr *e,
                                   Binding **locals, uint32_t n_locals) {
    if (!e) return;
    (void)a; /* arena already used to allocate locals */
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT:
            e->as.cloneable_shift_.live_captures  = locals;
            e->as.cloneable_shift_.n_live_captures = n_locals;
            patch_cloneable_shifts(a, e->as.cloneable_shift_.k_fn,  locals, n_locals);
            patch_cloneable_shifts(a, e->as.cloneable_shift_.body,  locals, n_locals);
            return;
        case EX_CLONEABLE_RESET:
            patch_cloneable_shifts(a, e->as.cloneable_reset_.body, locals, n_locals);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                patch_cloneable_shifts(a, e->as.let_.bindings[i].init, locals, n_locals);
            patch_cloneable_shifts(a, e->as.let_.body, locals, n_locals);
            return;
        case EX_IF:
            patch_cloneable_shifts(a, e->as.if_.cond, locals, n_locals);
            patch_cloneable_shifts(a, e->as.if_.then_, locals, n_locals);
            patch_cloneable_shifts(a, e->as.if_.else_or_null, locals, n_locals);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                patch_cloneable_shifts(a, e->as.do_.items[i], locals, n_locals);
            return;
        case EX_WHILE:
            patch_cloneable_shifts(a, e->as.while_.cond, locals, n_locals);
            patch_cloneable_shifts(a, e->as.while_.body, locals, n_locals);
            return;
        case EX_RETURN:
            patch_cloneable_shifts(a, e->as.return_.value, locals, n_locals);
            return;
        case EX_THROW:
            patch_cloneable_shifts(a, e->as.throw_.payload, locals, n_locals);
            return;
        case EX_DEFER:
            patch_cloneable_shifts(a, e->as.defer_.body, locals, n_locals);
            return;
        case EX_TRY:
            patch_cloneable_shifts(a, e->as.try_.body, locals, n_locals);
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++)
                patch_cloneable_shifts(a, e->as.try_.clauses[i].handler, locals, n_locals);
            patch_cloneable_shifts(a, e->as.try_.finally_body, locals, n_locals);
            return;
        case EX_CALL:
            patch_cloneable_shifts(a, e->as.call_.fn_expr, locals, n_locals);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                patch_cloneable_shifts(a, e->as.call_.args[i], locals, n_locals);
            return;
        default:
            return;
    }
}

/* Helper: mark a function as may_capture (for future one-shot CPS) */
static void mark_fn_may_capture(FnDef *fd) {
    if (fd) {
        fd->may_capture = true;
    }
}

/* Phase B2: mark a function as needing cloneable CPS.
 * Sets may_capture so the emitter knows the function interacts with
 * continuations, even before the full cloneable CPS pass is implemented. */
static void mark_fn_needs_cloneable_cps(FnDef *fd) {
    if (fd) {
        fd->may_capture = true;  /* subsumes may_capture for now */
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
            /* Phase B2: also mark functions needing cloneable CPS */
            if (cps_fn_needs_cloneable_transform(fd)) {
                mark_fn_needs_cloneable_cps(fd);
            }
            FnDef *new_fd = arena_alloc(a, sizeof(FnDef));
            *new_fd = *fd;
            new_fd->body = cps_mark_expr(a, fd->body);
            /* CPS-CL1: populate live_captures at each cloneable-shift site */
            if (cps_fn_needs_cloneable_transform(new_fd)) {
                cps_compute_live_at_shift(a, new_fd->body);
            }
            Expr *out = expr_new(a, EX_FN_DEF, e->type, e->span);
            out->as.fn_def_.fn = new_fd;
            return out;
        }

        case EX_FN: {
            FnDef *fn = e->as.fn_.fn;
            if (cps_fn_needs_transform(fn)) {
                mark_fn_may_capture(fn);
            }
            /* Phase B2: also mark functions needing cloneable CPS */
            if (cps_fn_needs_cloneable_transform(fn)) {
                mark_fn_needs_cloneable_cps(fn);
            }
            FnDef *new_fn = arena_alloc(a, sizeof(FnDef));
            *new_fn = *fn;
            new_fn->body = cps_mark_expr(a, fn->body);
            /* CPS-CL1: populate live_captures at each cloneable-shift site */
            if (cps_fn_needs_cloneable_transform(new_fn)) {
                cps_compute_live_at_shift(a, new_fn->body);
            }
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
        case EX_DICT:
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
        
        /* Phase B2: Cloneable continuations - pass through but mark */
        case EX_CLONEABLE_RESET: {
            Expr *new_body = cps_mark_expr(a, e->as.cloneable_reset_.body);
            Expr *out = expr_new(a, EX_CLONEABLE_RESET, e->type, e->span);
            out->as.cloneable_reset_.body = new_body;
            return out;
        }
        
        case EX_CLONEABLE_SHIFT: {
            Expr *new_k_fn = cps_mark_expr(a, e->as.cloneable_shift_.k_fn);
            Expr *new_body = cps_mark_expr(a, e->as.cloneable_shift_.body);
            Expr *out = expr_new(a, EX_CLONEABLE_SHIFT, e->type, e->span);
            out->as.cloneable_shift_.k_fn = new_k_fn;
            out->as.cloneable_shift_.body = new_body;
            /* live_captures will be filled by cps_compute_live_at_shift */
            out->as.cloneable_shift_.live_captures   = NULL;
            out->as.cloneable_shift_.n_live_captures = 0;
            /* CPS-CL3: cont_body is set later if needed for full continuation splitting */
            out->as.cloneable_shift_.cont_body = NULL;
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

/* CPS-CL10: Walk the program and for every EX_CLONEABLE_SHIFT node, verify
 * that each live-captured binding's type has a Clone instance in tc_env.
 * Emits TUR-E0014 for each binding that does not satisfy the constraint. */
static void cps_check_cloneable_captures_expr(const Expr *e, TypeClassEnv *tc_env,
                                               TypeClass *clone_tc) {
    if (!e || !clone_tc) return;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT: {
            for (uint32_t i = 0; i < e->as.cloneable_shift_.n_live_captures; i++) {
                Binding *b = e->as.cloneable_shift_.live_captures[i];
                if (!b) continue;
                Type t = b->type;
                TypeClassInstance *inst =
                    typeclass_env_lookup_instance(tc_env, clone_tc, &t, 1);
                if (!inst) {
                    const char *bname = (b->name && b->name->name) ? b->name->name : "<unknown>";
                    diag_emit_with_code(DIAG_ERROR, e->span,
                                        TUR_E0014_NOT_CLONE,
                                        "captured binding '%s' does not implement Clone "
                                        "(required by cloneable-shift)", bname);
                }
            }
            cps_check_cloneable_captures_expr(e->as.cloneable_shift_.k_fn,  tc_env, clone_tc);
            cps_check_cloneable_captures_expr(e->as.cloneable_shift_.body,  tc_env, clone_tc);
            return;
        }
        case EX_CLONEABLE_RESET:
            cps_check_cloneable_captures_expr(e->as.cloneable_reset_.body, tc_env, clone_tc);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                cps_check_cloneable_captures_expr(e->as.let_.bindings[i].init, tc_env, clone_tc);
            cps_check_cloneable_captures_expr(e->as.let_.body, tc_env, clone_tc);
            return;
        case EX_IF:
            cps_check_cloneable_captures_expr(e->as.if_.cond,         tc_env, clone_tc);
            cps_check_cloneable_captures_expr(e->as.if_.then_,        tc_env, clone_tc);
            cps_check_cloneable_captures_expr(e->as.if_.else_or_null, tc_env, clone_tc);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                cps_check_cloneable_captures_expr(e->as.do_.items[i], tc_env, clone_tc);
            return;
        case EX_WHILE:
            cps_check_cloneable_captures_expr(e->as.while_.cond, tc_env, clone_tc);
            cps_check_cloneable_captures_expr(e->as.while_.body, tc_env, clone_tc);
            return;
        case EX_RETURN:
            cps_check_cloneable_captures_expr(e->as.return_.value, tc_env, clone_tc);
            return;
        case EX_CALL:
            cps_check_cloneable_captures_expr(e->as.call_.fn_expr, tc_env, clone_tc);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                cps_check_cloneable_captures_expr(e->as.call_.args[i], tc_env, clone_tc);
            return;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn)
                cps_check_cloneable_captures_expr(e->as.fn_def_.fn->body, tc_env, clone_tc);
            return;
        case EX_FN:
            if (e->as.fn_.fn)
                cps_check_cloneable_captures_expr(e->as.fn_.fn->body, tc_env, clone_tc);
            return;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                cps_check_cloneable_captures_expr(e->as.program.items[i], tc_env, clone_tc);
            return;
        default:
            return;
    }
}

/* CPS-CL10: Entry point — find the "Clone" typeclass and check all captured
 * bindings at every cloneable-shift site in the program. */
static void cps_check_cloneable_captures(Expr *program, TypeClassEnv *tc_env) {
    if (!program || !tc_env) return;
    /* Find the Clone typeclass by iterating the linked list and comparing names */
    TypeClass *clone_tc = NULL;
    for (TypeClass *tc = tc_env->typeclasses; tc != NULL; tc = tc->next) {
        if (tc->name && strcmp(tc->name->name, "Clone") == 0) {
            clone_tc = tc;
            break;
        }
    }
    if (!clone_tc) return; /* No Clone typeclass defined; nothing to check */
    cps_check_cloneable_captures_expr(program, tc_env, clone_tc);
}

/* Main entry point: mark functions that contain shift for CPS transformation */
Expr *cps_transform(Arena *a, Expr *program, TypeClassEnv *tc_env) {
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
    
    /* Mark all functions that contain shift and populate live_captures */
    Expr *result = cps_mark_expr(a, program);

    /* CPS-CL10: check Clone instances for all captured bindings */
    if (result && tc_env) cps_check_cloneable_captures(result, tc_env);

    return result;
}
