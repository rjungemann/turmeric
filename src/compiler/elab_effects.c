/* elab_effects.c -- delimited continuations and algebraic effects. */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static void check_cloneable_capture(Elab *e, Span span);
static void check_serializable_capture(Elab *e, Span span);
static bool is_effect_handled(Elab *e, const Symbol *name);
static void push_handled_effect(Elab *e, const Symbol *name);
static bool elab_effect_is_referred(const Elab *e, const Effect *eff);
static void cont_mark_consumed(Expr *k);

/* Phase 18: Delimited continuations */

/* CF2: Derive the delimited-continuation result type for shift/shift0.
 *
 * Typing rule (v1 semantics: `(shift f body)` evaluates to `(f body)`):
 *   f : A -> B          -- the continuation receiver (1 value argument)
 *   body : A            -- the value handed to the receiver
 *   ------------------------------------------------------------------
 *   (shift f body) : B  -- the receiver's *result* type, not body's type
 *
 * The earlier placeholder reused `body->type` (the receiver's *domain* A),
 * which only coincides with B when f's domain equals its codomain.  This walks
 * f to recover both A (domain) and B (codomain).  A captured-closure receiver
 * carries the env as parameter 0, so the value parameter is at index 1.
 * Returns false when f's function type is not statically available. */
static bool shift_fn_domain_codomain(const Expr *k, Type *domain, Type *codomain) {
    const Type *ft = NULL;
    uint8_t val_idx = 0;
    if (k->kind == EX_CLOSURE) {
        struct Closure *c = k->as.closure_.closure;
        if (c && c->fn && c->fn->binding && c->fn->binding->type.kind == TY_FN) {
            ft = &c->fn->binding->type;
            val_idx = 1;  /* env occupies parameter 0 */
        }
    } else if (k->type.kind == TY_FN) {
        ft = &k->type;
    } else if (k->kind == EX_VAR && k->as.var.binding &&
               k->as.var.binding->type.kind == TY_FN) {
        ft = &k->as.var.binding->type;
    }
    if (!ft || ft->as.fn.arity <= val_idx) return false;

    if (ft->as.fn.arg_full_types && ft->as.fn.arg_full_types[val_idx]) {
        *domain = *ft->as.fn.arg_full_types[val_idx];
    } else {
        TypeKind dk = ft->as.fn.arg_kinds[val_idx];
        *domain = type_simple(dk, typekind_default_copy_kind(dk));
    }
    if (ft->as.fn.result_full_type) {
        *codomain = *ft->as.fn.result_full_type;
    } else {
        TypeKind rk = ft->as.fn.result_kind;
        *codomain = type_simple(rk, typekind_default_copy_kind(rk));
    }
    return true;
}

/* CF2: shared shift/shift0 result-typing.  Verifies the body matches the
 * receiver's domain (rejecting a mistyped shift) and returns the receiver's
 * codomain as the expression's type.  Falls back to `body->type` only when the
 * receiver's type is not statically known (or its codomain is unresolved), so
 * the placeholder behavior is preserved exactly where no better type exists.
 * `form` is "shift" or "shift0" for diagnostics; `body_span` locates the body.
 * On a type error emits TUR-E0001 and sets *ok = false. */
static Type shift_result_type(const Expr *k, const Expr *body,
                              const char *form, Span body_span, bool *ok) {
    *ok = true;
    Type domain, codomain;
    if (!shift_fn_domain_codomain(k, &domain, &codomain))
        return body->type;  /* receiver type unknown -- preserve prior behavior */

    /* Reject a mistyped shift: the body is the value passed to the continuation
     * receiver, so it must match the receiver's parameter type. */
    if (domain.kind != TY_UNKNOWN && body->type.kind != TY_UNKNOWN &&
        !type_eq(body->type, domain)) {
        diag_emit_with_code(DIAG_ERROR, body_span, TUR_E0001_TYPE_MISMATCH,
            "%s: body type mismatch -- the continuation receiver expects %s, "
            "but the body has type %s",
            form, type_name(domain), type_name(body->type));
        *ok = false;
        return body->type;
    }

    return codomain.kind != TY_UNKNOWN ? codomain : body->type;
}

/* (reset body) - Establish a continuation boundary.
 */
Expr *elab_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(reset body) requires exactly one argument");
        return NULL;
    }
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;
    Expr *out = expr_new(e->arena, EX_RESET, body->type, call->span);
    out->as.reset_.body = body;
    return out;
}

/* (shift k body) - Capture the current continuation.
 */
Expr *elab_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(shift k body) requires exactly two arguments");
        return NULL;
    }
    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;
    
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "shift requires a function as first argument");
        return NULL;
    }
    
    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* CF2: the result type of (shift f body) is f's codomain (the result of
     * calling f with body's value), not body's type.  Also rejects a body whose
     * type does not match f's parameter type. */
    bool ok = true;
    Type result_type = shift_result_type(k_expr, body, "shift",
                                         call->as.list.items[2]->span, &ok);
    if (!ok) return NULL;
    Expr *out = expr_new(e->arena, EX_SHIFT, result_type, call->span);
    out->as.shift_.k_fn = k_expr;
    out->as.shift_.body = body;
    return out;
}

/* (shift0 k body) - One-shot shift.
 */
Expr *elab_shift0(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(shift0 k body) requires exactly two arguments");
        return NULL;
    }
    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;
    
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "shift0 requires a function as first argument");
        return NULL;
    }
    
    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* CF2: result type is f's codomain (see elab_shift).  shift0 differs from
     * shift only in delimiter behavior at runtime, not in this local typing. */
    bool ok = true;
    Type result_type = shift_result_type(k_expr, body, "shift0",
                                         call->as.list.items[2]->span, &ok);
    if (!ok) return NULL;
    Expr *out = expr_new(e->arena, EX_SHIFT0, result_type, call->span);
    out->as.shift0_.k_fn = k_expr;
    out->as.shift0_.body = body;
    return out;
}

/* Phase B2: Cloneable continuations */

/* CPS-CL10 (elab-time): Walk all local bindings visible at a cloneable-shift
 * site and emit TUR-E0014 for any that lack a Clone instance.  This is a
 * conservative check — it covers every binding in scope, not just those that
 * are actually live at the shift; liveness-precise checking would require the
 * CPS pass.  Running early (in elab) gives diagnostics before codegen. */
static void check_cloneable_capture(Elab *e, Span span) {
    const Symbol *clone_sym = intern_cstr(e->st, "Clone");
    TypeClass *clone_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, clone_sym);
    if (!clone_tc) return; /* No Clone typeclass in scope; nothing to check */

    /* CF7.3: stop at the outer-function boundary so bindings from enclosing
     * functions (which are NOT captured in the continuation env) are not
     * falsely flagged as needing Clone.  For top-level defns fn_entry_outer_scope
     * is &e->global, giving the same behavior as before. */
    Scope *stop = e->fn_entry_outer_scope ? e->fn_entry_outer_scope : &e->global;
    for (Scope *s = e->scope; s != NULL && s != stop; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            Binding *b = s->bindings[i];
            if (!b || !b->name) continue;
            Type t = b->type;
            /* Primitive/function/continuation types are always safe to capture */
            if (t.kind == TY_NIL || t.kind == TY_FN ||
                t.kind == TY_CLONEABLE_CONT) continue;
            TypeClassInstance *inst =
                typeclass_env_lookup_instance(&e->typeclass_env, clone_tc, &t, 1);
            if (!inst) {
                diag_emit_with_code(DIAG_ERROR, span,
                                    TUR_E0014_NOT_CLONE,
                                    "captured binding '%s' does not implement Clone "
                                    "(required by cloneable-shift)", b->name->name);
            }
        }
    }
}

/* (cloneable-reset body) - Establish a continuation boundary with cloneable captures.
 * Similar to reset, but all captured values must implement Clone.
 * The body can use cloneable-shift to capture a cloneable continuation. */
Expr *elab_cloneable_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-reset body) requires exactly one argument");
        return NULL;
    }
    /* CPS-CL7: track nesting depth so cloneable-shift can detect missing reset */
    e->cloneable_reset_depth++;
    Expr *body = elab_form(e, call->as.list.items[1]);
    e->cloneable_reset_depth--;
    if (!body) return NULL;
    Expr *out = expr_new(e->arena, EX_CLONEABLE_RESET, body->type, call->span);
    out->as.cloneable_reset_.body = body;
    return out;
}

/* (cloneable-shift k body) - Capture the current cloneable continuation.
 * Similar to shift, but the continuation passed to k is cloneable (multi-shot).
 * All captured environment values must implement Clone.
 * k is a function that receives the cloneable continuation as its first argument.
 */
Expr *elab_cloneable_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-shift k body) requires exactly two arguments");
        return NULL;
    }

    /* CPS-CL7: cloneable-shift must be inside a cloneable-reset */
    if (e->cloneable_reset_depth == 0) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
                            "cloneable-shift used outside of any cloneable-reset boundary");
        return NULL;
    }

    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;

    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }

    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "cloneable-shift requires a function as first argument");
        return NULL;
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* The result type of cloneable-shift is the result type of calling k_fn with
     * a cloneable continuation and body's value. For now, use body's type as placeholder. */
    Expr *out = expr_new(e->arena, EX_CLONEABLE_SHIFT, body->type, call->span);
    out->as.cloneable_shift_.k_fn = k_expr;
    out->as.cloneable_shift_.body = body;
    out->as.cloneable_shift_.cont_body = NULL;
    /* CPS-CL10: verify all local captures implement Clone at elaboration time */
    check_cloneable_capture(e, call->span);
    return out;
}

/* (call/cc* f) - multi-shot call/cc that produces a cloneable continuation.
 *
 * CPS-CL8 / cps-transform-plan (CPS11): the continuation f receives is the one
 * captured up to the nearest *enclosing* cloneable-reset:
 *
 *   - Inside a cloneable-reset (cloneable_reset_depth > 0): desugar to a BARE
 *     (cloneable-shift f 0) that binds to that enclosing reset. The captured
 *     continuation is the real delimited context between the call/cc* and its
 *     reset -- e.g. (cloneable-reset (+ 10 (call/cc* f))) hands f a continuation
 *     that computes (+ 10 []) and is replayable multi-shot (CPS9 lowers it onto
 *     the DK machine).
 *   - With no enclosing cloneable-reset: desugar to (cloneable-reset
 *     (cloneable-shift f 0)) -- a freshly-installed delimiter whose captured
 *     continuation is empty/trivial. This is the original sugar and keeps
 *     call/cc* usable on its own (no reset required), matching the existing
 *     call-cc-star semantics (k is the identity continuation). */
Expr *elab_call_cc_star(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(call/cc* f) requires exactly one argument");
        return NULL;
    }
    Expr *f_expr = elab_form(e, call->as.list.items[1]);
    if (!f_expr) return NULL;

    /* The shift emitter allocates a continuation and passes it to k_fn (= f),
     * so f receives the continuation directly -- no wrapper needed. */

    /* Determine f's return type */
    Type call_result_type = TYPE_INT;
    if (f_expr->kind == EX_VAR && f_expr->as.var.binding
        && f_expr->as.var.binding->type.kind == TY_FN) {
        call_result_type = type_from_kind(
            f_expr->as.var.binding->type.as.fn.result_kind);
    }

    /* Build EX_INT_LIT for the default value 0 */
    Expr *zero = expr_new(e->arena, EX_INT_LIT, TYPE_INT, call->span);
    zero->as.i = 0;

    /* Build EX_CLONEABLE_SHIFT: (cloneable-shift f 0) */
    Expr *shift = expr_new(e->arena, EX_CLONEABLE_SHIFT, call_result_type, call->span);
    shift->as.cloneable_shift_.k_fn = f_expr;
    shift->as.cloneable_shift_.body = zero;
    shift->as.cloneable_shift_.live_captures = NULL;
    shift->as.cloneable_shift_.n_live_captures = 0;
    shift->as.cloneable_shift_.cont_body = NULL;

    /* Inside an enclosing cloneable-reset: capture up to it (real call/cc*
     * semantics). The bare shift binds to that reset's prompt. */
    if (e->cloneable_reset_depth > 0) {
        return shift;
    }

    /* No enclosing reset: install a fresh delimiter (trivial continuation). */
    Expr *reset = expr_new(e->arena, EX_CLONEABLE_RESET, call_result_type, call->span);
    reset->as.cloneable_reset_.body = shift;
    return reset;
}

/* Phase 21: Serializable continuations */

/* Check that all local bindings visible at a serial-shift site implement
 * the Serializable typeclass.  Mirrors check_cloneable_capture() for Clone. */
static void check_serializable_capture(Elab *e, Span span) {
    const Symbol *ser_sym = intern_cstr(e->st, "Serializable");
    TypeClass *ser_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, ser_sym);
    if (!ser_tc) return; /* Serializable not yet in scope; defer to a later pass */

    for (Scope *s = e->scope; s != NULL && s != &e->global; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            Binding *b = s->bindings[i];
            if (!b || !b->name) continue;
            Type t = b->type;
            /* Primitive types that are always serializable */
            if (t.kind == TY_NIL || t.kind == TY_FN ||
                t.kind == TY_CLONEABLE_CONT || t.kind == TY_CONT) continue;
            TypeClassInstance *inst =
                typeclass_env_lookup_instance(&e->typeclass_env, ser_tc, &t, 1);
            if (!inst) {
                diag_emit_with_code(DIAG_ERROR, span,
                                    TUR_E0018_NOT_SERIALIZABLE,
                                    "captured binding '%s' does not implement Serializable "
                                    "(required by serial-shift)\n"
                                    "  = help: use serial-reset outside non-serializable "
                                    "resources, or implement Serializable for the type",
                                    b->name->name);
            }
        }
    }
}

/* (serial-reset body) - Establish a serializable continuation boundary.
 * Like reset, but marks the region so serial-shift can capture it. */
Expr *elab_serial_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(serial-reset body) requires exactly one argument");
        return NULL;
    }
    e->serial_reset_depth++;
    Expr *body = elab_form(e, call->as.list.items[1]);
    e->serial_reset_depth--;
    if (!body) return NULL;
    Expr *out = expr_new(e->arena, EX_SERIAL_RESET, body->type, call->span);
    out->as.serial_reset_.body = body;
    return out;
}

/* (serial-shift k body) - Capture the current continuation as a serializable one.
 * k is a function that receives the serial-continuation as its argument.
 * All captured environment values must implement Serializable. */
Expr *elab_serial_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(serial-shift k body) requires exactly two arguments");
        return NULL;
    }

    if (e->serial_reset_depth == 0) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,
                            "serial-shift used outside of any serial-reset boundary");
        return NULL;
    }

    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;

    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "serial-shift requires a function as first argument");
        return NULL;
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;

    Expr *out = expr_new(e->arena, EX_SERIAL_SHIFT, body->type, call->span);
    out->as.serial_shift_.k_fn = k_expr;
    out->as.serial_shift_.body = body;

    /* Check that all captured bindings implement Serializable. */
    check_serializable_capture(e, call->span);
    return out;
}

/* (try-with body (EffectName [params] k) handler ...)
 * Sugar: identical to (handle body ...).
 * Provided for OCaml/algebraic-effects familiarity.
 */
Expr *elab_try_with(Elab *e, const Form *call) {
    /* try-with has the same surface syntax as handle; delegate directly. */
    return elab_handle(e, call);
}

/* (defeffect Name [param1 : T1, param2 : T2, ...] : R)
 * (defeffect ^private Name [param1 : T1, param2 : T2, ...] : R)
 * Declares a new algebraic effect with parameters and a result type.
 * The optional ^private annotation restricts the effect to the defining module.
 */
Expr *elab_defeffect(Elab *e, const Form *call) {
    /* Minimum: (defeffect Name [params...] :ret-type)
     * Or with visibility: (defeffect ^private Name [params...] :ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defeffect requires (defeffect Name [params...] result-type)");
        return NULL;
    }

    /* Phase P19-6: Parse optional ^private visibility annotation */
    bool is_private = false;
    uint32_t name_idx = 1;  /* index of the effect name form */
    if (call->as.list.len >= 5) {
        Form *maybe_private = call->as.list.items[1];
        if (maybe_private->tag == F_SYM
            && maybe_private->as.sym == e->sym_caret_private) {
            is_private = true;
            name_idx = 2;
        }
    }

    /* Parse effect name */
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defeffect: effect name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_f->as.sym;
    
    /* Check if effect already exists */
    if (effect_env_contains(e->effect_env, name)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defeffect: '%s' is already defined", name->name);
        return NULL;
    }
    
    /* Parse parameter list (index shifts by 1 when ^private is present) */
    Form *params_f = call->as.list.items[name_idx + 1];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defeffect: expected parameter list, got %s",
                  form_tag_name(params_f->tag));
        return NULL;
    }
    
    /* Parse parameter list.
     * Accepts:
     *   []          — no params
     *   [x y]       — untyped params (default to TY_INT)
     *   [x :int y :cstr] — typed params (name :type pairs)
     */
    uint8_t raw_n = (uint8_t)params_f->as.list.len;
    /* Count actual params (skip type keyword items) */
    uint8_t n_params = 0;
    for (uint8_t i = 0; i < raw_n; i++) {
        Form *f = params_f->as.list.items[i];
        if (f->tag == F_SYM) n_params++;
        /* F_KEYWORD items are type annotations, not params */
    }
    const Symbol **param_names = arena_alloc(e->arena, n_params * sizeof(const Symbol *));
    TypeKind *param_types = arena_alloc(e->arena, n_params * sizeof(TypeKind));

    {
        uint8_t p = 0;
        for (uint8_t i = 0; i < raw_n; i++) {
            Form *param_f = params_f->as.list.items[i];
            if (param_f->tag == F_KEYWORD || param_f->tag == F_TYPE_ANN) {
                /* Type annotation for preceding param — already handled below */
                continue;
            }
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "defeffect: parameter name must be a symbol");
                return NULL;
            }
            param_names[p] = param_f->as.sym;
            /* Check if the next item is a type keyword or F_TYPE_ANN */
            TypeKind pk = TY_INT;
            if (i + 1 < raw_n) {
                Form *next = params_f->as.list.items[i + 1];
                if (next->tag == F_KEYWORD) {
                    pk = typekind_from_symbol(next->as.sym->name);
                    if (pk == TY_UNKNOWN) pk = TY_INT;
                    i++; /* Consume the type keyword */
                } else if (next->tag == F_TYPE_ANN) {
                    if (next->as.list.len > 0) {
                        Type *ann = type_expr_from_form(e, next->as.list.items[0], NULL, NULL, NULL, 0);
                        if (ann) pk = ann->kind;
                    }
                    i++; /* Consume the type annotation */
                }
            }
            param_types[p] = pk;
            p++;
        }
    }
    
    /* Parse return type.
     * v1 accepts both historical symbol syntax and keyword syntax:
     *   (defeffect E [] int)
     *   (defeffect E [] :int)
     */
    Form *ret_f = call->as.list.items[name_idx + 2];
    if (ret_f->tag != F_SYM && ret_f->tag != F_KEYWORD && ret_f->tag != F_TYPE_ANN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: return type annotation must be a symbol or keyword like :int");
        return NULL;
    }

    TypeKind result_type;
    if (ret_f->tag == F_TYPE_ANN) {
        Type *ann = (ret_f->as.list.len > 0)
            ? type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0)
            : NULL;
        result_type = ann ? ann->kind : TY_UNKNOWN;
    } else {
        result_type = typekind_from_symbol(ret_f->as.sym->name);
    }
    if (result_type == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: unknown return type '%s'", ret_f->as.sym->name);
        return NULL;
    }
    
    /* ET4: Parse optional ^extends ParentName after the return type.
     * stdlib-effect-rows: also parse the standalone ^capability flag, which may
     * appear anywhere among the trailing attributes (it takes no argument). */
    const Symbol *parent_name = NULL;
    bool is_capability = false;
    uint32_t extends_start = name_idx + 3; /* items after (defeffect [^private] Name [params] :ret) */
    for (uint32_t xi = extends_start; xi < (uint32_t)call->as.list.len; xi++) {
        Form *f = call->as.list.items[xi];
        if (f->tag == F_SYM && f->as.sym == e->sym_caret_capability) {
            is_capability = true;
            continue;
        }
        if (f->tag == F_SYM && f->as.sym == e->sym_caret_extends) {
            if (xi + 1 >= (uint32_t)call->as.list.len) {
                diag_emit(DIAG_ERROR, f->span,
                          "defeffect: expected effect name after ^extends");
                return NULL;
            }
            Form *pname_f = call->as.list.items[xi + 1];
            if (pname_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, pname_f->span,
                          "defeffect: expected effect name after ^extends");
                return NULL;
            }
            parent_name = pname_f->as.sym;
            xi++; /* consume the parent name */
        }
    }

    /* Register the effect — pass module visibility info (Phase P19-6) */
    Effect *effect = effect_env_register(e->effect_env, e->arena, name,
                                          param_names, param_types, n_params, result_type,
                                          e->current_module_name, is_private);
    if (!effect) return NULL;
    effect->is_capability = is_capability;

    /* ET4: Resolve parent effect if ^extends was specified */
    if (parent_name) {
        Effect *parent_eff = effect_env_lookup(e->effect_env, parent_name);
        if (!parent_eff) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defeffect: unknown parent effect '%s' in ^extends clause",
                      parent_name->name);
            return NULL;
        }
        effect->parent = parent_eff;
    }

    /* Create the effect definition expression */
    EffectDef *def = arena_alloc(e->arena, sizeof(EffectDef));
    def->name = name;
    def->param_names = param_names;
    def->param_types = param_types;
    def->n_params = n_params;
    def->result_type = result_type;
    def->is_private = is_private;
    def->defining_module_name = e->current_module_name;
    def->parent_name = parent_name;  /* ET4: ^extends parent effect name */
    def->is_capability = is_capability;  /* stdlib-effect-rows */

    Expr *out = expr_new(e->arena, EX_DEFECT, TYPE_NIL, call->span);
    out->as.effect_def_.def = def;
    return out;
}

/* Phase 19 TUR-E0008: Helpers for unhandled-effect scope tracking. */
static bool is_effect_handled(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_handled_effects; i++) {
        if (e->handled_effect_names[i] == name) return true;
    }
    return false;
}

static void push_handled_effect(Elab *e, const Symbol *name) {
    if (e->n_handled_effects >= e->cap_handled_effects) {
        e->cap_handled_effects = e->cap_handled_effects ? e->cap_handled_effects * 2 : 8;
        e->handled_effect_names = (const Symbol **)realloc(
            e->handled_effect_names,
            e->cap_handled_effects * sizeof(const Symbol *));
    }
    e->handled_effect_names[e->n_handled_effects++] = name;
}

/* PR5-3-D: Check if an effect was explicitly imported via :refer [(effect Name)]. */
static bool elab_effect_is_referred(const Elab *e, const Effect *eff) {
    for (uint32_t i = 0; i < e->n_referred_effects; i++) {
        if (e->referred_effects[i] == eff) return true;
    }
    return false;
}

/* (perform (EffectName arg1 arg2 ...))
 * Perform an algebraic effect with arguments.
 */
Expr *elab_perform(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "perform requires (perform (EffectName arg1 ...))");
        return NULL;
    }
    
    /* Parse effect call form: (EffectName arg1 arg2 ...) */
    Form *effect_call_f = call->as.list.items[1];
    if (effect_call_f->tag != F_LIST) {
        diag_emit(DIAG_ERROR, effect_call_f->span,
                  "perform: expected effect call as list, got %s",
                  form_tag_name(effect_call_f->tag));
        return NULL;
    }
    
    if (effect_call_f->as.list.len < 1) {
        diag_emit(DIAG_ERROR, effect_call_f->span,
                  "perform: effect call must have at least an effect name");
        return NULL;
    }
    
    /* Parse effect name */
    Form *name_f = effect_call_f->as.list.items[0];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "perform: effect name must be a symbol");
        return NULL;
    }
    const Symbol *effect_name = name_f->as.sym;
    
    /* Check if effect exists */
    Effect *effect = effect_env_lookup(e->effect_env, effect_name);
    if (!effect) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "perform: unknown effect '%s'", effect_name->name);
        return NULL;
    }

    /* Phase P19-6 / ER5: Enforce effect visibility — private effects cannot be
     * performed from outside their defining module (TUR-E0021). */
    if (!effect->is_exported
        && effect->defining_module_name != NULL
        && effect->defining_module_name != e->current_module_name
        && !elab_effect_is_referred(e, effect)) {
        diag_emit_with_code(DIAG_ERROR, name_f->span,
                            TUR_E0021_PRIVATE_EFFECT,
                            "effect '%s' is private to module '%s'",
                            effect_name->name, effect->defining_module_name->name);
        diag_emit(DIAG_NOTE, name_f->span,
                  "remove ^private from the defeffect declaration to make it public");
        return NULL;
    }

    /* Phase 19 TUR-E0008: At top level (not inside a defn/fn body), every
     * perform must be wrapped by an enclosing handle expression. Inside a
     * defn/fn the caller is expected to provide the handler, so we skip. */
    if (e->fn_body_depth == 0 && !is_effect_handled(e, effect_name)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "[TUR-E0008]: effect '%s' is performed but has no enclosing handler",
                  effect_name->name);
        diag_emit(DIAG_NOTE, name_f->span,
                  "wrap the expression with a (handle ...) block or a handler macro");
        return NULL;
    }

    /* Parse arguments */
    uint8_t n_args = effect_call_f->as.list.len - 1;
    Expr **args = arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint8_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, effect_call_f->as.list.items[i + 1]);
        if (!args[i]) return NULL;
    }
    
    /* Create the perform expression */
    PerformExpr *perform = arena_alloc(e->arena, sizeof(PerformExpr));
    perform->effect_name = effect_name;
    perform->args = args;
    perform->n_args = n_args;
    
    /* The return type of perform is the result type of the effect */
    Type result_type = type_from_kind(effect->constructor->result_type);
    
    Expr *out = expr_new(e->arena, EX_PERFORM, result_type, call->span);
    out->as.perform_.perform = perform;
    return out;
}

/* (handle expr case1 case2 ...)
 * Handle algebraic effects with cases.
 * Each case: (EffectName [param1 param2 ...] k) body ...
 */
Expr *elab_handle(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "handle requires (handle expr case1 case2 ...)");
        return NULL;
    }
    
    /* Phase 19 TUR-E0008: Pre-scan case headers to push effect names onto the
     * handled-effects scope BEFORE elaborating the body.  This ensures that
     * perform calls inside the body see the enclosing handle as a handler,
     * without changing the case elaboration logic below. */
    uint32_t n_case_forms_pre = call->as.list.len >= 2 ? call->as.list.len - 2 : 0;
    uint8_t n_cases_pre = (uint8_t)((n_case_forms_pre / 2));
    uint32_t saved_n_handled = e->n_handled_effects;
    for (uint8_t i = 0; i < n_cases_pre; i++) {
        Form *case_f = call->as.list.items[2 + (i * 2)];
        if (case_f->tag == F_LIST && case_f->as.list.len >= 1 &&
            case_f->as.list.items[0]->tag == F_SYM) {
            push_handled_effect(e, case_f->as.list.items[0]->as.sym);
        }
    }

    /* Parse the body to be handled */
    Expr *body = elab_form(e, call->as.list.items[1]);

    /* Pop the pre-scanned handlers after body elaboration */
    e->n_handled_effects = saved_n_handled;

    if (!body) return NULL;
    
    /* Parse handle cases.
     * Surface syntax follows effects-plan.md §4.3:
     *   (handle expr
     *     (Effect [params...] k) body
     *     ...)
     * so cases are provided as header/body pairs.
     */
    uint32_t n_case_forms = call->as.list.len - 2;
    if ((n_case_forms & 1U) != 0U) {
        diag_emit(DIAG_ERROR, call->span,
                  "handle expects pairs of (case-header body)");
        return NULL;
    }

    uint8_t n_cases = (uint8_t)(n_case_forms / 2);
    HandleCase *cases = arena_alloc(e->arena, n_cases * sizeof(HandleCase));
    
    for (uint8_t i = 0; i < n_cases; i++) {
        Form *case_f = call->as.list.items[2 + (i * 2)];
        if (case_f->tag != F_LIST) {
            diag_emit(DIAG_ERROR, case_f->span,
                      "handle: expected case as list, got %s",
                      form_tag_name(case_f->tag));
            return NULL;
        }
        
        /* Case header format: (EffectName [params...] k)
         * LC0: Optional annotation before k:
         *   (EffectName [params...] ^linear k)    -- CK_LINEAR: exactly-once
         *   (EffectName [params...] ^multishot k) -- CK_MULTISHOT: safe multi-shot */
        uint32_t hdr_len = case_f->as.list.len;
        if (hdr_len != 3 && hdr_len != 4) {
            diag_emit(DIAG_ERROR, case_f->span,
                      "handle case header requires (EffectName [params...] k) "
                      "or (EffectName [params...] ^annotation k)");
            return NULL;
        }

        /* Parse effect name */
        Form *name_f = case_f->as.list.items[0];
        if (name_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "handle case: effect name must be a symbol");
            return NULL;
        }
        cases[i].effect_name = name_f->as.sym;

        /* Parse parameter list */
        Form *params_f = case_f->as.list.items[1];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            diag_emit(DIAG_ERROR, params_f->span,
                      "handle case: expected parameter list, got %s",
                      form_tag_name(params_f->tag));
            return NULL;
        }

        cases[i].n_params = params_f->as.list.len;
        cases[i].param_names = arena_alloc(e->arena, cases[i].n_params * sizeof(const Symbol *));
        cases[i].param_bindings = arena_alloc(e->arena, cases[i].n_params * sizeof(Binding *));
        for (uint8_t j = 0; j < cases[i].n_params; j++) {
            Form *param_f = params_f->as.list.items[j];
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "handle case: parameter name must be a symbol");
                return NULL;
            }
            cases[i].param_names[j] = param_f->as.sym;
        }

        /* LC0: Parse optional cont_kind annotation and continuation name.
         * 3 items: (Effect [params...] k)             -> CK_UNIQUE (default)
         * 4 items: (Effect [params...] ^annotation k) -> CK_LINEAR or CK_COPY */
        cases[i].cont_kind = CK_UNIQUE;
        Form *k_f;
        if (hdr_len == 4) {
            Form *ann_f = case_f->as.list.items[2];
            k_f = case_f->as.list.items[3];
            if (ann_f->tag == F_SYM && ann_f->as.sym == e->sym_caret_linear) {
                cases[i].cont_kind = CK_LINEAR;
            } else if (ann_f->tag == F_SYM &&
                       ann_f->as.sym == e->sym_caret_multishot) {
                cases[i].cont_kind = CK_MULTISHOT;
            } else {
                diag_emit(DIAG_ERROR, ann_f->span,
                          "handle case: expected ^linear or ^multishot "
                          "before continuation name, got '%s'",
                          ann_f->tag == F_SYM ? ann_f->as.sym->name : "<non-symbol>");
                return NULL;
            }
        } else {
            k_f = case_f->as.list.items[2];
        }

        if (k_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, k_f->span,
                      "handle case: continuation name must be a symbol");
            return NULL;
        }
        cases[i].k_name = k_f->as.sym;
        
        /* Look up effect definition to get param types */
        Effect *eff = effect_env_lookup(e->effect_env, cases[i].effect_name);

        /* Phase P19-6 / ER5: Enforce effect visibility in handler declarations
         * (TUR-E0021). */
        if (eff && !eff->is_exported
            && eff->defining_module_name != NULL
            && eff->defining_module_name != e->current_module_name
            && !elab_effect_is_referred(e, eff)) {
            diag_emit_with_code(DIAG_ERROR, name_f->span,
                                TUR_E0021_PRIVATE_EFFECT,
                                "effect '%s' is private to module '%s'",
                                cases[i].effect_name->name, eff->defining_module_name->name);
            diag_emit(DIAG_NOTE, name_f->span,
                      "remove ^private from the defeffect declaration to make it public");
            return NULL;
        }

        /* Create a handler scope with bindings for params and k */
        Scope handler_scope;
        scope_init(&handler_scope, e->scope);
        Scope *saved_scope = e->scope;
        e->scope = &handler_scope;
        
        /* Create bindings for each parameter */
        for (uint8_t j = 0; j < cases[i].n_params; j++) {
            /* Use the effect's declared param type if available, else TY_INT */
            TypeKind pk = (eff && j < eff->constructor->n_params)
                ? eff->constructor->param_types[j] : TY_INT;
            Type ptype = type_from_kind(pk);
            Binding *pb = binding_new(e, cases[i].param_names[j], ptype, false, false, params_f->span);
            cases[i].param_bindings[j] = pb;
            scope_add(&handler_scope, pb);
        }
        
        /* LC0: Create binding for k; apply cont_kind annotation.
         * is_continuation lets the async-escape check (T25) detect continuation capture. */
        Binding *kb = binding_new(e, cases[i].k_name, TYPE_INT, false, false, k_f->span);
        switch (cases[i].cont_kind) {
        case CK_LINEAR:
            /* ^linear k: exactly one resume/discontinue required.
             * LC2: is_affine + is_relevant wire k into the ST0-ST1 usage-tracking
             * machinery so that usage_state is maintained via elab_var. */
            kb->is_linear = true;
            kb->type.copy_kind = CK_LINEAR;
            kb->is_affine    = true;  /* no duplication */
            kb->is_relevant  = true;  /* must be consumed */
            break;
        case CK_MULTISHOT:
            /* ^multishot k: MS1: safe multi-shot via snapshot semantics; no ownership warning. */
            kb->type.copy_kind = CK_MULTISHOT;
            break;
        default: /* CK_UNIQUE */
            /* Default: affine (at most once).
             * LC2: is_affine wires k into the ST0-ST1 usage-tracking machinery. */
            kb->type.copy_kind = CK_MOVE;
            kb->is_affine = true;  /* no duplication */
            break;
        }
        kb->is_continuation = true;
        cases[i].k_binding = kb;
        scope_add(&handler_scope, kb);
        
        /* Parse handler body inside the handler scope */
        Form *body_f = call->as.list.items[3 + (i * 2)];
        cases[i].body = elab_form(e, body_f);

        /* MS2: For ^multishot handlers, check all captured free variables.
         * CK_UNIQUE (move-only) and CK_LINEAR captures are forbidden because
         * the handler body may run multiple times -- each resume takes a
         * snapshot of k, so the body executes N times and must not consume a
         * linear or unique value on each execution. */
        if (cases[i].cont_kind == CK_MULTISHOT && cases[i].body) {
            /* Build the "local params" list: effect params + k (not free vars) */
            uint8_t n_hparams = (uint8_t)(cases[i].n_params + 1);
            Binding **hparams = arena_alloc(e->arena, n_hparams * sizeof(Binding *));
            for (uint8_t j = 0; j < cases[i].n_params; j++)
                hparams[j] = cases[i].param_bindings[j];
            hparams[cases[i].n_params] = kb;
            uint32_t n_caps = 0;
            Binding **caps = collect_free_vars(cases[i].body, hparams, n_hparams, NULL, 0, &n_caps);
            for (uint32_t ci = 0; ci < n_caps; ci++) {
                CopyKind ck = caps[ci]->type.copy_kind;
                if (ck == CK_UNIQUE || ck == CK_LINEAR) {
                    diag_emit_with_code(DIAG_ERROR, cases[i].body->span,
                        TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,
                        "^multishot handler captures '%s' which is %s -- "
                        "cannot be safely captured in a multi-shot handler",
                        caps[ci]->name->name,
                        ck == CK_UNIQUE ? "unique (move-only)" : "linear");
                }
            }
            free(caps);
        }

        /* LC1/LC2: ^linear k must be consumed (resumed or discontinued) in the handler body.
         * Check immediately after body elaboration while the binding is still in scope.
         * Uses is_linear_consumed (flow-sensitive via elab_if linear-state machinery). */
        if (cases[i].cont_kind == CK_LINEAR && kb && !kb->is_linear_consumed) {
            diag_emit_with_code(DIAG_ERROR, k_f->span,
                TUR_E0100_LINEAR_DROPPED,
                "linear continuation '%s' was not resumed or discontinued",
                kb->name->name);
        }

        /* Restore outer scope */
        e->scope = saved_scope;
        scope_free(&handler_scope);

        if (!cases[i].body) return NULL;

        /* ET3-C: Check that the handler clause result type matches the body type.
         * Skip the check when either is TY_UNKNOWN or TY_NEVER, or when the clause
         * body ends in a resume expression (resume returns the value type passed to
         * the continuation, not the overall handle result type). */
        {
            /* Helper: check if an expression ends in a resume.
             * Handles bare (resume ...) and (do ... (resume ...)) patterns. */
            const Expr *cb = cases[i].body;
            bool ends_in_resume = (cb->kind == EX_RESUME);
            if (!ends_in_resume && cb->kind == EX_DO && cb->as.do_.n > 0) {
                const Expr *last = cb->as.do_.items[cb->as.do_.n - 1];
                ends_in_resume = (last->kind == EX_RESUME);
            }

            if (!ends_in_resume && i == 0 && body != NULL
                && cases[i].body->type.kind != TY_UNKNOWN
                && cases[i].body->type.kind != TY_NEVER
                && body->type.kind != TY_UNKNOWN
                && body->type.kind != TY_NEVER
                && cases[i].body->type.kind != body->type.kind) {
                diag_emit_with_code(DIAG_ERROR, body_f->span,
                    TUR_E0252_HANDLER_RESULT_MISMATCH,
                    "handler clause result type '%s' does not match handle expression type '%s'",
                    type_name(cases[i].body->type), type_name(body->type));
            } else if (!ends_in_resume && i > 0 && cases[0].body != NULL) {
                /* Also check if case 0's body ends in resume before comparing */
                const Expr *cb0 = cases[0].body;
                bool c0_ends_resume = (cb0->kind == EX_RESUME);
                if (!c0_ends_resume && cb0->kind == EX_DO && cb0->as.do_.n > 0) {
                    const Expr *last0 = cb0->as.do_.items[cb0->as.do_.n - 1];
                    c0_ends_resume = (last0->kind == EX_RESUME);
                }
                if (!c0_ends_resume
                    && cases[i].body->type.kind != TY_UNKNOWN
                    && cases[i].body->type.kind != TY_NEVER
                    && cases[0].body->type.kind != TY_UNKNOWN
                    && cases[0].body->type.kind != TY_NEVER
                    && cases[i].body->type.kind != cases[0].body->type.kind) {
                    diag_emit_with_code(DIAG_ERROR, body_f->span,
                        TUR_E0252_HANDLER_RESULT_MISMATCH,
                        "handler clause result type '%s' does not match other clauses' type '%s'",
                        type_name(cases[i].body->type), type_name(cases[0].body->type));
                }
            }
        }
    }

    /* Create the handle expression */
    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    handle->body = body;
    handle->cases = cases;
    handle->n_cases = n_cases;
    handle->is_unsafe_marker = false;

    /* The return type of handle is the same as the body's type */
    Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
    out->as.handle_.handle = handle;
    return out;
}

/* FH2: (handler (E [params] k) body) -- a single-effect handler value literal.
 * Parses one handle case (identical surface syntax to a (handle ...) case) and
 * builds a HandleExpr with body == NULL.  The result has type TY_HANDLER
 * carrying the effect name, value/result kinds, and continuation discipline.
 */
Expr *elab_handler_lit(Elab *e, const Form *call) {
    if (!g_effect_types_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "'handler' value literal requires -Xeffect-types");
        return NULL;
    }
    /* (handler (E [params] k) body) -- head + header + body = 3 items. */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(handler (E [params] k) body) requires a case header and a body");
        return NULL;
    }

    HandleCase *cases = arena_alloc(e->arena, sizeof(HandleCase));
    memset(cases, 0, sizeof(HandleCase));

    Form *case_f = call->as.list.items[1];
    if (case_f->tag != F_LIST) {
        diag_emit(DIAG_ERROR, case_f->span,
                  "handler literal: expected case header (E [params] k), got %s",
                  form_tag_name(case_f->tag));
        return NULL;
    }
    uint32_t hdr_len = case_f->as.list.len;
    if (hdr_len != 3 && hdr_len != 4) {
        diag_emit(DIAG_ERROR, case_f->span,
                  "handler literal header requires (EffectName [params...] k) "
                  "or (EffectName [params...] ^annotation k)");
        return NULL;
    }

    Form *name_f = case_f->as.list.items[0];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "handler literal: effect name must be a symbol");
        return NULL;
    }
    cases[0].effect_name = name_f->as.sym;

    Form *params_f = case_f->as.list.items[1];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "handler literal: expected parameter list, got %s",
                  form_tag_name(params_f->tag));
        return NULL;
    }
    cases[0].n_params = params_f->as.list.len;
    cases[0].param_names = arena_alloc(e->arena, cases[0].n_params * sizeof(const Symbol *));
    cases[0].param_bindings = arena_alloc(e->arena, cases[0].n_params * sizeof(Binding *));
    for (uint8_t j = 0; j < cases[0].n_params; j++) {
        Form *param_f = params_f->as.list.items[j];
        if (param_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, param_f->span, "handler literal: parameter name must be a symbol");
            return NULL;
        }
        cases[0].param_names[j] = param_f->as.sym;
    }

    /* Optional cont_kind annotation + continuation name. */
    cases[0].cont_kind = CK_UNIQUE;
    Form *k_f;
    if (hdr_len == 4) {
        Form *ann_f = case_f->as.list.items[2];
        k_f = case_f->as.list.items[3];
        if (ann_f->tag == F_SYM && ann_f->as.sym == e->sym_caret_linear) {
            cases[0].cont_kind = CK_LINEAR;
        } else if (ann_f->tag == F_SYM && ann_f->as.sym == e->sym_caret_multishot) {
            cases[0].cont_kind = CK_MULTISHOT;
        } else {
            diag_emit(DIAG_ERROR, ann_f->span,
                      "handler literal: expected ^linear or ^multishot "
                      "before continuation name");
            return NULL;
        }
    } else {
        k_f = case_f->as.list.items[2];
    }
    if (k_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, k_f->span, "handler literal: continuation name must be a symbol");
        return NULL;
    }
    cases[0].k_name = k_f->as.sym;

    Effect *eff = effect_env_lookup(e->effect_env, cases[0].effect_name);
    if (eff && !eff->is_exported
        && eff->defining_module_name != NULL
        && eff->defining_module_name != e->current_module_name
        && !elab_effect_is_referred(e, eff)) {
        diag_emit_with_code(DIAG_ERROR, name_f->span, TUR_E0021_PRIVATE_EFFECT,
                            "effect '%s' is private to module '%s'",
                            cases[0].effect_name->name, eff->defining_module_name->name);
        return NULL;
    }

    /* Handler scope: bind params + k, then elaborate the body. */
    Scope handler_scope;
    scope_init(&handler_scope, e->scope);
    Scope *saved_scope = e->scope;
    e->scope = &handler_scope;

    for (uint8_t j = 0; j < cases[0].n_params; j++) {
        TypeKind pk = (eff && j < eff->constructor->n_params)
            ? eff->constructor->param_types[j] : TY_INT;
        Type ptype = type_from_kind(pk);
        Binding *pb = binding_new(e, cases[0].param_names[j], ptype, false, false, params_f->span);
        cases[0].param_bindings[j] = pb;
        scope_add(&handler_scope, pb);
    }

    Binding *kb = binding_new(e, cases[0].k_name, TYPE_INT, false, false, k_f->span);
    switch (cases[0].cont_kind) {
    case CK_LINEAR:
        kb->is_linear = true; kb->type.copy_kind = CK_LINEAR;
        kb->is_affine = true; kb->is_relevant = true;
        break;
    case CK_MULTISHOT:
        kb->type.copy_kind = CK_MULTISHOT;
        break;
    default:
        kb->type.copy_kind = CK_MOVE; kb->is_affine = true;
        break;
    }
    kb->is_continuation = true;
    cases[0].k_binding = kb;
    scope_add(&handler_scope, kb);

    Form *body_f = call->as.list.items[2];
    cases[0].body = elab_form(e, body_f);

    /* MS2: a ^multishot handler literal may re-run its body per resume, so it
     * must not capture a unique (move-only) or linear free variable (TUR-E0500).
     * Mirrors the inline-handle check in elab_handle. */
    if (cases[0].cont_kind == CK_MULTISHOT && cases[0].body) {
        uint8_t n_hparams = (uint8_t)(cases[0].n_params + 1);
        Binding **hparams = arena_alloc(e->arena, n_hparams * sizeof(Binding *));
        for (uint8_t j = 0; j < cases[0].n_params; j++)
            hparams[j] = cases[0].param_bindings[j];
        hparams[cases[0].n_params] = kb;
        uint32_t n_caps = 0;
        Binding **caps = collect_free_vars(cases[0].body, hparams, n_hparams, NULL, 0, &n_caps);
        for (uint32_t ci = 0; ci < n_caps; ci++) {
            CopyKind ck = caps[ci]->type.copy_kind;
            if (ck == CK_UNIQUE || ck == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, cases[0].body->span,
                    TUR_E0500_MULTISHOT_UNIQUE_CAPTURE,
                    "^multishot handler captures '%s' which is %s -- "
                    "cannot be safely captured in a multi-shot handler",
                    caps[ci]->name->name,
                    ck == CK_UNIQUE ? "unique (move-only)" : "linear");
            }
        }
        free(caps);
    }

    if (cases[0].cont_kind == CK_LINEAR && kb && !kb->is_linear_consumed) {
        diag_emit_with_code(DIAG_ERROR, k_f->span, TUR_E0100_LINEAR_DROPPED,
            "linear continuation '%s' was not resumed or discontinued", kb->name->name);
    }

    e->scope = saved_scope;
    scope_free(&handler_scope);
    if (!cases[0].body) return NULL;

    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    handle->body = NULL;          /* literal: detached from any body (FH design) */
    handle->cases = cases;
    handle->n_cases = 1;
    handle->is_unsafe_marker = false;

    /* Build the TY_HANDLER value type. */
    Type htype;
    memset(&htype, 0, sizeof(Type));
    htype.kind = TY_HANDLER;
    htype.copy_kind = CK_COPY;
    htype.hkt_kind = KIND_STAR;
    htype.as.handler_.effect_name = cases[0].effect_name->name;
    {
        /* FH4.1: single-element handled row (unresolved name-set). */
        const Symbol *one[1] = { cases[0].effect_name };
        htype.as.handler_.handled_row = effect_row_unresolved(e->arena, one, 1);
    }
    htype.as.handler_.value_kind  = (eff && eff->constructor->n_params > 0)
        ? eff->constructor->param_types[0] : TY_INT;
    htype.as.handler_.result_kind = cases[0].body->type.kind;
    htype.as.handler_.cont_kind   = cases[0].cont_kind;

    Expr *out = expr_new(e->arena, EX_HANDLER_LIT, htype, call->span);
    out->as.handler_lit_.handle = handle;
    return out;
}

/* FH3: (with-handler hv body) -- apply a handler value to a body.
 * The body runs with hv's dispatch table installed; the result type is the
 * body's type (the answer type T).  hv must be a TY_HANDLER value.
 */
Expr *elab_with_handler(Elab *e, const Form *call) {
    if (!g_effect_types_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "(with-handler hv body) value application requires -Xeffect-types");
        return NULL;
    }
    /* hv first so we know which effects to mark handled while elaborating body. */
    Expr *hv = elab_form(e, call->as.list.items[1]);
    if (!hv) return NULL;
    if (hv->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "(with-handler): first argument must be a handler value, got '%s'",
                  type_name(hv->type));
        return NULL;
    }

    /* Push the handler's effect(s) onto the handled-effects scope so perform
     * calls in the body see them as handled (mirrors elab_handle's pre-scan). */
    uint32_t saved_n_handled = e->n_handled_effects;
    if (hv->type.as.handler_.effect_name != NULL) {
        /* Intern the effect name back to a Symbol for push_handled_effect. */
        const Symbol *eff_sym = intern_cstr(e->st, hv->type.as.handler_.effect_name);
        push_handled_effect(e, eff_sym);
    }
    Expr *body = elab_form(e, call->as.list.items[2]);
    e->n_handled_effects = saved_n_handled;
    if (!body) return NULL;

    Expr *out = expr_new(e->arena, EX_WITH_HANDLER, body->type, call->span);
    out->as.with_handler_.handler = hv;
    out->as.with_handler_.body = body;
    return out;
}

/* ET3-E / FH5: (compose-handlers h1 h2)
 * Compose two handler values that must handle different effects (TUR_E0251 on
 * overlap).  Produces a TY_HANDLER value whose runtime table is the
 * concatenation of h1's and h2's tables (h1 outer, per FH0.1).
 */
Expr *elab_compose_handlers(Elab *e, const Form *call) {
    if (!g_effect_types_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "'compose-handlers' requires -Xeffect-types");
        return NULL;
    }
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(compose-handlers h1 h2) requires exactly two arguments");
        return NULL;
    }
    Expr *h1 = elab_form(e, call->as.list.items[1]);
    if (!h1) return NULL;
    Expr *h2 = elab_form(e, call->as.list.items[2]);
    if (!h2) return NULL;

    /* ET3: Both arguments should have TY_HANDLER type */
    if (h1->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "(compose-handlers): first argument must be a handler value, got '%s'",
                  type_name(h1->type));
        return NULL;
    }
    if (h2->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[2]->span,
                  "(compose-handlers): second argument must be a handler value, got '%s'",
                  type_name(h2->type));
        return NULL;
    }

    /* FH4.1: handlers must handle disjoint effect *sets* (TUR-E0251).  Compare
     * the handled rows by name so the check also covers composed (multi-effect)
     * handlers, not just the single effect_name. */
    {
        const Symbol *n1[32]; uint8_t c1 = 0;
        const Symbol *n2[32]; uint8_t c2 = 0;
        effect_row_collect_names(h1->type.as.handler_.handled_row, n1, &c1, 32);
        effect_row_collect_names(h2->type.as.handler_.handled_row, n2, &c2, 32);
        for (uint8_t i = 0; i < c1; i++)
            for (uint8_t j = 0; j < c2; j++)
                if (n1[i] == n2[j]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0251_HANDLER_OVERLAP,
                        "composed handlers both handle effect '%s'; overlapping "
                        "effects are not allowed", n1[i]->name);
                    return NULL;
                }
    }
    /* Fallback for legacy types without a handled_row: compare effect_name. */
    if ((!h1->type.as.handler_.handled_row || !h2->type.as.handler_.handled_row)
        && h1->type.as.handler_.effect_name != NULL
        && h2->type.as.handler_.effect_name != NULL
        && h1->type.as.handler_.effect_name == h2->type.as.handler_.effect_name) {
        diag_emit_with_code(DIAG_ERROR, call->span,
            TUR_E0251_HANDLER_OVERLAP,
            "composed handlers both handle effect '%s'; overlapping effects are not allowed",
            h1->type.as.handler_.effect_name);
        return NULL;
    }

    /* FH5: runtime composition is implemented -- the CF3 TUR-E0704 gate is
     * removed.  The static checks above (TUR-E0251 overlap, handler-value
     * typing) still fire first.  Composition lowers to table concatenation
     * (emit_effects_compose_handlers); see docs/first-class-handlers-semantics.md. */

    /* FH0.3: composed handlers must agree on the answer type T.  In v1 a
     * detached handler literal does not carry the answer type -- its
     * result_kind records the case body's own type, which for a resume-
     * terminated body is the resumed value's type, not T.  Enforcing strict T
     * equality from that field would misfire, so the check is deferred to the
     * application site (the body type of with-handler is the real T).  The
     * disjoint-effect rule (TUR-E0251) above is still enforced. */

    /* FH5: produce a composed handler value.  Its runtime table is the
     * concatenation of h1's and h2's tables (h1 outer, per FH0.1).  The
     * value type is TY_HANDLER; the single effect_name field is meaningful
     * only for single-effect handlers, so a composed value sets it to NULL
     * (the row is carried implicitly by the two children at codegen time). */
    Type ctype;
    memset(&ctype, 0, sizeof(Type));
    ctype.kind = TY_HANDLER;
    ctype.copy_kind = CK_COPY;
    ctype.hkt_kind = KIND_STAR;
    ctype.as.handler_.effect_name = NULL;   /* composed: multi-effect row */
    /* FH4.1: the composed handled set is the union of the two rows. */
    ctype.as.handler_.handled_row = effect_row_union(e->arena,
        h1->type.as.handler_.handled_row, h2->type.as.handler_.handled_row);
    ctype.as.handler_.value_kind  = TY_UNKNOWN;
    ctype.as.handler_.result_kind = (h1->type.as.handler_.result_kind != TY_UNKNOWN)
        ? h1->type.as.handler_.result_kind : h2->type.as.handler_.result_kind;
    ctype.as.handler_.cont_kind   = CK_COPY;

    Expr *out = expr_new(e->arena, EX_COMPOSE_HANDLERS, ctype, call->span);
    out->as.compose_handlers_.h1 = h1;
    out->as.compose_handlers_.h2 = h2;
    return out;
}

/* LC1/LC2: Pre-check a continuation binding for double-use before elaborating k.
 * Uses is_linear_consumed (CK_LINEAR) and is_moved (CK_UNIQUE) — both are
 * snapshot/restored by elab_if, making them flow-sensitive across branches.
 * Emits the appropriate error and returns true if the use should be rejected. */
bool cont_check_double_use(Elab *e, const Form *k_form) {
    if (k_form->tag != F_SYM) return false;
    Binding *kb = scope_lookup(e->scope, k_form->as.sym);
    if (!kb || !kb->is_continuation) return false;
    /* CK_COPY / CK_MULTISHOT: multi-shot allowed -- no double-use restriction. */
    if (kb->type.copy_kind == CK_COPY || kb->type.copy_kind == CK_MULTISHOT) return false;

    if (kb->type.copy_kind == CK_LINEAR) {
        if (kb->is_linear_consumed) {
            diag_emit_with_code(DIAG_ERROR, k_form->span,
                TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                "linear continuation '%s' has already been resumed or discontinued",
                kb->name->name);
            return true;
        }
    } else {
        /* CK_UNIQUE: is_moved is flow-sensitive (elab_if restores it between branches). */
        if (kb->is_moved) {
            diag_emit_with_code(DIAG_ERROR, k_form->span,
                TUR_E0201_UNIQUE_COPY,
                "continuation '%s' has already been resumed or discontinued",
                kb->name->name);
            return true;
        }
    }
    return false;
}

/* LC1/LC2: Mark a continuation binding consumed after resume/discontinue.
 * Sets is_linear_consumed (CK_LINEAR), is_moved (CK_UNIQUE) — both flow-sensitive.
 * Also sets usage_state for ST0-ST1 interop. */
static void cont_mark_consumed(Expr *k) {
    if (!k || k->kind != EX_VAR) return;
    Binding *kb = k->as.var.binding;
    if (!kb->is_continuation) return;
    /* LC2: also set usage_state for ST0-ST1 informational tracking. */
    kb->usage_state = USAGE_USED_ONCE;
    if (kb->type.copy_kind == CK_LINEAR) {
        kb->is_linear_consumed = true;
    } else if (type_is_move(kb->type)) {
        /* CK_UNIQUE: mark moved so elab_if's move-state machinery stays consistent. */
        binding_mark_moved(kb, k->span);
    }
    /* CK_COPY / CK_MULTISHOT: usage_state updated but no ownership enforcement. */
}

/* (resume k value)
 * Resume a captured continuation with a value.
 */
Expr *elab_resume(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(resume k value) requires exactly two arguments");
        return NULL;
    }

    /* LC1: Check for double-use before elaborating k (would emit E0005 otherwise). */
    if (cont_check_double_use(e, call->as.list.items[1])) return NULL;

    Expr *k = elab_form(e, call->as.list.items[1]);
    if (!k) return NULL;

    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;

    /* LC1: Mark continuation consumed per its cont_kind. */
    cont_mark_consumed(k);

    /* MS2: Resuming a ^multishot continuation inside atomically is unsafe --
     * the handler body may be re-executed by STM retry, causing the continuation
     * to be resumed more than once in unexpected ways. */
    if (k->kind == EX_VAR && k->as.var.binding &&
        k->as.var.binding->type.copy_kind == CK_MULTISHOT &&
        elab_in_atomically) {
        diag_emit_with_code(DIAG_ERROR, call->span,
            TUR_E0502_MULTISHOT_RESUME_IN_ATOMIC,
            "cannot resume a '^multishot' continuation inside 'atomically' -- "
            "STM retry may cause the continuation to be resumed multiple times unexpectedly");
        return NULL;
    }

    ResumeExpr *resume = arena_alloc(e->arena, sizeof(ResumeExpr));
    resume->k = k;
    resume->value = value;

    Expr *out = expr_new(e->arena, EX_RESUME, value->type, call->span);
    out->as.resume_.resume = resume;
    return out;
}

/* (discontinue k exception)
 * Discontinue a captured continuation with an exception.
 */
Expr *elab_discontinue(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(discontinue k exception) requires exactly two arguments");
        return NULL;
    }
    
    /* LC1: Check for double-use before elaborating k. */
    if (cont_check_double_use(e, call->as.list.items[1])) return NULL;

    Expr *k = elab_form(e, call->as.list.items[1]);
    if (!k) return NULL;

    Expr *exception = elab_form(e, call->as.list.items[2]);
    if (!exception) return NULL;

    /* LC1: Mark continuation consumed per its cont_kind. */
    cont_mark_consumed(k);

    DiscontinueExpr *discontinue = arena_alloc(e->arena, sizeof(DiscontinueExpr));
    discontinue->k = k;
    discontinue->exception = exception;

    Expr *out = expr_new(e->arena, EX_DISCONTINUE, TYPE_NIL, call->span);
    out->as.discontinue_.discontinue = discontinue;
    return out;
}

/* Phase 19: (cont? k) — check if a Phase 18 continuation has not been consumed.
 * For Phase 19 algebraic-effect continuations (which are CK_MOVE int64_t dummies),
 * the static one-shot check already catches double-resume at compile time.
 * For Phase 18 tur_cont* continuations, this checks tur_cont_consumed at runtime.
 * Returns: bool
 */
Expr *elab_cont_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cont? k) requires exactly one argument");
        return NULL;
    }
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Accepts TY_CONT (Phase 18 tur_cont*) or TY_INT (Phase 19 CK_MOVE dummy k) */
    if (inner->type.kind != TY_CONT && inner->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cont? k) requires a continuation argument");
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_CONT_PRED, TYPE_BOOL, call->span);
    out->as.cont_pred_.expr = inner;
    return out;
}

/* CC4.2: default an unannotated continuation parameter to the escape flavor.
 * (call/cc f) / (escape f) hand `f` an undelimited continuation captured against
 * the implicit root prompt; resuming it lowers to tur_escape_resume.  When `f`
 * is written inline as (fn [k] ...) with a single *unannotated* trailing param,
 * rewrite the param list to [k :escape-cont] so the (k v) application sugar
 * dispatches to the escape resume runtime instead of defaulting `k` to :int.
 * (This is the local exception to the :int-by-default lambda-param rule, scoped
 * to the call/cc / escape receiver.)  Caret markers (^linear, ^unique, ...) are
 * '^'-prefixed and always precede their param, so an unannotated trailing param
 * is simply a non-'^' F_SYM in the last vec slot -- ^linear k still gets the
 * annotation appended (preserving OQ3's opt-in linearity). */
static Form *callcc_default_cont_param(Elab *e, Form *f_form) {
    if (!f_form || f_form->tag != F_LIST || f_form->as.list.len < 2) return f_form;
    const Form *head = f_form->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_fn) return f_form;
    Form *params = f_form->as.list.items[1];
    if (params->tag != F_VEC || params->as.list.len == 0) return f_form;
    const Form *last = params->as.list.items[params->as.list.len - 1];
    /* Already annotated (keyword / `: T` / type-list) -- leave it alone. */
    if (last->tag != F_SYM || last->as.sym->name[0] == '^') return f_form;

    /* Rebuild the param vec with :escape-cont appended. */
    uint32_t pn = params->as.list.len;
    Form **new_params = (Form **)arena_alloc(e->arena, sizeof(Form *) * (pn + 1));
    for (uint32_t i = 0; i < pn; i++) new_params[i] = params->as.list.items[i];
    new_params[pn] = form_keyword(e->arena, last->span,
                                  intern_cstr(e->st, "escape-cont"));
    Form *new_vec = form_vec(e->arena, params->span, new_params, pn + 1);

    /* Rebuild the fn form with the rewritten param vec. */
    uint32_t fn_n = f_form->as.list.len;
    Form **new_fn = (Form **)arena_alloc(e->arena, sizeof(Form *) * fn_n);
    for (uint32_t i = 0; i < fn_n; i++) new_fn[i] = f_form->as.list.items[i];
    new_fn[1] = new_vec;
    return form_list(e->arena, f_form->span, new_fn, fn_n);
}

/* call-cc-completion: build the shared EX_CALLCC node for (call/cc f) and
 * (escape f).  The result type is f's codomain (CF2 typing): calling f with the
 * captured continuation yields a value of f's return type. */
static Expr *callcc_node(Elab *e, const Form *call, bool is_escape) {
    Form *f_form = callcc_default_cont_param(e, call->as.list.items[1]);
    Expr *f_expr = elab_form(e, f_form);
    if (!f_expr) return NULL;

    /* f's codomain becomes the call/cc result type; fall back to int. */
    Type result_type = TYPE_INT;
    if (f_expr->type.kind == TY_FN) {
        result_type = f_expr->type.as.fn.result_full_type
            ? *f_expr->type.as.fn.result_full_type
            : type_from_kind(f_expr->type.as.fn.result_kind);
    } else if (f_expr->kind == EX_VAR && f_expr->as.var.binding
               && f_expr->as.var.binding->type.kind == TY_FN) {
        result_type = type_from_kind(
            f_expr->as.var.binding->type.as.fn.result_kind);
    }

    Expr *out = expr_new(e->arena, EX_CALLCC, result_type, call->span);
    out->as.callcc_.fn = f_expr;
    out->as.callcc_.is_escape = is_escape;
    return out;
}

/* call-cc-completion (CC1): (call/cc f) - undelimited continuation capture
 * against the implicit program-wide prompt.  f receives a real continuation
 * handle; invoking it (tur_escape_resume) returns its argument at the call/cc
 * site (one-shot, upward).  No enclosing reset required; unbounded depth. */
Expr *elab_call_cc(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(call/cc f) requires exactly one argument");
        return NULL;
    }
    /* call-cc-completion (CC5): the -Xcallcc gate (and TUR-E0700) are retired.
     * The CPS substrate (cps-transform-plan CPS5.3/CPS6) supplies the implicit
     * program-wide prompt and unbounded capture, so call/cc is now sound and
     * ungated.  -Xcallcc is accepted as a deprecated no-op for one release. */
    /* call-cc-completion (CC1): real undelimited capture against the implicit
     * program-wide prompt.  Build an EX_CALLCC node; codegen establishes a
     * setjmp landing at the call/cc site and hands f the landing as the
     * continuation handle.  f receives it (as int64_t); invoking it via
     * (tur_escape_resume k v) returns v at this site (one-shot, upward escape),
     * with no enclosing reset required and no 16-frame ceiling.  The result
     * type is f's codomain (CF2). */
    return callcc_node(e, call, /*is_escape=*/false);
}

/* call-cc-completion (CC3): (escape f) - one-shot undelimited early-exit.
 * `f` receives a real continuation captured against the implicit root prompt;
 * invoking it (tur_escape_resume) unwinds to this site without re-installing a
 * prompt (the shift0-style abort flavor). */
Expr *elab_escape(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(escape f) requires exactly one argument");
        return NULL;
    }
    /* call-cc-completion (CC5): the -Xcallcc gate (and TUR-E0701) are retired --
     * see elab_call_cc.  escape now has real early-exit semantics on the CPS
     * substrate; -Xcallcc is a deprecated no-op for one release. */
    /* call-cc-completion (CC3): escape is the one-shot abort flavor.  Same
     * undelimited capture as call/cc; invoking the continuation unwinds to this
     * site without re-installing a prompt.  For the one-shot upward use both
     * flavors behave identically (the captured context is abandoned either
     * way); is_escape is recorded for the eventual shift-vs-shift0 distinction. */
    return callcc_node(e, call, /*is_escape=*/true);
}
