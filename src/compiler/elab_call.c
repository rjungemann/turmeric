/* elab_call.c -- function-call elaboration, partial application, polymorphic dispatch. */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static Expr *elab_call_hamt_fn(Elab *e, Span span, const Symbol *fn_name, uint32_t n_args, Expr **args);
static Expr *elab_lower_map_call(Elab *e, const Form *call, const Symbol *name);
static Expr *elab_partial_apply(Elab *e, const Form *call, Binding *fn_binding,
    Type fn_type, Expr **elab_args, uint32_t n_provided);
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding);
static Expr *elab_poly_call(Elab *e, const Form *call, Binding *fn_binding);
static Expr *elab_call_head_expr(Elab *e, const Form *call, Expr *head_expr);

/* Phase P3: HAMT lowering - create a call to a HAMT function binding */
static Expr *elab_call_hamt_fn(Elab *e, Span span, const Symbol *fn_name, uint32_t n_args, Expr **args) {
    /* Look up the HAMT function binding */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, fn_name, span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;
    if (!fn_binding) {
        /* HAMT module not imported - for now, we require it to be imported */
        diag_emit(DIAG_ERROR, span, "HAMT module must be imported to use persistent maps");
        return NULL;
    }
    
    /* Create the EX_CALL expression with the function's return type */
    Type result_type;
    if (fn_binding->type.kind == TY_FN) {
        result_type = type_from_kind(fn_binding->type.as.fn.result_kind);
    } else {
        result_type = TYPE_NIL;
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

/* Phase P3: HAMT lowering - lower map function calls when first arg is persistent */
static Expr *elab_lower_map_call(Elab *e, const Form *call, const Symbol *name) {
    uint32_t n_args = call->as.list.len - 1;
    
    /* Elaborate the first argument to check if it's a persistent binding */
    /* For map-new, there are no arguments, so we skip the first arg check */
    if (n_args == 0) {
        /* Only map-new takes 0 arguments */
        if (name != e->sym_map_new) {
            diag_emit(DIAG_ERROR, call->span, "map function '%s' requires at least 1 argument", name->name);
            return NULL;
        }
        /* For map-new, we don't need to check the first arg since there isn't one */
    }
    
    Expr *first_arg = NULL;
    if (n_args > 0) {
        first_arg = elab_form(e, call->as.list.items[1]);
        if (!first_arg) return NULL;
    }
    
    /* Check if first argument is a variable reference to a persistent binding */
    /* For map-new, first_arg is NULL, so we treat it as non-persistent (it creates a new map) */
    bool is_persistent_map = false;
    if (first_arg && first_arg->kind == EX_VAR && first_arg->as.var.binding->is_persistent) {
        is_persistent_map = true;
    }
    
    if (!is_persistent_map) {
        /* Not a persistent binding - fall through to normal elaboration */
        /* Re-elaborate all args together */
        Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
        if (n_args > 0) {
            args[0] = first_arg;
            for (uint32_t i = 1; i < n_args; i++) {
                args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!args[i]) return NULL;
            }
        }
        
        /* Look up the function binding */
        bool fn_qual_err = false;
        Binding *fn_binding = elab_lookup_sym(e, name, call->as.list.items[0]->span, &fn_qual_err);
        if (!fn_binding && fn_qual_err) return NULL;
        if (!fn_binding) {
            diag_emit(DIAG_ERROR, call->span, "unknown function '%s'", name->name);
            return NULL;
        }
        return elab_call_fn(e, call, fn_binding);
    }
    
    /* Mark that we need HAMT */
    e->needs_hamt = true;
    /* Phase P3: Set global flag for emit phase */
    extern bool g_needs_hamt;
    g_needs_hamt = true;
    
    /* Elaborate remaining arguments (for map-new, n_args is 0, so this is safe) */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    if (n_args > 0) {
        args[0] = first_arg;
        for (uint32_t i = 1; i < n_args; i++) {
            args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!args[i]) return NULL;
        }
    }
    
    /* Transform based on the function name */
    if (name == e->sym_map_new) {
        if (n_args != 0) {
            diag_emit(DIAG_ERROR, call->span, "map-new takes 0 arguments");
            return NULL;
        }
        /* map-new -> (hamt/new) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_new, 0, NULL);
    } else if (name == e->sym_assoc) {
        if (n_args != 3) {
            diag_emit(DIAG_ERROR, call->span, "assoc takes 3 arguments: (assoc map key value)");
            return NULL;
        }
        /* assoc m k v -> (hamt/set m (hamt_hash_ptr k) k v) */
        /* First, compute the hash: (hamt_hash_ptr k) */
        bool hash_qual_err = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err);
        if (!hash_binding && hash_qual_err) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        /* Create the hash argument */
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];  /* key */
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        /* Create args for hamt/set: m, hash, k, v */
        Expr **set_args = (Expr **)arena_alloc(e->arena, 4 * sizeof(Expr *));
        set_args[0] = args[0];  /* m */
        set_args[1] = hash_call;  /* hash */
        set_args[2] = args[1];  /* k */
        set_args[3] = args[2];  /* v */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_set, 4, set_args);
    } else if (name == e->sym_dissoc) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "dissoc takes 2 arguments: (dissoc map key)");
            return NULL;
        }
        /* dissoc m k -> (hamt/del m (hamt_hash_ptr k) k) */
        bool hash_qual_err2 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err2);
        if (!hash_binding && hash_qual_err2) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **del_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        del_args[0] = args[0];  /* m */
        del_args[1] = hash_call;  /* hash */
        del_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_del, 3, del_args);
    } else if (name == e->sym_map_get) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "get takes 2 arguments: (get map key)");
            return NULL;
        }
        /* get m k -> (hamt/get m (hamt_hash_ptr k) k) */
        bool hash_qual_err4 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err4);
        if (!hash_binding && hash_qual_err4) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **get_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        get_args[0] = args[0];  /* m */
        get_args[1] = hash_call;  /* hash */
        get_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_get, 3, get_args);
    } else if (name == e->sym_map_has) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "has? takes 2 arguments: (has? map key)");
            return NULL;
        }
        /* has? m k -> (hamt/has? m (hamt_hash_ptr k) k) */
        bool hash_qual_err3 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err3);
        if (!hash_binding && hash_qual_err3) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **has_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        has_args[0] = args[0];  /* m */
        has_args[1] = hash_call;  /* hash */
        has_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_has, 3, has_args);
    } else if (name == e->sym_map_count) {
        if (n_args != 1) {
            diag_emit(DIAG_ERROR, call->span, "count takes 1 argument: (count map)");
            return NULL;
        }
        /* count m -> (hamt/count m) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_count, 1, args);
    } else if (name == e->sym_map_merge) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "merge takes 2 arguments: (merge a b)");
            return NULL;
        }
        /* merge a b -> (hamt/merge a b) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_merge, 2, args);
    }
    
    diag_emit(DIAG_ERROR, call->span, "unexpected map function '%s'", name->name);
    return NULL;
}

static Expr *elab_call_head_expr(Elab *e, const Form *call, Expr *head_expr) {
    TypeKind head_kind = head_expr->type.kind;
    if (head_kind != TY_FN && head_kind != TY_PTR_VOID && head_kind != TY_CONT) {
        diag_emit(DIAG_ERROR, call->as.list.items[0]->span,
                  "expression in call head has type `%s`, which is not callable",
                  type_name(head_expr->type));
        return NULL;
    }

    char tmp_name[32];
    snprintf(tmp_name, sizeof(tmp_name), "__call_head_%u", e->next_id++);
    const Symbol *tmp_sym = symtab_intern(e->st, strslice(tmp_name, (uint32_t)strlen(tmp_name)));
    Binding *tmp_b = binding_new(e, tmp_sym, head_expr->type, false, false, call->as.list.items[0]->span);

    Expr *source_expr = head_expr;
    while (source_expr && source_expr->kind == EX_ASCRIBE) {
        source_expr = source_expr->as.ascribe_.inner;
    }
    if (source_expr && source_expr->kind == EX_CLOSURE &&
        source_expr->as.closure_.closure && source_expr->as.closure_.closure->fn) {
        tmp_b->closure_fn_binding = source_expr->as.closure_.closure->fn->binding;
    } else if (source_expr && source_expr->kind == EX_VAR && source_expr->as.var.binding) {
        Binding *source_b = source_expr->as.var.binding;
        tmp_b->closure_fn_binding = source_b->closure_fn_binding;
        if (source_b->is_poly_fn) {
            tmp_b->is_poly_fn = true;
            tmp_b->poly_type = source_b->poly_type;
        }
    }

    Expr *call_expr = elab_call_fn(e, call, tmp_b);
    if (!call_expr) return NULL;

    LetBinding *let_bs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
    let_bs->binding = tmp_b;
    let_bs->init = head_expr;

    Expr *let_expr = expr_new(e->arena, EX_LET, call_expr->type, call->span);
    let_expr->as.let_.bindings = let_bs;
    let_expr->as.let_.n = 1;
    let_expr->as.let_.body = call_expr;
    return let_expr;
}

/* ---- general elab ---- */

Expr *elab_call(Elab *e, Form *call) {
    /* Already established: call->tag == F_LIST and len >= 1. */
    Form *head = call->as.list.items[0];

    /* General callable-expression heads: ((expr) args...). */
    if (head->tag != F_SYM) {
        Expr *head_expr = elab_form(e, head);
        if (!head_expr) return NULL;
        return elab_call_head_expr(e, call, head_expr);
    }
    const Symbol *name = head->as.sym;

    /* Special forms. */
    if (name == e->sym_def)    return elab_def   (e, call);
    if (name == e->sym_let)    return elab_let   (e, call);
    if (name == e->sym_if)     return elab_if    (e, call);
    if (name == e->sym_do)     return elab_do    (e, call);
    if (name == e->sym_unsafe) return elab_unsafe(e, call);
    if (name == e->sym_set)    return elab_set   (e, call);
    if (name == e->sym_while)  return elab_while (e, call);
    if (name == e->sym_case)   return elab_case  (e, call);
    /* Phase 4 */
    if (name == e->sym_defer)  return elab_defer (e, call);
    if (name == e->sym_return) return elab_return(e, call);
    /* GF1: Generator forms */
    if (name == e->sym_gen)      return elab_gen     (e, call);
    if (name == e->sym_yield)    return elab_yield   (e, call);
    if (name == e->sym_gen_next) return elab_gen_next(e, call);
    if (name == e->sym_gen_done) return elab_gen_done(e, call);
    /* Phase 5 */
    if (name == e->sym_ref)    return elab_ref   (e, call);
    if (name == e->sym_deref)  return elab_deref (e, call);
    if (name == e->sym_drop)   return elab_drop  (e, call);
    /* LT3: lref<T> */
    if (name == e->sym_lref_new) return elab_lref_new(e, call);
    /* Phase 9: rc<T> + weak<T> */
    if (name == e->sym_rc_of)       return elab_rc_of(e, call);
    if (name == e->sym_rc_clone)    return elab_rc_clone(e, call);
    if (name == e->sym_rc_drop)     return elab_rc_drop(e, call);
    if (name == e->sym_rc_ptr)      return elab_rc_ptr(e, call);
    if (name == e->sym_rc_strong_count) return elab_rc_strong_count(e, call);
    if (name == e->sym_rc_from_ref) return elab_rc_from_ref(e, call);
    if (name == e->sym_ref_from_rc) return elab_ref_from_rc(e, call);
    if (name == e->sym_weak)        return elab_weak(e, call);
    if (name == e->sym_upgrade)     return elab_weak_upgrade(e, call);
    if (name == e->sym_weak_pred)   return elab_weak_pred(e, call);
    if (name == e->sym_ref_pred)    return elab_ref_pred(e, call);
    /* Phase 18: Delimited continuations */
    if (name == e->sym_reset)      return elab_reset(e, call);
    if (name == e->sym_shift)      return elab_shift(e, call);
    if (name == e->sym_shift0)     return elab_shift0(e, call);
    if (name == e->sym_call_cc)    return elab_call_cc(e, call);
    if (name == e->sym_escape)     return elab_escape(e, call);
    /* Phase B2: Cloneable continuations */
    if (name == e->sym_cloneable_reset)  return elab_cloneable_reset(e, call);
    if (name == e->sym_cloneable_shift)  return elab_cloneable_shift(e, call);
    if (name == e->sym_call_cc_star)      return elab_call_cc_star(e, call);
    /* Phase 21: Serializable continuations */
    if (name == e->sym_serial_reset) return elab_serial_reset(e, call);
    if (name == e->sym_serial_shift) return elab_serial_shift(e, call);
    /* DV0-DV1: Dynamic vars */
    if (name == e->sym_defdynamic) return elab_defdynamic(e, call);
    if (name == e->sym_binding)    return elab_binding   (e, call);
    /* Phase 19: Algebraic effects */
    if (name == e->sym_defeffect) return elab_defeffect(e, call);
    if (name == e->sym_perform)   return elab_perform(e, call);
    if (name == e->sym_handle)       return elab_handle(e, call);
    if (name == e->sym_try_with)     return elab_try_with(e, call);
    if (name == e->sym_with_handler) return elab_handle(e, call);  /* T25: sugar for handle in async context */
    if (name == e->sym_resume)    return elab_resume(e, call);
    if (name == e->sym_discontinue) return elab_discontinue(e, call);
    /* ET3-E: compose-handlers */
    if (name == e->sym_compose_handlers) return elab_compose_handlers(e, call);
    if (name == e->sym_cont_pred)   return elab_cont_pred(e, call);
    /* Phase 10: GC */
    if (name == e->sym_gc_force)    return elab_gc_force(e, call);
    if (name == e->sym_gc_enable)   return elab_gc_enable(e, call);
    if (name == e->sym_gc_disable)  return elab_gc_disable(e, call);
    /* Phase M0: Module system */
    if (name == e->sym_load)      return elab_load(e, call);
    if (name == e->sym_defmodule) return elab_defmodule(e, call);
    if (name == e->sym_export) {
        diag_emit(DIAG_ERROR, call->span,
                  "export is only allowed inside defmodule");
        return NULL;
    }
    if (name == e->sym_import) {
        diag_emit(DIAG_ERROR, call->span,
                  "import is only allowed inside defmodule");
        return NULL;
    }
    /* Phase N: numeric cast */
    if (name == e->sym_as) return elab_as_cast(e, call);
    /* IT4: gradual typing */
    if (name == e->sym_type_of) return elab_any_type_of(e, call);
    if (name == e->sym_cast)    return elab_any_cast(e, call);
    /* Phase 11: defstruct */
    if (name == e->sym_defstruct) return elab_defstruct(e, call);
    if (name == e->sym_make_struct) return elab_make_struct(e, call);
    /* SI4-C: defopaque */
    if (name == e->sym_defopaque) return elab_defopaque(e, call);
    /* Phase G0: ADTs */
    if (name == e->sym_defdata) return elab_defdata(e, call);
    if (name == e->sym_match) return elab_match(e, call);
    if (name == e->sym_defgadt) return elab_defgadt(e, call);
    if (name == e->sym_coerce)  return elab_coerce(e, call);
    /* Phase 12: Borrow traits */
    if (name == e->sym_borrow) return elab_borrow_immut(e, call);
    if (name == e->sym_borrow_mut) return elab_borrow_mut(e, call);
    /* Phase 15: Typeclasses */
    if (name == e->sym_defclass) return elab_defclass(e, call);
    if (name == e->sym_definstance) return elab_definstance(e, call);
    /* Phase HKT H5: kind aliases */
    if (name == e->sym_defkind) return elab_defkind(e, call);
    /* Phase HKT-P2: recursive type binders */
    if (name == e->sym_defrec) return elab_defrec(e, call);
    if (name == e->sym_deftype) return elab_deftype(e, call);
    /* Phase TA1: defalias */
    if (name == e->sym_defalias) return elab_defalias(e, call);
    /* Phase HRT0: forall/exists are type-level forms; reject in expression position */
    if (name == e->sym_forall || name == e->sym_forall_u) {
        diag_emit(DIAG_ERROR, call->span,
                  "'forall' is a type-level annotation and cannot appear in expression position "
                  "(use it in a type annotation: (deftype MyType (forall [a] ...)))");
        return NULL;
    }
    if (name == e->sym_exists || name == e->sym_exists_u) {
        diag_emit(DIAG_ERROR, call->span,
                  "'exists' is a type-level annotation and cannot appear in expression position "
                  "(use it in a type annotation: (deftype MyType (exists [a] ...)))");
        return NULL;
    }
    /* Phase HKT-P1: type-level application */
    if (name == e->sym_type_app) return elab_type_app(e, call);
    /* Phase HRT1: (:: expr type) — type ascription */
    if (name == e->sym_ascribe) return elab_ascribe(e, call);
    /* Phase HRT2: existential types */
    if (name == e->sym_pack) return elab_pack(e, call);
    if (name == e->sym_open) return elab_open(e, call);
    /* SS0b: Session channel operations (-Xsessions) */
    if (g_sessions_enabled) {
        if (name == e->sym_defprotocol)   return elab_defprotocol(e, call);
        if (name == e->sym_make_protocol) return elab_make_protocol(e, call);
        if (name == e->sym_send_to)       return elab_send_to(e, call);
        if (name == e->sym_recv_from)     return elab_recv_from(e, call);
        if (name == e->sym_make_session)  return elab_session_make(e, call);
        if (name == e->sym_send)          return elab_session_send(e, call);
        if (name == e->sym_recv)          return elab_session_recv(e, call);
        /* SS5: close handles both TY_SESSION (binary) and TY_ROLE (multi-party) */
        if (name == e->sym_close)         return elab_session_close(e, call);
        if (name == e->sym_offer)         return elab_session_offer(e, call);
        if (name == e->sym_choose_left)   return elab_session_choose_left(e, call);
        if (name == e->sym_choose_right)  return elab_session_choose_right(e, call);
        if (name == e->sym_recv_timeout)  return elab_session_recv_timeout(e, call);
    }
    /* Phase R2: Panic */
    if (name == e->sym_panic) return elab_panic(e, call);
    if (name == e->sym_panic_with) return elab_panic_with(e, call);
    if (name == e->sym_catch_unwind) return elab_catch_unwind(e, call);
    if (name == e->sym_catch_panic_of) return elab_catch_panic_of(e, call);
    if (name == e->sym_throw) return elab_throw(e, call);
    if (name == e->sym_try)   return elab_try_catch(e, call);
    if (name == e->sym_panic_payload_type) return elab_panic_payload_type(e, call);
    if (name == e->sym_panic_payload_value) return elab_panic_payload_value(e, call);
    if (name == e->sym_panic_payload_file) return elab_panic_payload_file(e, call);
    if (name == e->sym_panic_payload_line) return elab_panic_payload_line(e, call);
    if (name == e->sym_panic_payload_downcast) return elab_panic_payload_downcast(e, call);
    /* Phase U3: Unsafe primitives - pointer operations */
    if (name == e->sym_ptr_deref)   return elab_ptr_deref(e, call);
    if (name == e->sym_ptr_write)  return elab_ptr_write(e, call);
    if (name == e->sym_ptr_add)     return elab_ptr_add(e, call);
    if (name == e->sym_ptr_sub)     return elab_ptr_sub(e, call);
    if (name == e->sym_ptr_nullq)   return elab_ptr_nullq(e, call);
    if (name == e->sym_ptr_of)      return elab_ptr_of(e, call);
    /* Phase U3: Unsafe primitives - type casting */
    if (name == e->sym_unsafe_cast) return elab_unsafe_cast(e, call);
    if (name == e->sym_reinterpret) return elab_reinterpret(e, call);
    if (name == e->sym_transmute)   return elab_transmute(e, call);
    /* Phase U3: Unsafe primitives - unchecked array ops */
    if (name == e->sym_array_get_unchecked)  return elab_array_get_unchecked(e, call);
    if (name == e->sym_array_set_unchecked)  return elab_array_set_unchecked(e, call);
    /* Phase U3: Unsafe primitives - raw memory */
    if (name == e->sym_raw_malloc)  return elab_raw_malloc(e, call);
    if (name == e->sym_raw_free)    return elab_raw_free(e, call);
    if (name == e->sym_raw_realloc) return elab_raw_realloc(e, call);
    if (name == e->sym_raw_memcpy)  return elab_raw_memcpy(e, call);
    if (name == e->sym_raw_memset)  return elab_raw_memset(e, call);
    /* Phase U3: Unsafe primitives - FFI */
    if (name == e->sym_c_call)      return elab_c_call(e, call);
    if (name == e->sym_dlopen)      return elab_dlopen(e, call);
    if (name == e->sym_dlsym)       return elab_dlsym(e, call);
    if (name == e->sym_dlclose)     return elab_dlclose(e, call);
    /* Phase T19-B: thread-spawn (Send-safety check for cross-thread closures) */
    /* Only intercept when arg[1] is a literal (fn ...) form; if the user has  */
    /* defined their own thread-spawn function, let it fall through below.      */
    if (name == e->sym_thread_spawn &&
        call->as.list.len == 2 &&
        call->as.list.items[1]->tag == F_LIST &&
        call->as.list.items[1]->as.list.len >= 1 &&
        call->as.list.items[1]->as.list.items[0]->tag == F_SYM &&
        call->as.list.items[1]->as.list.items[0]->as.sym == e->sym_fn)
        return elab_thread_spawn(e, call);
    /* Phase T21-F: async/await sugar */
    if (name == e->sym_async && call->as.list.len == 2)
        return elab_async(e, call);
    if (name == e->sym_await && call->as.list.len == 2)
        return elab_await(e, call);
    /* Phase SEL1: fair multi-channel select */
    if (name == e->sym_select && call->as.list.len >= 2)
        return elab_select(e, call);
    /* Phase 20: Software Transactional Memory */
    if (name == e->sym_stm) return elab_stm(e, call);
    if (name == e->sym_atomically && call->as.list.len == 2)
        return elab_atomically(e, call);
    if (name == e->sym_retry) return elab_retry(e, call);
    if (name == e->sym_check && call->as.list.len == 2)
        return elab_check(e, call);
    if (name == e->sym_or_else && call->as.list.len == 3)
        return elab_or_else(e, call);
    if (name == e->sym_tvar_new && call->as.list.len == 2)
        return elab_tvar_new(e, call);
    if (name == e->sym_tvar_read && call->as.list.len == 2)
        return elab_tvar_read(e, call);
    if (name == e->sym_tvar_write && call->as.list.len == 3)
        return elab_tvar_write(e, call);
    if (name == e->sym_tvar_modify && call->as.list.len == 3)
        return elab_tvar_modify(e, call);
    if (name == e->sym_tvar_swap && call->as.list.len == 3)
        return elab_tvar_swap(e, call);
    if (name == e->sym_tvar_cas && call->as.list.len == 4)
        return elab_tvar_cas(e, call);
    /* TVar operations with / syntax */
    if (name == e->sym_tvar && call->as.list.len >= 2) {
        Form *op = call->as.list.items[1];
        if (op->tag == F_SYM) {
            if (op->as.sym == e->sym_new && call->as.list.len == 3)
                return elab_tvar_new(e, call);
            if (op->as.sym == e->sym_read && call->as.list.len == 3)
                return elab_tvar_read(e, call);
            if (op->as.sym == e->sym_write && call->as.list.len == 4)
                return elab_tvar_write(e, call);
            if (op->as.sym == e->sym_modify && call->as.list.len == 4)
                return elab_tvar_modify(e, call);
            if (op->as.sym == e->sym_swap && call->as.list.len == 4)
                return elab_tvar_swap(e, call);
            if (op->as.sym == e->sym_cas && call->as.list.len == 5)
                return elab_tvar_cas(e, call);
        }
    }
    /* Phase R1: ? operator — lowers to early-return on err */
    if (name == e->sym_question) {
        return elab_question(e, call);
    }
    /* Phase 15: Method call syntax - (.method obj arg1 arg2) */
    if (name->len > 0 && name->name[0] == '.') {
        return elab_method_call(e, call);
    }
    /* Phase 6 */
    if (name == e->sym_defmacro) return elab_defmacro(e, call);
    if (name == e->sym_quote)    return elab_form(e, call->as.list.items[1]); /* (quote x) -> x */
    if (name == e->sym_gensym)   return elab_gensym(e, call);
    if (name == e->sym_thread)    return elab_thread(e, call);
    if (name == e->sym_thread_last) return elab_thread_last(e, call);

    /* Phase 2 */
    if (name == e->sym_defn)    return elab_defn  (e, call);
    if (name == e->sym_fn)      return elab_fn    (e, call);
    if (name == e->sym_lambda)  return elab_fn    (e, call); /* λ aliases fn */
    if (name == e->sym_extern_c) return elab_extern_c(e, call);

    /* Phase P3: HAMT lowering - lower map function calls when first arg is persistent */
    if (name == e->sym_map_new || name == e->sym_assoc || name == e->sym_dissoc ||
        name == e->sym_map_get || name == e->sym_map_has || name == e->sym_map_count ||
        name == e->sym_map_merge) {
        return elab_lower_map_call(e, call, name);
    }

    /* Phase 6: Check if it's a macro call */
    MacroDef *macro = elab_lookup_macro(e, name);
    if (macro) {
        if (e->macro_expand_depth >= ELAB_MAX_MACRO_EXPANSION_DEPTH) {
            diag_emit(DIAG_ERROR, call->span, "maximum macro expansion depth exceeded");
            return NULL;
        }
        e->macro_expand_depth++;
        /* Expand the macro with arguments */
        /* Extract arguments (rest of list) */
        uint32_t n_args = call->as.list.len - 1;
        Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(e->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = call->as.list.items[1 + i];
        }
        
        Form *expanded = elab_expand_macro(e, macro, args, n_args);
        if (!expanded) {
            e->macro_expand_depth--;
            return NULL;
        }

        /* Phase M4: Keep the expansion-module context active while elaborating
         * the expanded form so private helper macros from the same module are
         * visible when the expansion calls them (e.g. triple → helper-double). */
        const Symbol *saved_expansion = e->macro_expansion_module;
        e->macro_expansion_module = macro->defining_module_name;
        Expr *out = elab_form(e, expanded);
        e->macro_expansion_module = saved_expansion;
        e->macro_expand_depth--;
        return out;
    }

    /* Phase 2: Check if it's a user-defined function call.
     * M1: Use elab_lookup_sym for visibility + qualified name resolution. */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, name, head->span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;

    /* Phase G0: constructor call — (Ctor) or (Ctor :T1 ...) */
    if (fn_binding && fn_binding->type.kind == TY_ADT) {
        /* 0-arg constructor */
        AdtDef *adt = fn_binding->type.as.adt_.def;
        CtorDef *ctor = NULL;
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            if (strcmp(adt->ctors[ci]->name, name->name) == 0) {
                ctor = adt->ctors[ci];
                break;
            }
        }
        if (ctor && ctor->n_fields == 0) {
            uint32_t n_args_given = call->as.list.len - 1;
            if (n_args_given != 0) {
                diag_emit(DIAG_ERROR, call->span,
                          "constructor '%s' takes 0 arguments, got %u",
                          name->name, n_args_given);
                return NULL;
            }
            Expr *out = expr_new(e->arena, EX_CALL, fn_binding->type, call->span);
            out->as.call_.fn_binding = fn_binding;
            out->as.call_.args = NULL;
            out->as.call_.n_args = 0;
            out->as.call_.fn_expr = NULL;
            out->as.call_.dict_arg = NULL;
            return out;
        }
    }

    /* Phase G0: N-arg constructor call — result type needs ADT def pointer */
    if (fn_binding && fn_binding->type.kind == TY_FN &&
        fn_binding->type.as.fn.result_kind == TY_ADT) {
        /* Look up the constructor to find its AdtDef */
        CtorDef *ctor = elab_lookup_ctor(e, name);
        if (ctor) {
            /* Use elab_call_fn but fix up the result type after */
            Expr *call_expr = elab_call_fn(e, call, fn_binding);
            if (call_expr) {
                /* Patch result type with proper AdtDef pointer */
                call_expr->type = type_adt(ctor->adt);
            }
            return call_expr;
        }
        /* Phase G3: Non-constructor function returning ADT — patch result from result_full_type */
        Expr *call_expr = elab_call_fn(e, call, fn_binding);
        if (call_expr && fn_binding->type.as.fn.result_full_type) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        return call_expr;
    }

    if (fn_binding && (fn_binding->type.kind == TY_FN ||
                       (fn_binding->type.kind == TY_PTR_VOID && fn_binding->closure_fn_binding) ||
                       fn_binding->closure_fn_binding)) {
        Expr *call_expr = elab_call_fn(e, call, fn_binding);
        /* LT4: patch struct return type with full type containing StructDef pointer,
         * mirroring the G3 patch for TY_ADT above. Without this, the call expression
         * gets TY_STRUCT with def=NULL from type_from_kind(TY_STRUCT). */
        if (call_expr && fn_binding->type.kind == TY_FN &&
            fn_binding->type.as.fn.result_kind == TY_STRUCT &&
            fn_binding->type.as.fn.result_full_type) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        /* PTC4: patch TY_APP return type so dispatch can extract concrete elem types. */
        if (call_expr && fn_binding->type.kind == TY_FN &&
            fn_binding->type.as.fn.result_kind == TY_APP &&
            fn_binding->type.as.fn.result_full_type) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        /* F1-1: patch TY_EXISTS / TY_FORALL return type with the full
         * forall_ payload so `open` (and any other downstream consumer
         * that dereferences `as.forall_.body`) sees a populated struct
         * instead of a zero-initialised type_from_kind() shell. */
        if (call_expr && fn_binding->type.kind == TY_FN &&
            (fn_binding->type.as.fn.result_kind == TY_EXISTS ||
             fn_binding->type.as.fn.result_kind == TY_FORALL) &&
            fn_binding->type.as.fn.result_full_type) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        return call_expr;
    }

    /* Phase 19: Allow calling any binding (for function parameters, higher-order functions) */
    if (fn_binding) {
        return elab_call_fn(e, call, fn_binding);
    }

    /* Builtin operator. Evaluate args first, then look up. */
    uint32_t n_args = call->as.list.len - 1;
    Expr **args = (n_args == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it.
         * UT2 exception: ^unique ^mut bindings represent exclusive mutable access;
         * builtins never take unique ownership, so don't consume them. */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            Binding *arg_b2 = args[i]->as.var.binding;
            bool arg_is_unique_mut = g_unique_enabled && arg_b2->is_unique && arg_b2->is_mut;
            if (!arg_is_unique_mut) {
                binding_mark_moved(arg_b2, args[i]->span);
            }
        }
    }
    Type first_t = (n_args > 0) ? args[0]->type : TYPE_NIL;
    const BuiltinSpec *spec = builtin_lookup(name, first_t, n_args);
    if (!spec) {
        const BuiltinSpec *any = builtin_first_with_name(name);
        if (any) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0006_OPERATOR_LOOKUP_FAILED,
                                "operator lookup failed for '%s': got %u arg(s), first arg type %s",
                                name->name, n_args,
                                n_args > 0 ? type_name(first_t) : "<none>");

            const BuiltinSpec *overloads[32];
            uint32_t n_overloads = builtin_collect_with_name(name, overloads, 32);
            for (uint32_t oi = 0; oi < n_overloads; oi++) {
                const BuiltinSpec *ov = overloads[oi];
                const char *arg_name = (ov->arg_type.kind == TY_UNKNOWN)
                    ? "any"
                    : type_name(ov->arg_type);
                const char *res_name = type_name(ov->result_type);
                if (ov->max_arity < 0) {
                    diag_emit(DIAG_NOTE, call->span,
                              "available overload: %s arity %d..* arg=%s result=%s",
                              ov->name, ov->min_arity, arg_name, res_name);
                } else {
                    diag_emit(DIAG_NOTE, call->span,
                              "available overload: %s arity %d..%d arg=%s result=%s",
                              ov->name, ov->min_arity, ov->max_arity, arg_name, res_name);
                }
            }
        } else if (e->separate_compilation) {
            diag_emit(DIAG_ERROR, head->span,
                      "unknown function or operator '%s'", name->name);
        } else {
            /* eval mode: create a runtime-dispatch call so native builtins
             * registered in TuriEnv (e.g. async scheduler functions) are
             * callable without a compile-time declaration.
             * The binding is NOT added to any scope so future lookups don't
             * find a TYPE_INT entry and route through elab_call_fn. */
            Binding *dyn_b = binding_new(e, name, TYPE_INT, false, false, head->span);
            Expr *var_expr = expr_new(e->arena, EX_VAR, TYPE_INT, head->span);
            var_expr->as.var.binding = dyn_b;
            Expr *out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
            out->as.call_.fn_binding = NULL;
            out->as.call_.fn_expr    = var_expr;
            out->as.call_.args       = args;
            out->as.call_.n_args     = n_args;
            return out;
        }
        return NULL;
    }
    /* All args must match the spec's arg type. */
    for (uint32_t i = 0; i < n_args; i++) {
        if (!type_eq(args[i]->type, spec->arg_type)) {
            const char *expected_str = type_name(spec->arg_type);
            const char *actual_str = type_name(args[i]->type);
            
            /* Check if we can suggest a coercion */
            const char *suggestion = NULL;
            if (args[i]->type.kind == TY_BOOL && spec->arg_type.kind == TY_INT) {
                suggestion = "try wrapping the bool in (if x 1 0)";
            } else if (typekind_is_numeric(args[i]->type.kind) &&
                       typekind_is_numeric(spec->arg_type.kind)) {
                suggestion = "use (as <type> expr) for explicit numeric conversion";
            }

            diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0001_TYPE_MISMATCH,
                                "'%s' arg %u: type mismatch - expected %s, got %s",
                                name->name, i + 1, expected_str, actual_str);
            if (suggestion) {
                diag_emit(DIAG_HELP, args[i]->span, "%s", suggestion);
            }
            diag_emit(DIAG_NOTE, args[i]->span, "argument has this type");
            return NULL;
        }
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    out->as.builtin.args = args;
    out->as.builtin.n = n_args;
    return out;
}

/* CY1: Partially apply a function, returning a closure over the provided args.
 *
 * fn_type   -- the EFFECTIVE function type (thunk type if closure, including env param)
 * fn_binding -- the binding being called (closure_fn_binding != NULL iff it's a closure)
 * elab_args -- already-elaborated argument expressions [0..n_provided-1]
 * n_provided -- number of arguments already provided (< full_arity)
 */
static Expr *elab_partial_apply(Elab *e, const Form *call, Binding *fn_binding,
                                 Type fn_type, Expr **elab_args, uint32_t n_provided) {
    bool fn_is_closure = (fn_binding->closure_fn_binding != NULL);

    /* full_arity = user-visible arg count (strips env from thunk arity) */
    uint32_t full_arity = fn_type.as.fn.arity;
    if (fn_is_closure) full_arity--; /* strip hidden env param */

    uint32_t n_remaining = full_arity - n_provided;
    TypeKind result_kind = fn_type.as.fn.result_kind;

    /* Build capture bindings for provided args */
    Binding **cap_bindings = (Binding **)arena_alloc(e->arena, n_provided * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        char cap_name[32];
        snprintf(cap_name, sizeof(cap_name), "__papc%u", e->next_id++);
        const Symbol *cap_sym = symtab_intern(e->st, strslice(cap_name, (uint32_t)strlen(cap_name)));
        /* arg type from fn_type: skip index 0 if closure (env), so index = i+1 if closure, else i */
        TypeKind cap_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (i + 1) : i];
        Type cap_type = type_from_kind(cap_kind);
        Binding *cap_b = binding_new(e, cap_sym, cap_type, false, false, call->span);
        cap_bindings[i] = cap_b;
    }

    /* Build remaining param bindings for the new thunk */
    Binding **rem_params = (Binding **)arena_alloc(e->arena, n_remaining * sizeof(Binding *));
    TypeKind rem_kinds[MAX_FN_ARITY];
    for (uint32_t i = 0; i < n_remaining; i++) {
        char rem_name[32];
        snprintf(rem_name, sizeof(rem_name), "__papr%u", e->next_id++);
        const Symbol *rem_sym = symtab_intern(e->st, strslice(rem_name, (uint32_t)strlen(rem_name)));
        TypeKind rem_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (n_provided + 1 + i) : (n_provided + i)];
        Type rem_type = type_from_kind(rem_kind);
        Binding *rem_b = binding_new(e, rem_sym, rem_type, false, false, call->span);
        rem_params[i] = rem_b;
        rem_kinds[i] = rem_kind;
    }

    /* Build the inner call expression: (fn_binding cap0 cap1 ... rem0 rem1 ...) */
    /* n_call_args = full_arity (all user-visible args) */
    uint32_t n_call_args = full_arity;
    Expr **call_args = (Expr **)arena_alloc(e->arena, n_call_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_provided; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, cap_bindings[i]->type, call->span);
        var->as.var.binding = cap_bindings[i];
        call_args[i] = var;
    }
    for (uint32_t i = 0; i < n_remaining; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, rem_params[i]->type, call->span);
        var->as.var.binding = rem_params[i];
        call_args[n_provided + i] = var;
    }

    Type body_result_type = type_from_kind(result_kind);
    Expr *inner_call = expr_new(e->arena, EX_CALL, body_result_type, call->span);
    inner_call->as.call_.fn_binding = fn_binding;
    inner_call->as.call_.args = call_args;
    inner_call->as.call_.n_args = n_call_args;
    inner_call->as.call_.fn_expr = NULL;
    inner_call->as.call_.dict_arg = NULL;
    inner_call->as.call_.is_poly_call = false;
    inner_call->as.call_.poly_arg_mask = 0;

    /* Build the thunk FnDef */
    /* Thunk params: [env_param (TY_PTR_VOID), rem_param_0, ..., rem_param_{n_remaining-1}] */
    uint8_t thunk_n_params = (uint8_t)(1 + n_remaining);
    Binding **thunk_params = (Binding **)arena_alloc(e->arena, thunk_n_params * sizeof(Binding *));
    Type *thunk_param_types = (Type *)arena_alloc(e->arena, thunk_n_params * sizeof(Type));

    /* env param */
    char env_param_name[32];
    snprintf(env_param_name, sizeof(env_param_name), "__pap_env_%u", e->next_id++);
    const Symbol *env_param_sym = symtab_intern(e->st, strslice(env_param_name, (uint32_t)strlen(env_param_name)));
    Binding *env_param_b = binding_new(e, env_param_sym, TYPE_PTR_VOID, false, false, call->span);
    thunk_params[0] = env_param_b;
    thunk_param_types[0] = TYPE_PTR_VOID;

    for (uint32_t i = 0; i < n_remaining; i++) {
        thunk_params[1 + i] = rem_params[i];
        thunk_param_types[1 + i] = type_from_kind(rem_kinds[i]);
    }

    /* Thunk type: (TY_PTR_VOID, rem_kinds...) -> result_kind */
    TypeKind thunk_arg_kinds[MAX_FN_ARITY];
    thunk_arg_kinds[0] = TY_PTR_VOID;
    for (uint32_t i = 0; i < n_remaining; i++) {
        thunk_arg_kinds[1 + i] = rem_kinds[i];
    }
    Type thunk_type = type_fn(thunk_arg_kinds, thunk_n_params, result_kind);

    /* Thunk binding (global) */
    char pap_name[32];
    snprintf(pap_name, sizeof(pap_name), "__pap%u", e->next_id++);
    const Symbol *pap_sym = symtab_intern(e->st, strslice(pap_name, (uint32_t)strlen(pap_name)));
    Binding *thunk_binding = binding_new(e, pap_sym, thunk_type, false, true, call->span);
    scope_add(&e->global, thunk_binding);

    /* Build FnDef */
    FnDef *pap_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(pap_fd, 0, sizeof(FnDef));
    pap_fd->binding = thunk_binding;
    pap_fd->params = thunk_params;
    pap_fd->n_params = thunk_n_params;
    pap_fd->param_types = thunk_param_types;
    pap_fd->body = inner_call;
    pap_fd->is_variadic = false;
    pap_fd->inferred_effect_row = NULL;
    constraint_set_init(&pap_fd->constraints);

    /* Build the env struct name */
    char pap_env_name[32];
    snprintf(pap_env_name, sizeof(pap_env_name), "__pap_env_s_%u", e->next_id++);
    const Symbol *pap_env_sym = symtab_intern(e->st, strslice(pap_env_name, (uint32_t)strlen(pap_env_name)));

    /* Build captures list: [cap_binding[0], ..., cap_binding[n_provided-1]] + fn_binding if closure */
    uint32_t n_pap_captures = n_provided + (fn_is_closure ? 1 : 0);
    Binding **pap_captures = (Binding **)arena_alloc(e->arena, (n_pap_captures ? n_pap_captures : 1) * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        pap_captures[i] = cap_bindings[i];
    }
    if (fn_is_closure) {
        pap_captures[n_provided] = fn_binding;
    }

    /* Build Closure struct */
    struct Closure *pap_closure = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
    pap_closure->fn = pap_fd;
    pap_closure->captures = pap_captures;
    pap_closure->n_captures = (uint8_t)n_pap_captures;
    pap_closure->env_name = pap_env_sym;

    /* Wire closure into FnDef (required for emit_fn_def to emit the env struct) */
    pap_fd->closure = pap_closure;

    /* Register thunk at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, thunk_type, call->span);
    fn_def_expr->as.fn_def_.fn = pap_fd;
    elab_register_file_def(e, fn_def_expr);

    /* Build EX_CLOSURE */
    Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, TYPE_PTR_VOID, call->span);
    closure_expr->as.closure_.closure = pap_closure;

    if (n_provided == 0) {
        /* Edge case: no args provided, just return the closure directly */
        return closure_expr;
    }

    /* Wrap in EX_LET: let [cap0 = arg0, cap1 = arg1, ...] closure_expr */
    LetBinding *let_bs = (LetBinding *)arena_alloc(e->arena, n_provided * sizeof(LetBinding));
    for (uint32_t i = 0; i < n_provided; i++) {
        let_bs[i].binding = cap_bindings[i];
        let_bs[i].init = elab_args[i];
    }

    Expr *let_expr = expr_new(e->arena, EX_LET, TYPE_PTR_VOID, call->span);
    let_expr->as.let_.bindings = let_bs;
    let_expr->as.let_.n = n_provided;
    let_expr->as.let_.body = closure_expr;

    return let_expr;
}

/* Phase 2: Elaborate a function call (f a b c) */
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* Get the function type */
    Type fn_type = fn_binding->type;
    
    /* Phase HRT1: rank-2 polymorphic function parameter call — intercept before closure/PTR_VOID. */
    if (fn_binding->is_poly_fn) {
        return elab_poly_call(e, call, fn_binding);
    }

    /* For closure bindings, use the closure's thunk function type */
    if (fn_binding->closure_fn_binding) {
        /* This is a closure - get the thunk function type */
        fn_type = fn_binding->closure_fn_binding->type;
    } else if (fn_binding->type.kind == TY_PTR_VOID) {
        /* CY2: fat-closure dynamic dispatch through ptr<void> binding.
         * Supports 0-arg (original behavior) and n-arg (new fat-closure call). */
        Expr **cb_args = NULL;
        if (n_args > 0) {
            cb_args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args; i++) {
                cb_args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!cb_args[i]) return NULL;
            }
        }
        Expr *out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
        out->as.call_.fn_binding = fn_binding;
        out->as.call_.args = cb_args;
        out->as.call_.n_args = n_args;
        out->as.call_.fn_expr = NULL;
        out->as.call_.dict_arg = NULL;
        out->as.call_.is_poly_call = false;
        out->as.call_.poly_arg_mask = 0;
        return out;
    }
    
    if (fn_type.kind != TY_FN && fn_type.kind != TY_CONT) {
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' is not a function or continuation", fn_binding->name->name);
        return NULL;
    }

    if (fn_type.kind == TY_FN &&
        e->unsafe_depth == 0 &&
        effect_row_contains_symbol(fn_type.as.fn.effect_row, e->sym_effect_unsafe)) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe function '%s' requires an enclosing (unsafe ...)",
                  fn_binding->name->name);
        return NULL;
    }

    uint8_t expected_arity = 0;
    if (fn_type.kind == TY_FN) {
        expected_arity = fn_type.as.fn.arity;
        
        /* For closure bindings, the thunk function has an extra env parameter */
        if (fn_binding->closure_fn_binding) {
            expected_arity--;  /* Subtract the hidden env parameter */
        }
    } else if (fn_type.kind == TY_CONT) {
        /* Continuations are callable with exactly 1 argument (the resume value) */
        expected_arity = 1;
    }
    
    if (n_args < expected_arity && fn_type.kind == TY_FN) {
        /* CY1: Partial application */
        Expr **pap_elab_args = (n_args > 0)
            ? (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *))
            : NULL;
        for (uint32_t i = 0; i < n_args; i++) {
            pap_elab_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!pap_elab_args[i]) return NULL;
        }
        return elab_partial_apply(e, call, fn_binding, fn_type, pap_elab_args, n_args);
    }
    if (n_args > expected_arity && fn_type.kind == TY_FN) {
        /* CY2: Over-application */
        TypeKind result_kind = fn_type.as.fn.result_kind;
        if (result_kind != TY_PTR_VOID) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                                "function '%s' returns %s, which is not callable -- "
                                "did you mean to pass all %u argument(s)?",
                                fn_binding->name->name,
                                type_name(type_from_kind(result_kind)),
                                expected_arity);
            return NULL;
        }
        /* Elaborate the first expected_arity args (used in inner call) */
        Expr **inner_args = (Expr **)arena_alloc(e->arena, expected_arity * sizeof(Expr *));
        for (uint32_t i = 0; i < expected_arity; i++) {
            inner_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!inner_args[i]) return NULL;
        }
        /* Build inner call expression */
        Type inner_result_type = type_from_kind(result_kind);
        Expr *inner_call = expr_new(e->arena, EX_CALL, inner_result_type, call->span);
        inner_call->as.call_.fn_binding = fn_binding;
        inner_call->as.call_.args = inner_args;
        inner_call->as.call_.n_args = expected_arity;
        inner_call->as.call_.fn_expr = NULL;
        inner_call->as.call_.dict_arg = NULL;
        inner_call->as.call_.is_poly_call = false;
        inner_call->as.call_.poly_arg_mask = 0;
        /* Create a let-binding for the intermediate closure result */
        char oar_name[32];
        snprintf(oar_name, sizeof(oar_name), "__oar%u", e->next_id++);
        const Symbol *oar_sym = symtab_intern(e->st, strslice(oar_name, (uint32_t)strlen(oar_name)));
        Binding *oar_binding = binding_new(e, oar_sym, inner_result_type, false, false, call->span);
        /* Set closure_fn_binding if the inner result is a closure */
        /* (We don't know at this point, but EX_CLOSURE wrapping in emit handles it dynamically) */
        /* Elaborate remaining args */
        uint32_t n_outer = n_args - expected_arity;
        Expr **outer_args = (Expr **)arena_alloc(e->arena, n_outer * sizeof(Expr *));
        for (uint32_t i = 0; i < n_outer; i++) {
            outer_args[i] = elab_form(e, call->as.list.items[1 + expected_arity + i]);
            if (!outer_args[i]) return NULL;
        }
        /* Result type of outer call: TY_INT as default for opaque fat closures */
        Type outer_result_type = TYPE_INT;
        if (fn_type.as.fn.result_full_type &&
            fn_type.as.fn.result_full_type->kind == TY_FN) {
            outer_result_type = type_from_kind(fn_type.as.fn.result_full_type->as.fn.result_kind);
        }
        Expr *outer_call = expr_new(e->arena, EX_CALL, outer_result_type, call->span);
        outer_call->as.call_.fn_binding = oar_binding;
        outer_call->as.call_.args = outer_args;
        outer_call->as.call_.n_args = n_outer;
        outer_call->as.call_.fn_expr = NULL;
        outer_call->as.call_.dict_arg = NULL;
        outer_call->as.call_.is_poly_call = false;
        outer_call->as.call_.poly_arg_mask = 0;
        /* Wrap in EX_LET */
        LetBinding *oar_let_bs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
        oar_let_bs->binding = oar_binding;
        oar_let_bs->init = inner_call;
        Expr *oar_let = expr_new(e->arena, EX_LET, outer_result_type, call->span);
        oar_let->as.let_.bindings = oar_let_bs;
        oar_let->as.let_.n = 1;
        oar_let->as.let_.body = outer_call;
        return oar_let;
    }
    if (n_args != expected_arity) {
        /* Phase 8: Enhanced arity mismatch diagnostic with error code */
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                            "function '%s' expects %u argument(s), got %u",
                            fn_binding->name->name, expected_arity, n_args);
        return NULL;
    }

    /* Elaborate arguments */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
        TypeKind expected_arg_kind = TY_INT;
        if (fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx = i;
            if (fn_binding->closure_fn_binding) {
                /* Closure thunk arg[0] is hidden env ptr. */
                fn_arg_idx = i + 1;
            }
            expected_arg_kind = fn_type.as.fn.arg_kinds[fn_arg_idx];
        }

        /* Phase HRT1: Detect rank-2 poly param and wrap arg in EX_POLY_WRAP.
         * arg_full_types[fn_arg_idx] is TY_FORALL → this is a rank-2 param. */
        bool is_rank2_param = false;
        if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t fn_arg_idx2 = i;
            if (fn_binding->closure_fn_binding) fn_arg_idx2 = i + 1;
            if (fn_arg_idx2 < fn_type.as.fn.arity) {
                Type *aft = fn_type.as.fn.arg_full_types[fn_arg_idx2];
                /* F1-1: TY_EXISTS-typed params are constrained existential
                 * values, not rank-2 functions; pass the arg through as-is
                 * and let the regular kind check accept the matching
                 * TY_EXISTS argument from `pack`. */
                if (aft && aft->kind == TY_FORALL) {
                    is_rank2_param = true;
                }
            }
        }

        bool arg_ok = (args[i]->type.kind == expected_arg_kind);
        /* Phase HRT/G2: A TY_TYVAR parameter (named type variable like :a) accepts any argument.
         * The concrete type is resolved per-arm inside a GADT match. */
        if (!arg_ok && expected_arg_kind == TY_TYVAR) {
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_FN) {
            /* Allow passing a function value where callback pointer is expected.
             * If this is a rank-2 param, we'll wrap it in a poly wrapper below. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID &&
                (args[i]->type.kind == TY_FORALL || args[i]->type.kind == TY_EXISTS)) {
            /* Allow passing an ascribed poly type (from ::) where rank-2 is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_NIL) {
            /* Allow nil as a null pointer for ptr<void> parameters. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_STRUCT) {
            /* Phase HKT H3: Allow passing an HKT container (TY_STRUCT) where int64_t
             * is expected.  HKT type constructor values are opaque int64_t at runtime. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_ADT) {
            /* Phase G0: ADT values are heap-allocated and passed as int64_t pointers.
             * Allow passing a TY_ADT where int64_t is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_APP) {
            /* Phase HKT §3: Allow passing a partially-applied type (TY_APP) where int64_t
             * is expected.  Partial type application values are opaque int64_t at runtime. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_APP && args[i]->type.kind == TY_ADT) {
            /* Phase HKT/G4: Allow passing a TY_ADT where TY_APP is expected.
             * Both lower to int64_t at runtime.  This arises when a function
             * parameter is annotated with a parameterised type like (Equal a b)
             * (which parses as TY_APP) and the caller passes a GADT constructor
             * value such as Refl (which has type TY_ADT). */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_APP && args[i]->type.kind == TY_TYVAR) {
            /* Phase HKT/G4: Allow passing a TY_TYVAR where TY_APP is expected.
             * Type variables and applied types share int64_t representation. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_FN) {
            /* Phase HKT H3/H4: Allow passing a function value where int64_t is expected.
             * Function references are represented as int64_t in HKT helper calls. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_PTR_VOID) {
            /* Phase HKT §5: Allow passing a capturing closure (heap-allocated env struct,
             * TY_PTR_VOID) where int64_t is expected.  emit.c will apply the
             * (int64_t)(intptr_t) cast so the generated C99 code is valid. */
            arg_ok = true;
        }
        /* IT4: Union type subtyping — accept a value of type A where (A | B) is expected.
         * Wrap the argument with EX_UNION_INJECT so emit.c produces TUR_TAG(idx, val). */
        if (!arg_ok && g_union_types_enabled && expected_arg_kind == TY_UNION) {
            uint32_t fn_arg_idx3 = i;
            if (fn_binding->closure_fn_binding) fn_arg_idx3 = i + 1;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx3 < fn_type.as.fn.arity) {
                Type *union_t = fn_type.as.fn.arg_full_types[fn_arg_idx3];
                if (union_t && union_t->kind == TY_UNION) {
                    for (uint8_t um = 0; um < union_t->as.union_.n_members; um++) {
                        Type *mem = union_t->as.union_.members[um];
                        if (mem && type_eq(args[i]->type, *mem)) {
                            arg_ok = true;
                            /* IT4: wrap in EX_UNION_INJECT to tag the value at runtime */
                            Expr *inject = expr_new(e->arena, EX_UNION_INJECT,
                                                    *union_t, args[i]->span);
                            inject->as.union_inject_.tag_idx = (int64_t)um;
                            inject->as.union_inject_.value = args[i];
                            args[i] = inject;
                            break;
                        }
                    }
                    /* Also accept if the argument is already the same union type (no injection needed) */
                    if (!arg_ok && args[i]->type.kind == TY_UNION) {
                        arg_ok = type_eq(args[i]->type, *union_t);
                    }
                }
            }
        }
        /* IT1: Widening — accept a member type where the expected union matches. */
        if (!arg_ok && g_union_types_enabled && args[i]->type.kind == TY_UNION &&
            expected_arg_kind == TY_UNION) {
            arg_ok = (args[i]->type.kind == expected_arg_kind);
        }
        /* IT3: Intersection elimination — accept a value of intersection type (A & B)
         * where any single member type is expected.  (A & B) <: A and (A & B) <: B. */
        if (!arg_ok && g_intersection_types_enabled &&
            args[i]->type.kind == TY_INTERSECTION) {
            Type *isect_t = &args[i]->type;
            for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                Type *mem = isect_t->as.intersection_.members[im];
                if (mem && mem->kind == expected_arg_kind) {
                    arg_ok = true;
                    break;
                }
            }
        }
        /* IT3: Intersection introduction check — function expects (A & B), arg must
         * satisfy all members.  Emit TUR_E0351 for the first unsatisfied member. */
        if (!arg_ok && g_intersection_types_enabled && expected_arg_kind == TY_INTERSECTION) {
            uint32_t fn_arg_idx5 = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx5 < fn_type.as.fn.arity) {
                Type *isect_t = fn_type.as.fn.arg_full_types[fn_arg_idx5];
                if (isect_t && isect_t->kind == TY_INTERSECTION) {
                    /* Check each member -- the arg must match all of them */
                    bool all_ok = true;
                    const char *first_mismatch = NULL;
                    for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                        Type *mem = isect_t->as.intersection_.members[im];
                        if (!mem) continue;
                        /* For concrete member types, the arg must have an equal or
                         * compatible type.  For typeclass members we cannot yet do
                         * instance resolution here, so skip them. */
                        if (!typekind_is_concrete_for_disjoint(mem->kind)) continue;
                        if (!type_eq(args[i]->type, *mem)) {
                            all_ok = false;
                            first_mismatch = type_name(*mem);
                            break;
                        }
                    }
                    if (all_ok) {
                        arg_ok = true;
                    } else {
                        diag_emit_with_code(DIAG_ERROR, args[i]->span,
                            TUR_E0351_INTERSECTION_MEMBER_MISMATCH,
                            "function '%s' arg %u: value of type %s does not satisfy "
                            "intersection member %s",
                            fn_binding->name->name, i + 1,
                            type_name(args[i]->type),
                            first_mismatch ? first_mismatch : "?");
                        return NULL;
                    }
                }
            }
        }

        /* IT4: A <: any — any value satisfies the top type.
         * Wrap with EX_UNION_INJECT using the TypeKind of the value as the tag,
         * so (type-of) and (cast) can retrieve it at runtime. */
        if (!arg_ok && (g_union_types_enabled || g_intersection_types_enabled) &&
            expected_arg_kind == TY_ANY) {
            arg_ok = true;
            Type any_type; memset(&any_type, 0, sizeof(any_type)); any_type.kind = TY_ANY;
            Expr *inject = expr_new(e->arena, EX_UNION_INJECT, any_type, args[i]->span);
            inject->as.union_inject_.tag_idx = (int64_t)args[i]->type.kind;
            inject->as.union_inject_.value = args[i];
            args[i] = inject;
        }

        /* LT2: When both expected and actual argument types are function types,
         * verify that their arg_linear flags match.  This catches attempts to
         * pass a (-> T R) function where (-> ^linear T R) is required (or vice
         * versa) in higher-order call positions. */
        if (arg_ok && g_linear_enabled &&
            expected_arg_kind == TY_FN && args[i]->type.kind == TY_FN) {
            uint32_t fn_arg_idx_lt2 = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_lt2 < fn_type.as.fn.arity) {
                const Type *expected_fn = fn_type.as.fn.arg_full_types[fn_arg_idx_lt2];
                if (expected_fn && expected_fn->kind == TY_FN &&
                    !fn_type_subtype(args[i]->type, *expected_fn)) {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: linear function type mismatch"
                                        " -- expected %s but got a function with different"
                                        " linearity annotations; ^linear parameters must match exactly",
                                        fn_binding->name->name, i + 1,
                                        type_name(*expected_fn));
                    return NULL;
                }
            }
        }

        if (!arg_ok) {
            /* Phase 8: Enhanced type mismatch with error code */
            /* IT1: Use union-specific error code when union type is involved */
            DiagCode err_code = TUR_E0001_TYPE_MISMATCH;
            if (g_union_types_enabled && (expected_arg_kind == TY_UNION ||
                                           args[i]->type.kind == TY_UNION)) {
                err_code = TUR_E0300_UNION_TYPE_MISMATCH;
            }
            /* Compute expected type name for diagnostic.
             * For compound types (union, intersection) that store their full type in
             * arg_full_types, look it up there so the name includes member types. */
            const char *expected_str;
            if ((expected_arg_kind == TY_UNION || expected_arg_kind == TY_INTERSECTION) &&
                fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                uint32_t fn_arg_idx4 = fn_binding->closure_fn_binding ? i + 1 : i;
                Type *ct = (fn_arg_idx4 < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fn_arg_idx4] : NULL;
                expected_str = ct ? type_name(*ct)
                                  : type_name(type_from_kind(expected_arg_kind));
            } else {
                expected_str = type_name(type_from_kind(expected_arg_kind));
            }
            diag_emit_with_code(DIAG_ERROR, args[i]->span, err_code,
                                "function '%s' arg %u: expected %s, got %s",
                                fn_binding->name->name, i + 1,
                                expected_str,
                                type_name(args[i]->type));
            return NULL;
        }

        /* Phase HRT1/HRT4: wrap rank-2 args with EX_POLY_WRAP + create wrapper thunk.
         * Phase HRT4: also handles TY_PTR_VOID is_poly_fn bindings (pass-through). */
        if (is_rank2_param && (args[i]->type.kind == TY_FN ||
                                args[i]->type.kind == TY_FORALL ||
                                args[i]->type.kind == TY_EXISTS ||
                                args[i]->type.kind == TY_PTR_VOID)) {
            Binding *inner_fn_b = poly_arg_fn_binding(args[i]);
            if (!inner_fn_b) {
                diag_emit(DIAG_ERROR, args[i]->span,
                          "rank-2 argument must be a named function (capturing closures not yet supported)");
                return NULL;
            }
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, args[i]->span);
            wrap->as.poly_wrap_.inner = args[i];
            if (inner_fn_b->is_poly_fn) {
                /* HRT4: pass-through — binding is already a tur_poly_fn_t, no wrapper needed. */
                wrap->as.poly_wrap_.wrapper_binding = NULL;
            } else {
                uint8_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                    ? inner_fn_b->type.as.fn.arity : 1;
                /* Closures have an env param counted in arity — subtract it */
                if (inner_fn_b->closure_fn_binding) inner_arity--;
                Binding *wrapper_b = make_poly_wrapper(e, inner_fn_b, inner_arity, args[i]->span);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            args[i] = wrap;
        }

        /* UT1: TUR_E0200 -- reject aliased value passed to ^unique parameter */
        if (g_unique_enabled && fn_type.kind == TY_FN &&
            i < fn_type.as.fn.arity && fn_type.as.fn.arg_unique[i] &&
            args[i]->kind == EX_VAR) {
            Binding *arg_b = args[i]->as.var.binding;
            if (arg_b->alias_state == AS_ALIASED) {
                if (arg_b->alias_name) {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0200_UNIQUE_ALIASED,
                                        "value '%s' is not unique -- aliased by '%s'",
                                        arg_b->name->name, arg_b->alias_name->name);
                } else {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0200_UNIQUE_ALIASED,
                                        "value '%s' is not unique -- it has been aliased",
                                        arg_b->name->name);
                }
                return NULL;
            }
        }

        /* UT2: Reject argument passed to ^unique ^mut param when active borrows exist.
         * A ^unique ^mut parameter requires exclusive mutable access; any live borrow
         * (&T or &mut T) on the same binding would violate that guarantee. */
        if (g_unique_enabled && fn_type.kind == TY_FN &&
            i < fn_type.as.fn.arity && fn_type.as.fn.arg_unique_mut[i] &&
            args[i]->kind == EX_VAR) {
            Binding *arg_b = args[i]->as.var.binding;
            if (scope_borrow_conflicts(e->scope, arg_b, BK_MUT)) {
                diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                    TUR_E0200_UNIQUE_ALIASED,
                                    "cannot pass '%s' as ^unique ^mut -- active borrow exists",
                                    arg_b->name->name);
                return NULL;
            }
        }

        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it.
         * UT2 semantics:
         *  - ^unique ^mut param: exclusive mutable ACCESS (borrow-like); caller keeps
         *    ownership -- do NOT mark arg as moved, regardless of arg's own flags.
         *  - ^unique param (no ^mut): OWNERSHIP TRANSFER; mark arg as moved so it
         *    can't be used again.
         *  - ordinary param with a ^unique ^mut arg binding: also do not consume,
         *    since the binding is a mutable unique cell that may be freely read. */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            Binding *arg_b2 = args[i]->as.var.binding;
            bool param_is_unique_mut = g_unique_enabled && fn_type.kind == TY_FN &&
                i < fn_type.as.fn.arity && fn_type.as.fn.arg_unique_mut[i];
            bool arg_is_unique_mut = g_unique_enabled && arg_b2->is_unique && arg_b2->is_mut;
            if (!param_is_unique_mut && !arg_is_unique_mut) {
                binding_mark_moved(arg_b2, args[i]->span);
            }
        }
    }

    /* Result type is the function's return type */
    Type result_type;
    if (fn_type.kind == TY_FN) {
        TypeKind result_kind = fn_type.as.fn.result_kind;
        /* Preserve the full return type payload when available.
         * This keeps compound returns such as Session[P], Role[...], and
         * higher-order TY_FN results from collapsing into a bare TypeKind shell. */
        if (fn_type.as.fn.result_full_type) {
            result_type = *fn_type.as.fn.result_full_type;
        } else {
            result_type = type_from_kind(result_kind);
        }
    } else if (fn_type.kind == TY_CONT) {
        /* Calling a continuation returns its result type (though in practice it jumps) */
        result_type = type_from_kind(fn_type.as.cont.returns);
    } else {
        result_type = TYPE_NIL;
    }

    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

/* Phase HRT1: create a poly wrapper thunk for passing a function to a rank-2 param.
 * The wrapper has signature: int64_t __poly_N(void *env, int64_t x0, ..., int64_t x_{arity-1})
 * Its body calls inner_b(x0, ..., x_{arity-1}), ignoring env.
 * Registers the wrapper as a file-level EX_FN_DEF and returns the wrapper Binding. */
Binding *make_poly_wrapper(Elab *e, Binding *inner_b, uint8_t inner_arity, Span span) {
    /* Wrapper name */
    char wname[32];
    snprintf(wname, sizeof(wname), "__poly_%u", e->next_id++);
    const Symbol *wsym = symtab_intern(e->st, strslice(wname, (uint32_t)strlen(wname)));

    /* Wrapper params: env (ptr<void>) + inner_arity int64_t args */
    uint8_t w_arity = inner_arity + 1;
    if (w_arity > MAX_FN_ARITY) {
        diag_emit(DIAG_ERROR, span, "rank-2 wrapper: too many arguments");
        return NULL;
    }
    Binding **wparams = (Binding **)arena_alloc(e->arena, w_arity * sizeof(Binding *));
    Type *wparam_types = (Type *)arena_alloc(e->arena, w_arity * sizeof(Type));

    /* env param */
    char env_pname[40];
    snprintf(env_pname, sizeof(env_pname), "__poly_env_%u", e->next_id++);
    const Symbol *env_psym = symtab_intern(e->st, strslice(env_pname, (uint32_t)strlen(env_pname)));
    Binding *env_pb = binding_new(e, env_psym, TYPE_PTR_VOID, false, false, span);
    wparams[0] = env_pb;
    wparam_types[0] = TYPE_PTR_VOID;

    /* Arg params x0, x1, ... */
    Binding *arg_bs[MAX_FN_ARITY];
    for (uint8_t i = 0; i < inner_arity; i++) {
        char apname[40];
        snprintf(apname, sizeof(apname), "__poly_x%u_%u", i, e->next_id++);
        const Symbol *apsym = symtab_intern(e->st, strslice(apname, (uint32_t)strlen(apname)));
        Binding *apb = binding_new(e, apsym, TYPE_INT, false, false, span);
        wparams[i + 1] = apb;
        wparam_types[i + 1] = TYPE_INT;
        arg_bs[i] = apb;
    }

    /* Build call body: (inner_b x0 x1 ...) */
    TypeKind inner_result_kind = (inner_b->type.kind == TY_FN)
        ? inner_b->type.as.fn.result_kind : TY_INT;
    Expr **call_args = (Expr **)arena_alloc(e->arena, (inner_arity ? inner_arity : 1) * sizeof(Expr *));
    uint32_t call_poly_mask = 0;
    for (uint8_t i = 0; i < inner_arity; i++) {
        Expr *av = expr_new(e->arena, EX_VAR, TYPE_INT, span);
        av->as.var.binding = arg_bs[i];
        call_args[i] = av;
        /* Phase HRT3: if inner_b's param i is a poly fn, the wrapper receives it as int64_t
         * (a pointer to a stack-allocated tur_poly_fn_t). Mark it so emit can dereference. */
        if (inner_b->type.kind == TY_FN && inner_b->type.as.fn.arg_full_types) {
            const Type *aft = inner_b->type.as.fn.arg_full_types[i];
            if (aft && aft->kind == TY_FORALL) {
                call_poly_mask |= (1u << i);
            }
        }
    }
    Expr *call_body = expr_new(e->arena, EX_CALL, type_from_kind(inner_result_kind), span);
    call_body->as.call_.fn_binding = inner_b;
    call_body->as.call_.args = inner_arity > 0 ? call_args : NULL;
    call_body->as.call_.n_args = inner_arity;
    call_body->as.call_.fn_expr = NULL;
    call_body->as.call_.dict_arg = NULL;
    call_body->as.call_.is_poly_call = false;
    call_body->as.call_.poly_arg_mask = call_poly_mask;

    /* Build wrapper fn type */
    TypeKind warg_kinds[MAX_FN_ARITY];
    warg_kinds[0] = TY_PTR_VOID;
    for (uint8_t i = 0; i < inner_arity; i++) warg_kinds[i + 1] = TY_INT;
    Type wfn_type = type_fn(warg_kinds, w_arity, inner_result_kind);

    Binding *wb = binding_new(e, wsym, wfn_type, false, true, span);
    scope_add(&e->global, wb);

    FnDef *wfd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(wfd, 0, sizeof(FnDef));
    wfd->binding = wb;
    wfd->params = wparams;
    wfd->n_params = w_arity;
    wfd->body = call_body;
    wfd->is_variadic = false;
    wfd->closure = NULL;
    wfd->inferred_effect_row = NULL;
    wfd->param_types = wparam_types;
    constraint_set_init(&wfd->constraints);

    Expr *wdef = expr_new(e->arena, EX_FN_DEF, wfn_type, span);
    wdef->as.fn_def_.fn = wfd;
    elab_register_file_def(e, wdef);

    return wb;
}

/* Phase HRT1/HRT4: Helper to extract the underlying fn binding from a poly arg expression.
 * Handles EX_VAR and EX_ASCRIBE(EX_VAR). Returns NULL if not a simple fn ref.
 * Phase HRT4: follows source_binding for let-bound aliases of global functions. */
Binding *poly_arg_fn_binding(Expr *arg) {
    if (arg->kind == EX_VAR) {
        Binding *b = arg->as.var.binding;
        /* For is_poly_fn bindings (already tur_poly_fn_t), return as-is — caller uses passthrough. */
        if (b->is_poly_fn) return b;
        /* Follow source_binding chain to resolve let-bound aliases back to global fns. */
        if (b->source_binding) return b->source_binding;
        return b;
    }
    if (arg->kind == EX_ASCRIBE) return poly_arg_fn_binding(arg->as.ascribe_.inner);
    return NULL;
}

/* Phase HRT1: Elaborate a call through a rank-2 polymorphic function parameter.
 * fn_binding->is_poly_fn is true; the call emits fn_name.fn(fn_name.env, args...) */
static Expr *elab_poly_call(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* Elaborate all arguments normally */
    Expr **args = (Expr **)arena_alloc(e->arena, (n_args ? n_args : 1) * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
    }

    /* Phase HRT3: Detect nested poly-fn args in the body type.
     * If body->arg_full_types[i] is TY_FORALL, wrap that arg with EX_POLY_WRAP
     * and mark it in poly_arg_mask so emit can pass it by pointer. */
    const Type *poly = fn_binding->poly_type;
    uint32_t poly_arg_mask = 0;
    if (poly && poly->kind == TY_FORALL) {
        const Type *pbody = poly->as.forall_.body;
        if (pbody && pbody->kind == TY_FN && pbody->as.fn.arg_full_types) {
            for (uint32_t i = 0; i < n_args && i < (uint32_t)pbody->as.fn.arity; i++) {
                const Type *aft = pbody->as.fn.arg_full_types[i];
                if (aft && aft->kind == TY_FORALL) {
                    /* Arg i is a nested poly fn — wrap it or pass through if already poly fn. */
                    Binding *inner_b = poly_arg_fn_binding(args[i]);
                    if (!inner_b) {
                        diag_emit(DIAG_ERROR, call->as.list.items[1 + i]->span,
                                  "rank-3: polymorphic function argument must be a named function");
                        return NULL;
                    }
                    Expr *orig_arg = args[i];
                    Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig_arg->span);
                    wrap->as.poly_wrap_.inner = orig_arg;
                    if (inner_b->is_poly_fn) {
                        /* HRT4: pass-through — already a tur_poly_fn_t. */
                        wrap->as.poly_wrap_.wrapper_binding = NULL;
                    } else {
                        uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                            ? (uint8_t)inner_b->type.as.fn.arity : 1;
                        Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span);
                        if (!wrapper_b) return NULL;
                        wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                    }
                    args[i] = wrap;
                    poly_arg_mask |= (1u << i);
                }
            }
        }
    }

    /* Determine return type by instantiation.
     * For (forall [a] (-> a a)): result matches first arg's type.
     * For (forall [s] (-> s int)): result is int (concrete). */
    TypeKind result_kind = TY_INT;
    if (poly && poly->kind == TY_FORALL) {
        const Type *body = poly->as.forall_.body;
        if (body && body->kind == TY_FN) {
            const Type *rfull = body->as.fn.result_full_type;
            if (rfull && rfull->kind == TY_STRUCT && rfull->as.struct_.def == NULL) {
                /* Result is a type variable — instantiate from first arg's type */
                result_kind = (n_args > 0 && args[0]) ? args[0]->type.kind : TY_INT;
            } else if (rfull) {
                result_kind = rfull->kind;
            } else {
                result_kind = body->as.fn.result_kind;
            }
        }
    }

    Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(result_kind), call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = n_args > 0 ? args : NULL;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    out->as.call_.dict_arg = NULL;
    out->as.call_.is_poly_call = true;
    out->as.call_.poly_arg_mask = poly_arg_mask;
    return out;
}
