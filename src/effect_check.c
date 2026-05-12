/* effect_check.c — Effect-row inference pass (Phase P19-2). */

#include "effect_check.h"

#include <string.h>
#include "buf.h"
#include "diag.h"
#include "effect.h"
#include "expr.h"

/* ---------------------------------------------------------------------------
 * Binding → FnDef index
 * A simple flat array used to look up the FnDef for a callee binding during
 * propagation.  Sized conservatively; top-level defn counts rarely exceed a
 * few hundred in realistic programs.
 * --------------------------------------------------------------------------- */

#define FN_INDEX_CAP 1024

typedef struct {
    const Binding *binding;
    FnDef         *fn;
} FnEntry;

typedef struct {
    FnEntry  entries[FN_INDEX_CAP];
    uint32_t n;
} FnIndex;

static Effect *find_unsafe_effect(EffectEnv *env) {
    if (!env) return NULL;
    for (uint32_t i = 0; i < env->n_effects; i++) {
        Effect *eff = env->effects[i];
        if (eff && eff->name && eff->name->name &&
            strcmp(eff->name->name, EFFECT_NAME_UNSAFE) == 0) {
            return eff;
        }
    }
    return NULL;
}

static void fn_index_add(FnIndex *idx, const Binding *b, FnDef *fn) {
    if (idx->n >= FN_INDEX_CAP) return; /* silently drop if full */
    idx->entries[idx->n].binding = b;
    idx->entries[idx->n].fn      = fn;
    idx->n++;
}

static FnDef *fn_index_lookup(const FnIndex *idx, const Binding *b) {
    for (uint32_t i = 0; i < idx->n; i++) {
        if (idx->entries[i].binding == b) return idx->entries[i].fn;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Single-function effect-row inference
 * --------------------------------------------------------------------------- */

/* Walk an expression tree and collect into *row all effects that are directly
 * performed (EX_PERFORM) or transitively performed (EX_CALL to a known defn
 * whose inferred row is already populated).
 *
 * Phase P19-4: `subst` is an effect-row substitution used to resolve row
 * variables when a polymorphic callee is instantiated at a call site.  Pass a
 * fresh empty EffectRowSubst for each top-level function analysis. */
static EffectRow *collect_effects_in_expr(Arena *a, Expr *e,
                                          EffectRow *row,
                                          const FnIndex *idx,
                                          EffectEnv *env,
                                          EffectRowSubst *subst) {
    if (!e) return row;

    switch (e->kind) {
    case EX_PERFORM: {
        /* Look up the effect by name in the environment. */
        const PerformExpr *p = e->as.perform_.perform;
        Effect *eff = effect_env_lookup(env, p->effect_name);
        if (eff) {
            EffectRow *single = effect_row_single(a, eff);
            row = effect_row_merge(a, row, single);
        }
        /* Also recurse into arguments. */
        for (uint8_t i = 0; i < p->n_args; i++) {
            row = collect_effects_in_expr(a, p->args[i], row, idx, env, subst);
        }
        return row;
    }

    case EX_CALL: {
        /* Phase P19-4: propagate from a known callee's already-inferred row,
         * applying any row-variable substitutions before merging. */

        /* Phase 16 v2: indirect capability field call — fn_expr is EX_GET_FIELD.
         * Propagate the effect-row annotation on the :fn struct field (if any). */
        if (e->as.call_.fn_expr &&
            e->as.call_.fn_expr->kind == EX_GET_FIELD) {
            Expr *gf = e->as.call_.fn_expr;
            StructDef *def = gf->as.get_field_.def;
            uint32_t fidx = gf->as.get_field_.field_idx;
            if (def && fidx < def->n_fields && def->fields[fidx].effect_row) {
                EffectRow *field_row = effect_row_apply_subst(
                    def->fields[fidx].effect_row, subst, a);
                row = effect_row_merge(a, row, field_row);
            }
            /* Also recurse into the struct expression and arguments. */
            row = collect_effects_in_expr(a, gf->as.get_field_.struct_expr, row, idx, env, subst);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                row = collect_effects_in_expr(a, e->as.call_.args[i], row, idx, env, subst);
            }
            return row;
        }

        FnDef *callee = fn_index_lookup(idx, e->as.call_.fn_binding);
        if (callee && callee->inferred_effect_row) {
            /* Apply substitution to resolve any row variables in the callee's
             * inferred row (e.g., from a polymorphic higher-order parameter). */
            EffectRow *callee_row =
                effect_row_apply_subst(callee->inferred_effect_row, subst, a);
            row = effect_row_merge(a, row, callee_row);
            /* Explicit unsafe annotation on the callee should also propagate. */
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (callee->binding &&
                callee->binding->type.kind == TY_FN &&
                callee->binding->type.as.fn.effect_row &&
                unsafe_eff &&
                effect_row_contains(callee->binding->type.as.fn.effect_row, unsafe_eff)) {
                row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
            }
        } else if (e->as.call_.fn_binding) {
            /* Callee not in the top-level index (e.g., a higher-order param).
             * If the binding's declared effect row has been resolved via the
             * current substitution, contribute those effects. */
            EffectRow *decl_row = e->as.call_.fn_binding->type.as.fn.effect_row;
            if (decl_row) {
                EffectRow *resolved = effect_row_apply_subst(decl_row, subst, a);
                if (resolved && resolved->kind == ERK_CONCRETE) {
                    row = effect_row_merge(a, row, resolved);
                }
            }
        }
        /* Recurse into arguments. */
        for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
            row = collect_effects_in_expr(a, e->as.call_.args[i], row, idx, env, subst);
        }
        return row;
    }

    case EX_LET: {
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            row = collect_effects_in_expr(a, e->as.let_.bindings[i].init, row, idx, env, subst);
        }
        return collect_effects_in_expr(a, e->as.let_.body, row, idx, env, subst);
    }

    case EX_DO: {
        for (uint32_t i = 0; i < e->as.do_.n; i++) {
            row = collect_effects_in_expr(a, e->as.do_.items[i], row, idx, env, subst);
        }
        return row;
    }

    case EX_IF:
        row = collect_effects_in_expr(a, e->as.if_.cond,       row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.if_.then_,      row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.if_.else_or_null, row, idx, env, subst);
        return row;

    case EX_WHILE:
        row = collect_effects_in_expr(a, e->as.while_.cond, row, idx, env, subst);
        row = collect_effects_in_expr(a, e->as.while_.body, row, idx, env, subst);
        return row;

    case EX_SET:
        return collect_effects_in_expr(a, e->as.set_.value, row, idx, env, subst);

    case EX_DEF:
        return collect_effects_in_expr(a, e->as.def_.init, row, idx, env, subst);

    case EX_BUILTIN:
        for (uint32_t i = 0; i < e->as.builtin.n; i++) {
            row = collect_effects_in_expr(a, e->as.builtin.args[i], row, idx, env, subst);
        }
        return row;

    case EX_RETURN:
        return collect_effects_in_expr(a, e->as.return_.value, row, idx, env, subst);

    case EX_THROW:
        return collect_effects_in_expr(a, e->as.throw_.payload, row, idx, env, subst);

    case EX_PANIC:
        return collect_effects_in_expr(a, e->as.panic_.payload, row, idx, env, subst);

    case EX_TRY:
        row = collect_effects_in_expr(a, e->as.try_.body, row, idx, env, subst);
        for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
            row = collect_effects_in_expr(a, e->as.try_.clauses[i].handler, row, idx, env, subst);
        }
        return collect_effects_in_expr(a, e->as.try_.finally_body, row, idx, env, subst);

    case EX_DEFER:
        return collect_effects_in_expr(a, e->as.defer_.body, row, idx, env, subst);

    case EX_RESET:
        return collect_effects_in_expr(a, e->as.reset_.body, row, idx, env, subst);

    case EX_SHIFT:
        row = collect_effects_in_expr(a, e->as.shift_.k_fn, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.shift_.body, row, idx, env, subst);

    case EX_SHIFT0:
        row = collect_effects_in_expr(a, e->as.shift0_.k_fn, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.shift0_.body, row, idx, env, subst);

    case EX_HANDLE: {
        /* Effects performed inside a handle body are handled (absorbed) by the
         * handler when a matching case exists; only effects NOT covered by a
         * case bubble up to the caller.
         *
         * Effect re-opening: a handler case body may itself perform effects
         * (e.g., it calls (perform Write ...) while handling a Read effect).
         * Those effects propagate to the enclosing function.
         *
         * Algorithm:
         *   1. Collect the body's effect row into body_row.
         *   2. For each handler case: remove its handled effect from body_row
         *      (it is absorbed here) and collect the case body's effects into
         *      the caller's row (they are re-opened / propagated outward).
         *   3. Merge any unhandled body effects into the caller's row. */
        HandleExpr *h = e->as.handle_.handle;
        EffectRow *body_row = collect_effects_in_expr(
            a, h->body, effect_row_empty(a), idx, env, subst);
        for (uint8_t i = 0; i < h->n_cases; i++) {
            /* Absorb the handled effect from the body row. */
            body_row = effect_row_remove(body_row, h->cases[i].effect_name, a);
            /* Propagate effects performed inside the handler case body. */
            row = collect_effects_in_expr(a, h->cases[i].body, row, idx, env, subst);
        }
        /* Any body effects that were not covered by a case propagate upward. */
        row = effect_row_merge(a, row, body_row);
        return row;
    }

    case EX_RESUME:
        row = collect_effects_in_expr(a, e->as.resume_.resume->k, row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.resume_.resume->value, row, idx, env, subst);

    case EX_REF:
        return collect_effects_in_expr(a, e->as.ref_.expr, row, idx, env, subst);
    case EX_DEREF:
        if (e->as.deref_.expr && e->as.deref_.expr->type.kind == TY_PTR_VOID) {
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (unsafe_eff) row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
        }
        return collect_effects_in_expr(a, e->as.deref_.expr, row, idx, env, subst);

    case EX_BORROW_IMMUT:
        if (e->as.borrow_immut_.expr &&
            e->as.borrow_immut_.expr->type.kind == TY_PTR_VOID) {
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (unsafe_eff) row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
        }
        return collect_effects_in_expr(a, e->as.borrow_immut_.expr, row, idx, env, subst);
    case EX_BORROW_MUT:
        if (e->as.borrow_mut_.expr &&
            e->as.borrow_mut_.expr->type.kind == TY_PTR_VOID) {
            Effect *unsafe_eff = find_unsafe_effect(env);
            if (unsafe_eff) row = effect_row_merge(a, row, effect_row_single(a, unsafe_eff));
        }
        return collect_effects_in_expr(a, e->as.borrow_mut_.expr, row, idx, env, subst);

    case EX_RC_OF:
        return collect_effects_in_expr(a, e->as.rc_of_.expr, row, idx, env, subst);
    case EX_RC_CLONE:
        return collect_effects_in_expr(a, e->as.rc_clone_.expr, row, idx, env, subst);
    case EX_RC_DROP:
        return collect_effects_in_expr(a, e->as.rc_drop_.expr, row, idx, env, subst);
    case EX_RC_PTR:
        return collect_effects_in_expr(a, e->as.rc_ptr_.expr, row, idx, env, subst);
    case EX_RC_COUNT:
        return collect_effects_in_expr(a, e->as.rc_count_.expr, row, idx, env, subst);
    case EX_RC_FROM_REF:
        return collect_effects_in_expr(a, e->as.rc_from_ref_.expr, row, idx, env, subst);
    case EX_REF_FROM_RC:
        return collect_effects_in_expr(a, e->as.ref_from_rc_.expr, row, idx, env, subst);
    case EX_WEAK:
        return collect_effects_in_expr(a, e->as.weak_.expr, row, idx, env, subst);
    case EX_WEAK_UPGRADE:
        return collect_effects_in_expr(a, e->as.weak_upgrade_.expr, row, idx, env, subst);
    case EX_WEAK_PRED:
        return collect_effects_in_expr(a, e->as.weak_pred_.expr, row, idx, env, subst);
    case EX_REF_PRED:
        return collect_effects_in_expr(a, e->as.ref_pred_.expr, row, idx, env, subst);

    case EX_CLOSURE:
        return collect_effects_in_expr(a, e->as.closure_.closure->fn->body, row, idx, env, subst);

    case EX_FN_DEF:
        /* Inner defn: do not recurse — that defn is processed separately. */
        return row;

    case EX_MAKE_STRUCT:
        for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
            row = collect_effects_in_expr(a, e->as.make_struct_.field_values[i], row, idx, env, subst);
        }
        return row;

    case EX_GET_FIELD:
        return collect_effects_in_expr(a, e->as.get_field_.struct_expr, row, idx, env, subst);

    case EX_CONT_PRED:
        return collect_effects_in_expr(a, e->as.cont_pred_.expr, row, idx, env, subst);

    case EX_SET_DEREF:
        row = collect_effects_in_expr(a, e->as.set_deref_.ref,   row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.set_deref_.value, row, idx, env, subst);

    case EX_DISCONTINUE:
        row = collect_effects_in_expr(a, e->as.discontinue_.discontinue->k,         row, idx, env, subst);
        return collect_effects_in_expr(a, e->as.discontinue_.discontinue->exception, row, idx, env, subst);

    default:
        return row;
    }
}

/* ---------------------------------------------------------------------------
 * effect_row_check_declared
 * --------------------------------------------------------------------------- */

int effect_row_check_declared(FnDef *fd, Arena *a) {
    if (!fd) return 0;

    /* Declared row lives in the function type. */
    EffectRow *declared = fd->binding
                            ? fd->binding->type.as.fn.effect_row
                            : NULL;

    /* Unannotated functions are unconstrained in v1 — skip the check. */
    if (!declared) return 0;

    EffectRow *inferred = fd->inferred_effect_row;
    if (!inferred) inferred = effect_row_empty(a);

    if (effect_row_is_subset(inferred, declared)) return 0;

    /* Inferred row is not a subset of the declared row: emit TUR-E0009 for each
     * violating effect so the user knows exactly which effect is undeclared. */
    int rc = 0;
    Span fn_span = fd->binding ? fd->binding->span : (Span){0, 0, 0, 0, 0, 0};

    if (inferred->kind == ERK_CONCRETE) {
        for (uint8_t i = 0; i < inferred->as.concrete.n_effects; i++) {
            Effect *eff = inferred->as.concrete.effects[i];
            if (!effect_row_contains(declared, eff)) {
                /* Build a human-readable declared-row string. */
                Buf decl_str;
                buf_init(&decl_str);
                effect_row_print(&decl_str, declared);

                diag_emit_with_code(DIAG_ERROR, fn_span,
                    TUR_E0009_EFFECT_ROW_MISMATCH,
                    "function '%s' performs effect '%s' but its declared row %.*s "
                    "does not include it",
                    fd->binding ? fd->binding->name->name : "<anonymous>",
                    eff->name->name,
                    (int)decl_str.len, decl_str.data);
                buf_free(&decl_str);
                rc = 1;
            }
        }
    } else {
        /* Non-concrete inferred row: emit a generic mismatch. */
        Buf inferred_str, decl_str;
        buf_init(&inferred_str);
        buf_init(&decl_str);
        effect_row_print(&inferred_str, inferred);
        effect_row_print(&decl_str, declared);

        diag_emit_with_code(DIAG_ERROR, fn_span,
            TUR_E0009_EFFECT_ROW_MISMATCH,
            "function '%s': inferred effect row %.*s is not a subset of declared row %.*s",
            fd->binding ? fd->binding->name->name : "<anonymous>",
            (int)inferred_str.len, inferred_str.data,
            (int)decl_str.len,    decl_str.data);
        buf_free(&inferred_str);
        buf_free(&decl_str);
        rc = 1;
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 * effect_check_pass
 * --------------------------------------------------------------------------- */

int effect_check_pass(Arena *a, Expr *program, EffectEnv *env) {
    if (!program || program->kind != EX_PROGRAM) return 0;
    if (!env) return 0;

    /* --- Step -1: Populate the effect env from EX_DEFECT nodes.
     * PASS_EFFECT_LOWER creates a fresh empty env; we scan the elaborated AST
     * for (defeffect ...) nodes and register each one so the env is populated
     * before ERK_UNRESOLVED resolution and inference run. --- */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_DEFECT || !item->as.effect_def_.def) continue;
        EffectDef *def = item->as.effect_def_.def;
        /* Skip if already registered (idempotent). */
        if (!effect_env_contains(env, def->name)) {
            effect_env_register(env, a, def->name,
                                def->param_names, def->param_types,
                                def->n_params, def->result_type);
        }
    }

    /* --- Step 0: Resolve ERK_UNRESOLVED declared effect rows.
     * Effect row annotations are parsed during elaboration as ERK_UNRESOLVED
     * (symbolic names).  Now that PASS_EFFECT_LOWER has populated the effect
     * environment, we can resolve each annotation to ERK_CONCRETE/ERK_VAR. --- */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        FnDef *fn = item->as.fn_def_.fn;
        if (fn->binding && fn->binding->type.as.fn.effect_row) {
            EffectRow *decl = fn->binding->type.as.fn.effect_row;
            if (decl->kind == ERK_UNRESOLVED) {
                fn->binding->type.as.fn.effect_row =
                    effect_row_resolve(decl, env, a);
            }
        }
    }

    /* --- Step 1: Collect all top-level FnDef nodes into an index. --- */
    FnIndex idx;
    memset(&idx, 0, sizeof(idx));

    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (item && item->kind == EX_FN_DEF && item->as.fn_def_.fn) {
            FnDef *fn = item->as.fn_def_.fn;
            if (fn->binding) {
                fn_index_add(&idx, fn->binding, fn);
            }
        }
    }

    /* --- Step 2: Fixed-point iteration.
     * On each sweep, recompute every FnDef's inferred row.  Repeat until no
     * row grows (monotone — rows only ever gain effects).               --- */
    bool changed = true;
    while (changed) {
        changed = false;

        for (uint32_t i = 0; i < program->as.program.n; i++) {
            Expr *item = program->as.program.items[i];
            if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;

            FnDef *fn = item->as.fn_def_.fn;

            /* Phase P19-4: create a fresh substitution for each function analysis.
             * This allows row-variable bindings to be computed per-invocation. */
            EffectRowSubst *subst = effect_row_subst_new(a);

            /* Start with the empty row for this iteration. */
            EffectRow *fresh = collect_effects_in_expr(
                a, fn->body, effect_row_empty(a), &idx, env, subst);

            /* Check if the row grew compared to the previous iteration. */
            EffectRow *prev = fn->inferred_effect_row;
            if (!prev || !effect_row_is_subset(fresh, prev) ||
                         !effect_row_is_subset(prev, fresh)) {
                fn->inferred_effect_row = fresh;
                changed = true;
            }
        }
    }

    /* --- Step 3: Validate each annotated function against its declared row. --- */
    int rc = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *item = program->as.program.items[i];
        if (!item || item->kind != EX_FN_DEF || !item->as.fn_def_.fn) continue;
        if (effect_row_check_declared(item->as.fn_def_.fn, a) != 0) rc = 1;
    }
    return rc;
}
