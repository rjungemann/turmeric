#include "effect_lower.h"

#include <string.h> /* For strlen */
#include "arena.h"
#include "diag.h"
#include "expr.h"
#include "effect.h"

/* Phase 19: Effect lowering pass
 * 
 * This pass transforms algebraic effect operations into delimited continuations:
 * - (perform (E args...)) -> (shift k (apply handler E args k))
 * - (handle expr cases...) -> (reset (let [handlers...] expr))
 * 
 * The lowering follows the OCaml 5-style approach described in effects-plan.md.
 * 
 * For v1, the lowering is a stub that just passes through the expressions.
 * The full implementation will be completed in a future pass.
 */

/* Check if an expression contains perform or handle */
static bool expr_contains_effects(const Expr *e) {
    if (!e) return false;
    
    switch (e->kind) {
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (expr_contains_effects(e->as.let_.bindings[i].init)) return true;
            }
            return expr_contains_effects(e->as.let_.body);
        case EX_IF:
            if (expr_contains_effects(e->as.if_.cond)) return true;
            if (expr_contains_effects(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null && expr_contains_effects(e->as.if_.else_or_null)) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_contains_effects(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_WHILE:
            return expr_contains_effects(e->as.while_.cond) ||
                   expr_contains_effects(e->as.while_.body);
        case EX_SET:
            return expr_contains_effects(e->as.set_.value);
        case EX_DEF:
            return e->as.def_.init && expr_contains_effects(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (expr_contains_effects(e->as.builtin.args[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return expr_contains_effects(e->as.fn_def_.fn->body);
        case EX_FN:
            return false; /* Function literals don't contain effects at this level */
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (expr_contains_effects(e->as.call_.args[i])) return true;
            }
            return false;
        case EX_CLOSURE:
            return expr_contains_effects(e->as.closure_.closure->fn->body);
        case EX_DEFER:
            return expr_contains_effects(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value && expr_contains_effects(e->as.return_.value);
        case EX_THROW:
            return expr_contains_effects(e->as.throw_.payload);
        case EX_TRY:
            if (expr_contains_effects(e->as.try_.body)) return true;
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                if (expr_contains_effects(e->as.try_.clauses[i].handler)) return true;
            }
            return e->as.try_.finally_body && expr_contains_effects(e->as.try_.finally_body);
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
            /* These are already lowered or primitive - no further lowering */
            return false;
        default:
            return false;
    }
}

/* Main entry point: lower all effect expressions in a program */
Expr *effect_lower(Arena *a, SymbolTable *st, Expr *program, EffectEnv *effect_env) {
    if (!program) return NULL;
    
    /* Check if the program contains any effects */
    if (!expr_contains_effects(program)) {
        return program; /* Nothing to lower */
    }
    
    /* For Phase 19 v1, the lowering is a stub that just passes through.
     * The full implementation will be completed in a future pass.
     * 
     * The actual lowering would:
     * 1. Transform each handle into reset + handler registration
     * 2. Transform each perform into shift + handler dispatch
     * 3. Manage the handler stack at runtime
     */
    
    return program;
}
