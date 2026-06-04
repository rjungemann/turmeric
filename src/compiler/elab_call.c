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

/* GS5/CS3: shared AbiTypeBinding lives in expr.h so emit can consume what
 * elaboration produced. Keep CallTypeBinding as a local alias for minimal
 * churn in the body of this file. */
typedef AbiTypeBinding CallTypeBinding;

/* TY2.2: Coerce a value expression to the `any` top type by wrapping it in an
 * EX_UNION_INJECT carrying the value's TypeKind as the runtime tag.  Used at
 * every widening site (call args, return position, branch unification) so a
 * narrower value flowing into an `any` slot is boxed exactly once.
 *
 * Carrier-compatible payloads (int/bool/float/nil/cstr/ptr and ADT handles)
 * ride the int64_t carrier directly.  By-value structs cannot, so box_struct
 * is set and codegen emits a heap copy.  Already-`any` values pass through
 * unchanged (no double-boxing).  Returns NULL only on allocation paths that
 * cannot happen (defensive). */
Expr *elab_coerce_to_any(Elab *e, Expr *value) {
    if (!value) return NULL;
    if (value->type.kind == TY_ANY) return value;  /* already boxed */
    Type any_type;
    memset(&any_type, 0, sizeof(any_type));
    any_type.kind = TY_ANY;
    Expr *inject = expr_new(e->arena, EX_UNION_INJECT, any_type, value->span);
    inject->as.union_inject_.tag_idx = (int64_t)value->type.kind;
    inject->as.union_inject_.value = value;
    inject->as.union_inject_.box_struct =
        (value->type.kind == TY_STRUCT) ? value->type.as.struct_.def : NULL;
    return inject;
}

static bool call_type_has_named_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name != NULL;
        case TY_APP:
            return call_type_has_named_tyvar(t->as.app.fn) ||
                   call_type_has_named_tyvar(t->as.app.arg);
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (call_type_has_named_tyvar(t->as.union_.members[i])) return true;
            }
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (call_type_has_named_tyvar(t->as.intersection_.members[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool call_find_type_binding(CallTypeBinding *bindings, uint8_t n_bindings,
                                   const char *name, uint8_t *out_idx) {
    if (!name) return false;
    for (uint8_t i = 0; i < n_bindings; i++) {
        if (bindings[i].name && strcmp(bindings[i].name, name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

/* Walk an applied type down its `fn` spine to the head, returning the head's
 * AdtDef when the spine bottoms out at a TY_ADT (e.g. the head of
 * (type-app (type-app Equal a) b) is the ADT `Equal`). Returns NULL otherwise. */
static const AdtDef *call_app_head_adt(const Type *t) {
    while (t && t->kind == TY_APP) t = t->as.app.fn;
    if (t && t->kind == TY_ADT) return t->as.adt_.def;
    return NULL;
}

static bool call_collect_type_bindings(const Type *expected, Type actual,
                                       CallTypeBinding *bindings, uint8_t *n_bindings) {
    if (!expected) return true;
    switch (expected->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (!expected->as.tyvar_.name) return true;
            if (call_find_type_binding(bindings, *n_bindings, expected->as.tyvar_.name, &idx)) {
                return type_eq(bindings[idx].type, actual);
            }
            if (*n_bindings >= 16) return false;
            bindings[*n_bindings].name = expected->as.tyvar_.name;
            bindings[*n_bindings].type = actual;
            (*n_bindings)++;
            return true;
        }
        case TY_APP:
            if (actual.kind != TY_APP || !expected->as.app.fn || !expected->as.app.arg ||
                !actual.as.app.fn || !actual.as.app.arg) {
                /* KB-022: A bare GADT/ADT value (TY_ADT) is a valid argument for a
                 * parameterised parameter type (TY_APP) when their heads agree --
                 * e.g. (Refl) : Equal passed where (Equal a b) is expected. The
                 * value carries no per-position type arguments to refine the named
                 * tyvars, so accept the head match and leave a/b unbound (the
                 * parameter is polymorphic, so any instantiation is sound). */
                if (actual.kind == TY_ADT) {
                    const AdtDef *exp_head = call_app_head_adt(expected);
                    return exp_head && exp_head == actual.as.adt_.def;
                }
                return false;
            }
            return call_collect_type_bindings(expected->as.app.fn, *actual.as.app.fn,
                                              bindings, n_bindings) &&
                   call_collect_type_bindings(expected->as.app.arg, *actual.as.app.arg,
                                              bindings, n_bindings);
        case TY_UNION:
        case TY_INTERSECTION:
            return type_eq(*expected, actual);
        default:
            return type_eq(*expected, actual);
    }
}

static Type call_instantiate_type(Elab *e, const Type *t,
                                  CallTypeBinding *bindings, uint8_t n_bindings) {
    if (!t) return TYPE_UNKNOWN;
    switch (t->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t->as.tyvar_.name &&
                call_find_type_binding(bindings, n_bindings, t->as.tyvar_.name, &idx)) {
                return bindings[idx].type;
            }
            return *t;
        }
        case TY_APP: {
            Type fn = call_instantiate_type(e, t->as.app.fn, bindings, n_bindings);
            Type arg = call_instantiate_type(e, t->as.app.arg, bindings, n_bindings);
            return type_app(e->arena, fn, arg, (Span){0});
        }
        case TY_UNION: {
            uint8_t n = t->as.union_.n_members;
            Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
            for (uint8_t i = 0; i < n; i++) {
                members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *members[i] = call_instantiate_type(e, t->as.union_.members[i], bindings, n_bindings);
            }
            return type_union_build(e->arena, members, n);
        }
        case TY_INTERSECTION: {
            uint8_t n = t->as.intersection_.n_members;
            Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
            for (uint8_t i = 0; i < n; i++) {
                members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *members[i] = call_instantiate_type(e, t->as.intersection_.members[i], bindings, n_bindings);
            }
            return type_intersection_build(e->arena, members, n);
        }
        default:
            return *t;
    }
}

static Expr *call_wrap_reinterpret(Elab *e, Expr *inner, TypeKind target_kind, Span span) {
    if (!inner) return NULL;
    TypeKind source_kind = inner->type.kind;
    if (source_kind == target_kind) return inner;
    int src_size = type_size_bytes(source_kind);
    int dst_size = type_size_bytes(target_kind);
    if (src_size <= 0 || dst_size <= 0 || src_size != dst_size) return inner;
    Expr *out = expr_new(e->arena, EX_REINTERPRET, type_from_kind(target_kind), span);
    out->as.reinterpret_.expr = inner;
    out->as.reinterpret_.source_kind = source_kind;
    out->as.reinterpret_.target_kind = target_kind;
    return out;
}

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
        /* Phase P3: if we are elaborating the RHS of a ^persistent let binding,
         * lower map-new directly to hamt/new so the result type is void * and
         * subsequent HAMT operations (count, assoc, …) receive the right type. */
        if (e->in_persistent_let) {
            e->needs_hamt = true;
            extern bool g_needs_hamt;
            g_needs_hamt = true;
            return elab_call_hamt_fn(e, call->span, e->sym_hamt_new, 0, NULL);
        }
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
    /* closure_fn_binding describes the underlying thunk signature of a closure
     * VALUE (ptr<void>). When the head is itself a function reference (TY_FN),
     * its own TY_FN type already encodes the signature, and conflating it with
     * returns_closure_fn_binding would make `((curry f) x)` dispatch to the
     * inner fn instead of calling f directly. */
    if (head_kind == TY_PTR_VOID) {
        tmp_b->closure_fn_binding = expr_closure_fn_binding(source_expr);
    } else if (head_kind == TY_FN && source_expr && source_expr->kind == EX_CALL) {
        /* curried-fn-typed-param: the head is the *result of a call* whose
         * static type is itself a function type -- e.g. ((adder 1) 2) where
         * (adder 1) : (fn [int] int).  When the callee genuinely produces a
         * fat closure (its returns_closure_fn_binding is set, as for a defn
         * whose body is a capturing lambda), the runtime value is a heap
         * closure box, not a thin function pointer.  Dispatch the chained
         * application through that closure thunk; otherwise (a call that
         * returns a bare fn reference, e.g. ((pick) 5) -> inc) the head
         * resolves to NULL here and stays a thin pointer call. */
        tmp_b->closure_fn_binding = expr_closure_fn_binding(source_expr);
    }
    if (source_expr && source_expr->kind == EX_VAR && source_expr->as.var.binding) {
        Binding *source_b = source_expr->as.var.binding;
        if (source_b->is_poly_fn) {
            tmp_b->is_poly_fn = true;
            tmp_b->poly_type = source_b->poly_type;
        }
        /* Propagate "returns a closure" so that chained calls through a let
         * binding (let [g ((curry f) x)] (g y)) see g as callable. */
        if (source_b->returns_closure_fn_binding) {
            tmp_b->returns_closure_fn_binding = source_b->returns_closure_fn_binding;
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

/* SZ7: static size checking (-Xsized-types).
 * When a call is `(size-assert-eq! a b)` or `(size-assert-le! a b)` and BOTH
 * size arguments reduce to compile-time constants, decide the relation at
 * compile time: a violation is reported with TUR-E0260 (no runtime check is
 * emitted because compilation fails).  When at least one size is not statically
 * known, returns false so the call elaborates normally and the existing runtime
 * assertion guards it -- the checker never silently accepts (SZ7.3). */
static bool sz7_static_size_violation(Elab *e, const Form *call, const Symbol *name) {
    if (!g_sized_types_enabled) return false;
    const char *fn = name->name;
    bool is_eq = (strcmp(fn, "size-assert-eq!") == 0);
    bool is_le = (strcmp(fn, "size-assert-le!") == 0);
    if (!is_eq && !is_le) return false;
    if (call->as.list.len != 3) return false;  /* (fn a b) */

    SizeTerm *t0 = size_term_from_form(e->arena, call->as.list.items[1], NULL, NULL);
    SizeTerm *t1 = size_term_from_form(e->arena, call->as.list.items[2], NULL, NULL);
    if (!t0 || !t1) return false;
    int64_t v0, v1;
    if (!size_term_eval(t0, &v0) || !size_term_eval(t1, &v1)) return false; /* runtime fallback */

    if (is_eq && v0 != v1) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0260_SIZED_TYPE_MISMATCH,
            "sized type mismatch (TUR-E0260): size %lld is not %lld",
            (long long)v0, (long long)v1);
        return true;
    }
    if (is_le && v0 > v1) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0260_SIZED_TYPE_MISMATCH,
            "sized type mismatch (TUR-E0260): size %lld exceeds upper bound %lld",
            (long long)v0, (long long)v1);
        return true;
    }
    return false;
}

/* SZ8: infer the type-level size index of a sized-GADT constructor application.
 * The index is computed by substituting each operand's already-inferred index
 * into the constructor's declared return-type index template. For example,
 * given `SVCons : int -> (SizedVec n) -> (SizedVec (Add (Static 1) n))`, a call
 * `(SVCons 7 v)` where `v` has inferred index `t` yields `(Add (Static 1) t)`.
 * SVNil's template `(Static 0)` is already closed, seeding the recursion.
 *
 * Returns the inferred SizeTerm (arena-allocated), or NULL when the GADT is not
 * size-indexed or an operand index is unknown (the size stays polymorphic).
 * Inference is purely additive metadata -- erased in codegen. */
static SizeTerm *sz8_infer_ctor_size_index(Elab *e, const CtorDef *ctor,
                                           Expr *call_expr) {
    if (!g_sized_types_enabled || !ctor || !ctor->adt || !ctor->adt->is_gadt)
        return NULL;
    const AdtDef *adt = ctor->adt;
    const Form *rt = ctor->result_type_form;
    if (!rt || rt->tag != F_LIST || rt->as.list.len < 2) return NULL;

    /* Find the size-index parameter position: the first return-type argument
     * that parses as a Size expression. */
    int size_pos = -1;
    SizeTerm *template = NULL;
    for (uint32_t i = 1; i < rt->as.list.len; i++) {
        SizeTerm *st = size_term_from_form(e->arena, rt->as.list.items[i], NULL, NULL);
        if (st) { size_pos = (int)i - 1; template = st; break; }
    }
    if (size_pos < 0 || !template) return NULL;

    /* For each field that is itself a value of the SAME sized GADT, map the
     * field's declared index variable to the argument's inferred index. */
    SizeTerm *result = template;
    uint32_t n_call_args = call_expr->as.call_.n_args;
    for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
        const Form *ff = ctor->field_forms ? ctor->field_forms[fi] : NULL;
        if (!ff || ff->tag != F_LIST || ff->as.list.len < 2) continue;
        const Form *fhd = ff->as.list.items[0];
        if (fhd->tag != F_SYM || strcmp(fhd->as.sym->name, adt->name) != 0) continue;
        if ((uint32_t)(size_pos + 1) >= ff->as.list.len) continue;
        const Form *fidx = ff->as.list.items[size_pos + 1];
        if (fidx->tag != F_SYM) continue;            /* only a bare index var threads */
        const char *fvar = fidx->as.sym->name;
        Expr *arg = call_expr->as.call_.args[fi];
        SizeTerm *arg_idx = (arg && arg->kind == EX_CALL)
                          ? arg->as.call_.size_index : NULL;
        if (!arg_idx) return NULL;                   /* operand unknown -> not inferable */
        result = size_term_subst(e->arena, result, fvar, arg_idx);
    }
    return result;
}

/* SZ8: true when `ctor` is a size-indexed GADT constructor (its return type
 * carries a Size expression in some argument position). */
static bool sz8_ctor_is_sized(Elab *e, const CtorDef *ctor) {
    if (!g_sized_types_enabled || !ctor || !ctor->adt || !ctor->adt->is_gadt)
        return false;
    const Form *rt = ctor->result_type_form;
    if (!rt || rt->tag != F_LIST || rt->as.list.len < 2) return false;
    for (uint32_t i = 1; i < rt->as.list.len; i++)
        if (size_term_from_form(e->arena, rt->as.list.items[i], NULL, NULL))
            return true;
    return false;
}

/* SZ8: --dump-sizes -- emit one line per size-indexed constructor application.
 * A folded constant prints as the number; an open term prints symbolically;
 * an un-inferable index (an operand whose size is unknown) prints as `?`. */
static void sz8_dump_ctor_size(Elab *e, const CtorDef *ctor,
                               const SizeTerm *inferred) {
    if (!g_dump_sizes || !sz8_ctor_is_sized(e, ctor)) return;
    char sbuf[128];
    int64_t k;
    if (inferred && size_term_eval(inferred, &k))
        fprintf(stderr, "size: %s : (%s %lld)\n",
                ctor->name, ctor->adt->name, (long long)k);
    else if (inferred)
        fprintf(stderr, "size: %s : (%s %s)\n", ctor->name, ctor->adt->name,
                size_term_to_string(inferred, sbuf, sizeof(sbuf)));
    else
        fprintf(stderr, "size: %s : (%s ?)\n", ctor->name, ctor->adt->name);
}

/* Phase GHE1: does `name` name a method of some registered typeclass?
 * Used to route a bare-name method call (hash x) / (eq? a b) to the same
 * argument-type dispatch the dotted (.method ...) form performs, but only
 * when no ordinary binding (user defn or local shadow) claims the name. */
static bool elab_name_is_typeclass_method(Elab *e, const Symbol *name) {
    if (!name) return false;
    for (TypeClass *c = e->typeclass_env.typeclasses; c != NULL; c = c->next) {
        for (uint8_t mi = 0; mi < c->n_methods; mi++) {
            const Symbol *mn = c->methods[mi].name;
            if (mn && mn->len == name->len &&
                memcmp(mn->name, name->name, name->len) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Phase R6b: true if [p, p+n) contains the substring `needle`. */
static bool lint_line_has_marker(const char *p, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return false;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(p + i, needle, m) == 0) return true;
    }
    return false;
}

/* Phase R6b: --lint-panic allow-list. A `;; #lint-panic-allow` comment in the
 * file's leading comment block silences the whole file; the same comment on
 * the line immediately preceding a call silences just that call. Returns true
 * if the call at `span` is allow-listed. */
static bool lint_panic_allowed(Span span) {
    const SourceFile *f = diag_source_file(span.file_id);
    if (!f || !f->src) return false;
    const char *src = f->src;
    size_t len = f->len;
    static const char marker[] = "#lint-panic-allow";

    /* File-level: scan the leading run of blank/comment lines. If the marker
     * appears before the first code line, the whole file is allow-listed. */
    {
        size_t i = 0;
        while (i < len) {
            size_t ls = i;
            while (i < len && src[i] != '\n') i++;
            size_t le = i;
            if (i < len) i++;
            size_t s = ls;
            while (s < le && (src[s] == ' ' || src[s] == '\t')) s++;
            if (s == le) continue;            /* blank line */
            if (src[s] == ';') {              /* comment line */
                if (lint_line_has_marker(src + s, le - s, marker)) return true;
                continue;
            }
            break;                            /* first code line -> stop */
        }
    }

    /* Per-call: examine the immediately-preceding non-blank line. */
    {
        size_t pos = span.off_start;
        if (pos > len) pos = len;
        while (pos > 0 && src[pos - 1] != '\n') pos--;  /* start of call's line */
        while (pos > 0) {
            size_t end = pos - 1;             /* '\n' ending the previous line */
            size_t ls = end;
            while (ls > 0 && src[ls - 1] != '\n') ls--;
            size_t s = ls;
            while (s < end && (src[s] == ' ' || src[s] == '\t')) s++;
            if (s == end) { pos = ls; continue; }   /* blank -> keep looking up */
            if (src[s] == ';')
                return lint_line_has_marker(src + s, end - s, marker);
            return false;                     /* non-comment code line -> no allow */
        }
    }
    return false;
}

/* Phase R6b: panic-site names flagged by --lint-panic. */
static bool lint_is_panic_site(const char *nm, bool *is_unwrap_out) {
    bool is_unwrap = (strcmp(nm, "result-unwrap") == 0 ||
                      strcmp(nm, "option-unwrap") == 0);
    *is_unwrap_out = is_unwrap;
    return is_unwrap ||
        strcmp(nm, "panic") == 0          || strcmp(nm, "tur_panic") == 0 ||
        strcmp(nm, "assert!") == 0        || strcmp(nm, "assert-msg!") == 0 ||
        strcmp(nm, "require!") == 0       || strcmp(nm, "require-msg!") == 0 ||
        strcmp(nm, "ensure!") == 0        || strcmp(nm, "ensure-msg!") == 0 ||
        strcmp(nm, "invariant!") == 0     || strcmp(nm, "invariant-msg!") == 0;
}

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

    /* Phase C2: --no-contracts strips contract checks before their arguments
     * are elaborated, so the predicate expression (and any side effects it
     * carries) never run -- matching the Rust/C `assert` convention. The
     * `assert!`/`require!`/`ensure!`/`invariant!` macros expand to calls to
     * `tur-contract-check` / `tur-contract-check-inv`; we drop those calls
     * here and fold `contract-enabled?` to `false`. */
    if (g_no_contracts) {
        const Symbol *cc  = symtab_intern(e->st, strslice("tur-contract-check", 18));
        const Symbol *cci = symtab_intern(e->st, strslice("tur-contract-check-inv", 22));
        const Symbol *ce  = symtab_intern(e->st, strslice("contract-enabled?", 17));
        if (name == cc || name == cci) {
            /* Void no-op: contract checks are `:void`, which lowers to TY_NIL. */
            return expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
        }
        if (name == ce && call->as.list.len == 1) {
            Expr *f = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, call->span);
            f->as.b = false;
            return f;
        }
    }

    /* Phase R6b: --lint-panic warns at panic call sites (panic/tur_panic, the
     * contract macros, and result-unwrap/option-unwrap) unless allow-listed by
     * a `;; #lint-panic-allow` comment. The macro names are still visible here
     * because macro expansion happens later in this function. result-unwrap /
     * option-unwrap carry a soft-deprecation hint toward *-must (OQ#1). */
    if (g_lint_panic) {
        bool is_unwrap = false;
        if (lint_is_panic_site(name->name, &is_unwrap) &&
            !lint_panic_allowed(call->span)) {
            if (is_unwrap) {
                diag_emit_with_code(DIAG_WARNING, call->span, TUR_W0038_LINT_PANIC_SITE,
                    "panic call site '%s' outside allow-list; "
                    "prefer result-must / option-must", name->name);
            } else {
                diag_emit_with_code(DIAG_WARNING, call->span, TUR_W0038_LINT_PANIC_SITE,
                    "panic call site '%s' outside allow-list", name->name);
            }
        }
    }

    /* SZ7: static size checking -- reject statically-known size mismatches at
     * compile time before normal call dispatch. */
    if (sz7_static_size_violation(e, call, name)) return NULL;

    /* Special forms. */
    if (name == e->sym_def)    return elab_def   (e, call);
    if (name == e->sym_define) return elab_define_error(e, call);
    if (name == e->sym_let)    return elab_let   (e, call);
    if (name == e->sym_letstar) return elab_letstar(e, call);
    if (name == e->sym_letrec) return elab_letrec(e, call);
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
    /* FH2: (handler (E [params] k) body) in value position is a handler literal.
     * A local binding named `handler` (e.g. a higher-order function parameter)
     * shadows the special form and is dispatched as an ordinary call. */
    if (name == e->sym_handler_type && !scope_lookup(e->scope, name))
        return elab_handler_lit(e, call);
    /* FH3: (with-handler hv body) -- exactly two args -- applies a handler value.
     * Any other arity is the T25 inline-handle sugar (body + case/body pairs). */
    if (name == e->sym_with_handler) {
        if (call->as.list.len == 3) return elab_with_handler(e, call);
        return elab_handle(e, call);  /* T25: sugar for handle in async context */
    }
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
    if (name == e->sym_is_q)    return elab_is_q(e, call);
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

    /* TMS3 (typed-map-surface-plan): hamt-of is now the single typed Map
     * builder and dispatches string keys through Hash[cstr]/MapKey[cstr] by
     * content, so the historical (hamt-of "k" ...) -> smap-of rewrite is no
     * longer needed -- string and int keys share one lowering. */

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

    /* Phase RT: return-type-directed dispatch for a typeclass method whose
     * dispatch variable appears only in its return type (e.g. (default-of),
     * (schema-of)).  Such methods cannot be resolved from arguments; the
     * instance is selected from the expected-type channel.  These methods did
     * not exist before tyvar-return parsing was added, so intercepting them
     * here cannot regress existing programs. */
    {
        bool rt_handled = false;
        Expr *rt = elab_try_return_dispatch(e, call, name, &rt_handled);
        if (rt_handled) return rt;
    }

    /* Phase GHE1: bare-name typeclass method dispatch.  A head symbol that
     * names a registered typeclass method but resolves to no binding (no user
     * defn and no local shadow) is routed to argument-type dispatch, exactly as
     * the dotted (.method ...) form would be: the first argument is the
     * receiver, and its static type selects the instance.  This lets bare
     * (hash x), (eq? a b), (show v) pick the right instance instead of falling
     * through to the eval-mode native fallback (which types them :int).
     *
     * Gated on `!fn_binding` so a user defn or local binding of the same name
     * always wins, and on class membership so a program that never declared
     * the class is never intercepted -- a genuinely-unbound symbol still flows
     * to its original unbound-symbol / eval-native handling.  Return-only
     * dispatch methods were already handled above; argument-dispatched methods
     * always carry their dispatch type variable in the first parameter for the
     * stdlib classes (Eq/Hash/Show/Num/Functor/...). */
    if (!fn_binding && call->as.list.len >= 2 &&
        elab_name_is_typeclass_method(e, name)) {
        char dotbuf[160];
        int dotlen = snprintf(dotbuf, sizeof(dotbuf), ".%s", name->name);
        if (dotlen > 0 && (size_t)dotlen < sizeof(dotbuf)) {
            const Symbol *dot_sym =
                symtab_intern(e->st, strslice(dotbuf, (uint32_t)dotlen));
            uint32_t n_items = call->as.list.len;
            Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
            items[0] = form_sym(e->arena, head->span, dot_sym);
            for (uint32_t i = 1; i < n_items; i++) items[i] = call->as.list.items[i];
            Form *dotcall = form_list(e->arena, call->span, items, n_items);
            return elab_method_call(e, dotcall);
        }
    }

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
            /* SZ8: a nullary sized-GADT constructor seeds the size index from its
             * (constant) return-type template, e.g. SVNil : (SizedVec (Static 0)). */
            out->as.call_.ctor = ctor;
            SizeTerm *inferred = sz8_infer_ctor_size_index(e, ctor, out);
            out->as.call_.size_index = inferred;
            sz8_dump_ctor_size(e, ctor, inferred);
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

                /* SZ8: record the CtorDef and infer the type-level size index of
                 * this constructed value (sized GADTs only; erased in codegen). */
                call_expr->as.call_.ctor = ctor;
                SizeTerm *inferred = sz8_infer_ctor_size_index(e, ctor, call_expr);
                call_expr->as.call_.size_index = inferred;
                sz8_dump_ctor_size(e, ctor, inferred);

                /* TP5: intra-constructor type-arg consistency check.
                 * For each field whose full_type is a named TY_TYVAR, record
                 * the concrete argument type.  If a later field binds the same
                 * param to a different type, emit a diagnostic. */
                struct { const char *name; Type type; } param_bindings[8];
                uint8_t  n_bound = 0;
                uint32_t n_call_args = call_expr->as.call_.n_args;
                for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
                    const Type *ft = ctor->fields[fi].full_type;
                    if (!ft || ft->kind != TY_TYVAR || !ft->as.tyvar_.name) continue;
                    const char *pname = ft->as.tyvar_.name;
                    /* TS4P1: If the argument was wrapped in EX_REINTERPRET (boxing a
                     * concrete type like float into int64_t for the TY_INT carrier),
                     * use the source type (before the reinterpret) as the concrete type.
                     * This ensures we bind e.g. `a -> float` instead of `a -> int`. */
                    const Expr *arg_expr = call_expr->as.call_.args[fi];
                    while (arg_expr && arg_expr->kind == EX_REINTERPRET &&
                           arg_expr->as.reinterpret_.target_kind == TY_INT &&
                           arg_expr->as.reinterpret_.expr) {
                        arg_expr = arg_expr->as.reinterpret_.expr;
                    }
                    Type concrete = arg_expr ? arg_expr->type : call_expr->as.call_.args[fi]->type;
                    bool found = false;
                    for (uint8_t bi = 0; bi < n_bound; bi++) {
                        if (strcmp(param_bindings[bi].name, pname) == 0) {
                            Type prev = param_bindings[bi].type;
                            bool mismatch = (prev.kind != concrete.kind);
                            if (!mismatch && concrete.kind == TY_ADT)
                                mismatch = (prev.as.adt_.def != concrete.as.adt_.def);
                            if (mismatch) {
                                diag_emit(DIAG_ERROR,
                                    call->as.list.items[1 + fi]->span,
                                    "constructor '%s': type parameter '%s' was bound to "
                                    "'%s' by an earlier field but argument %u has type '%s'",
                                    ctor->name, pname,
                                    type_name(prev),
                                    fi, type_name(concrete));
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found && n_bound < 8) {
                        param_bindings[n_bound].name = pname;
                        param_bindings[n_bound].type = concrete;
                        n_bound++;
                    }
                }

                /* TS4P1: Build TY_APP result type for per-use-site ADT monomorphisation.
                 * If all of the ADT's type parameters were bound to concrete types
                 * by the argument list (TP5 above), upgrade the result type from
                 * a plain TY_ADT to a TY_APP chain so the codegen can emit the
                 * correctly-typed monomorphised struct and constructor. */
                if (n_bound > 0 && ctor->adt->n_type_params > 0 &&
                    !ctor->adt->is_gadt &&
                    ctor->adt->n_type_params <= 8) {
                    bool all_bound = true;
                    Type adt_base = type_adt(ctor->adt);
                    adt_base.hkt_kind = kind_for_arity(ctor->adt->n_type_params);
                    Type app_type = adt_base;
                    for (uint8_t pi = 0;
                         pi < ctor->adt->n_type_params && all_bound; pi++) {
                        const char *pname = ctor->adt->type_params[pi];
                        bool found = false;
                        for (uint8_t bi = 0; bi < n_bound; bi++) {
                            if (param_bindings[bi].name &&
                                strcmp(param_bindings[bi].name, pname) == 0) {
                                if (param_bindings[bi].type.kind == TY_TYVAR) {
                                    all_bound = false;
                                } else {
                                    app_type = type_app(e->arena, app_type,
                                                        param_bindings[bi].type,
                                                        call->span);
                                }
                                found = true;
                                break;
                            }
                        }
                        if (!found) all_bound = false;
                    }
                    if (all_bound) {
                        call_expr->type = app_type;
                    }
                }
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
        } else if (e->separate_compilation || !g_interpret_mode) {
            /* UCH1 (diagnose-unbound-call-heads-plan): in any compiled path an
             * unknown call head is a genuine unbound reference (a typo, or a
             * missing import / extern-c).  Report it here instead of silently
             * typing the call :int -- which previously let `tur check` pass and
             * deferred the failure to a cryptic C-compiler "undeclared" error,
             * or surfaced as a misleading downstream type mismatch.  The
             * runtime-dispatch fallback below is reserved for interpret mode
             * (eval / --interpret / repl / worker), where TuriEnv natives are
             * resolved at runtime. */
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

            if (typekind_is_numeric(args[i]->type.kind) &&
                       typekind_is_numeric(spec->arg_type.kind)) {
                /* Phase B: TUR-E0042 -- no implicit coercion between distinct numeric kinds */
                diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0042_MIXED_WIDTH_ARITH,
                                    "mixed-width numeric arithmetic: '%s' arg %u is %s, expected %s",
                                    name->name, i + 1, actual_str, expected_str);
                diag_emit(DIAG_HELP, args[i]->span,
                          "use (as %s expr) for explicit numeric conversion", expected_str);
            } else {
                diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0001_TYPE_MISMATCH,
                                    "'%s' arg %u: type mismatch - expected %s, got %s",
                                    name->name, i + 1, expected_str, actual_str);
                if (args[i]->type.kind == TY_BOOL && spec->arg_type.kind == TY_INT) {
                    diag_emit(DIAG_HELP, args[i]->span, "try wrapping the bool in (if x 1 0)");
                }
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

    /* CY4: helper -- return the full type for original-arg user-slot `idx`
     * (0-based, env-stripped). NULL if the slot is monomorphic. */
    Type *const *src_full_types = fn_type.as.fn.arg_full_types;
    #define PAP_SLOT_FULL(idx) \
        (src_full_types ? src_full_types[fn_is_closure ? ((idx) + 1) : (idx)] : NULL)

    /* Build capture bindings for provided args */
    Binding **cap_bindings = (Binding **)arena_alloc(e->arena, n_provided * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        char cap_name[32];
        snprintf(cap_name, sizeof(cap_name), "__papc%u", e->next_id++);
        const Symbol *cap_sym = symtab_intern(e->st, strslice(cap_name, (uint32_t)strlen(cap_name)));
        /* arg type from fn_type: skip index 0 if closure (env), so index = i+1 if closure, else i */
        TypeKind cap_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (i + 1) : i];
        /* Type-check captured (partial-application) args against the slot they
         * fill.  Mirror the saturated positional check, which for a
         * struct/opaque/ADT parameter is strict: the captured argument must be
         * the *same* nominal type -- not merely the same TypeKind, and not a
         * value of a differing kind (e.g. a bare int) at all.  Two failure
         * modes are folded together here:
         *   - kind-level: a plain int (TY_INT) captured at a TY_STRUCT opaque
         *     slot -- differing kinds.  Without this, `(two 5)` -- binding an
         *     int into a :A slot -- slips through because the capture loop never
         *     compared the provided arg's type to the parameter at all.
         *   - nominal-identity: a :B captured at a :A slot -- same kind,
         *     different nominal.  The saturated path only re-checks the
         *     *remaining* params, so the captured slot must be validated here.
         * See docs/reported/partial-application-skips-captured-arg-type-check.md
         * and docs/upcoming/positional-nominal-type-identity-fix-plan.md. */
        {
            Type *cap_full_chk = PAP_SLOT_FULL(i);
            bool slot_is_nominal =
                (cap_full_chk &&
                 (cap_full_chk->kind == TY_STRUCT || cap_full_chk->kind == TY_ADT)) ||
                cap_kind == TY_STRUCT || cap_kind == TY_ADT;
            if (slot_is_nominal) {
                /* Prefer the recorded full type for an exact nominal compare;
                 * fall back to a kind-level compare when it is unavailable. */
                bool mismatch;
                Type expected_ty;
                if (cap_full_chk &&
                        (cap_full_chk->kind == TY_STRUCT || cap_full_chk->kind == TY_ADT)) {
                    mismatch = !type_eq(elab_args[i]->type, *cap_full_chk);
                    expected_ty = *cap_full_chk;
                } else {
                    mismatch = (elab_args[i]->type.kind != cap_kind);
                    expected_ty = type_from_kind(cap_kind);
                }
                if (mismatch) {
                    Buf eb; buf_init(&eb);
                    type_print(&eb, expected_ty); buf_putc(&eb, '\0');
                    Buf ab; buf_init(&ab);
                    type_print(&ab, elab_args[i]->type); buf_putc(&ab, '\0');
                    diag_emit_with_code(DIAG_ERROR, elab_args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: expected %s, got %s",
                                        fn_binding->name->name, i + 1, eb.data, ab.data);
                    buf_free(&eb); buf_free(&ab);
                    return NULL;
                }
            }
        }
        Type cap_type = type_from_kind(cap_kind);
        /* A5: a captured struct/ADT slot must carry its *full* nominal type, not
         * the kind-erased TY_STRUCT/TY_ADT.  Otherwise the env field is emitted
         * as int64_t (type_c_name of a nameless struct kind), the let-binding
         * init truncates the struct value, and the inner call passes an int64_t
         * where the callee expects the nominal struct -- a hard C compile error.
         * See docs/upcoming/stdlib-type-erasure-cleanup-plan.md (A5). */
        {
            Type *cap_full_t = PAP_SLOT_FULL(i);
            if (cap_full_t &&
                    (cap_full_t->kind == TY_STRUCT || cap_full_t->kind == TY_ADT)) {
                cap_type = *cap_full_t;
            }
        }
        Binding *cap_b = binding_new(e, cap_sym, cap_type, false, false, call->span);
        /* CY4: rank-2 captured arg -- carry forall info onto the binding so
         * the closure env field is emitted as tur_poly_fn_t. */
        Type *cap_full = PAP_SLOT_FULL(i);
        if (cap_full && cap_full->kind == TY_FORALL) {
            cap_b->is_poly_fn = true;
            cap_b->poly_type = cap_full;
        }
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
        /* CY4: rank-2 remaining param -- mark so the thunk's C signature uses
         * tur_poly_fn_t for that slot. */
        Type *rem_full = PAP_SLOT_FULL(n_provided + i);
        if (rem_full && rem_full->kind == TY_FORALL) {
            rem_b->is_poly_fn = true;
            rem_b->poly_type = rem_full;
        }
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
        /* CY4: rank-2 capture is already stored as tur_poly_fn_t; pass through. */
        if (cap_bindings[i]->is_poly_fn) {
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, call->span);
            wrap->as.poly_wrap_.inner = var;
            wrap->as.poly_wrap_.wrapper_binding = NULL;
            wrap->as.poly_wrap_.is_closure = false;
            call_args[i] = wrap;
        } else {
            call_args[i] = var;
        }
    }
    for (uint32_t i = 0; i < n_remaining; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, rem_params[i]->type, call->span);
        var->as.var.binding = rem_params[i];
        /* CY4: rank-2 remaining param arrives wrapped as tur_poly_fn_t; pass through. */
        if (rem_params[i]->is_poly_fn) {
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, call->span);
            wrap->as.poly_wrap_.inner = var;
            wrap->as.poly_wrap_.wrapper_binding = NULL;
            wrap->as.poly_wrap_.is_closure = false;
            call_args[n_provided + i] = wrap;
        } else {
            call_args[n_provided + i] = var;
        }
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

    /* CY4: Propagate effect_row from the original function. A partial
     * application of an effectful function must retain the same effect row so
     * the effect-check pass and any later call sites see the effects when the
     * resulting closure is invoked. */
    thunk_type.as.fn.effect_row = fn_type.as.fn.effect_row;

    /* CY4: Propagate arg_full_types and result_full_type for the remaining
     * parameters. This preserves rank-2 polymorphic parameter types and any
     * non-scalar full type info so the resulting closure can still accept
     * forall-typed arguments. */
    if (fn_type.as.fn.arg_full_types) {
        Type **rem_full = (Type **)arena_alloc(e->arena, thunk_n_params * sizeof(Type *));
        rem_full[0] = NULL; /* env */
        for (uint32_t i = 0; i < n_remaining; i++) {
            uint32_t src_idx = fn_is_closure ? (n_provided + 1 + i) : (n_provided + i);
            rem_full[1 + i] = fn_type.as.fn.arg_full_types[src_idx];
        }
        thunk_type.as.fn.arg_full_types = rem_full;
    }
    if (fn_type.as.fn.result_full_type) {
        thunk_type.as.fn.result_full_type = fn_type.as.fn.result_full_type;
    }

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
        /* CY4: When the captured slot is rank-2, wrap the user's argument so
         * the value stored in the env is a tur_poly_fn_t. Pass-through if the
         * argument is itself already a poly fn binding. */
        if (cap_bindings[i]->is_poly_fn) {
            Binding *inner_fn_b = poly_arg_fn_binding(elab_args[i]);
            if (!inner_fn_b) {
                diag_emit(DIAG_ERROR, elab_args[i]->span,
                          "rank-2 argument must be a named function (capturing closures not yet supported)");
                return NULL;
            }
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, elab_args[i]->span);
            wrap->as.poly_wrap_.inner = elab_args[i];
            wrap->as.poly_wrap_.is_closure = false;
            if (inner_fn_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL;
            } else {
                uint8_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                    ? inner_fn_b->type.as.fn.arity : 1;
                if (inner_fn_b->closure_fn_binding) inner_arity--;
                Binding *wrapper_b = make_poly_wrapper(e, inner_fn_b, inner_arity, elab_args[i]->span);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            let_bs[i].init = wrap;
        }
    }
    #undef PAP_SLOT_FULL

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
    } else if (fn_binding->type.kind == TY_PTR_VOID && !fn_binding->is_fat) {
        /* CRU B-4: retire the :ptr<void>-as-closure overload.  A *raw*
         * :ptr<void> (not a fat sink) is a plain pointer, not a callable
         * closure -- calling it directly is the representation-split hazard
         * (report ptr-void-direct-call-representation-split).  Closures are now
         * boxed TY_FN (B-1); a fat-closure parameter must be spelled ^fat (or
         * ^fat :(fn [...] :T)).  This makes :ptr<void> raw-pointer-only again. */
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' has type :ptr<void> (a raw pointer), which is not "
                  "directly callable; declare it as a fat closure parameter "
                  "(^fat %s, or ^fat %s :(fn [...] :T)) to call it",
                  fn_binding->name->name, fn_binding->name->name,
                  fn_binding->name->name);
        return NULL;
    } else if (fn_binding->type.kind == TY_PTR_VOID) {
        /* CY2: fat-closure dynamic dispatch through a ^fat :ptr<void> sink.
         * Supports 0-arg and n-arg fat-closure calls (emit_expr.c reads slot 0
         * of the box for all arities). */
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

    /* CC4 (cps-transform-plan): (k v) application sugar for a cloneable
     * continuation. A call through a cont-typed binding desugars to a resume of
     * the cloneable continuation handle -- what the surface previously had to
     * spell as (tur_cloneable_cont_resume k v). The handle is carried as an
     * int64_t (see type_c_name TY_CONT), so the resume builtin consumes it
     * directly. */
    if (fn_type.kind == TY_CONT) {
        if (n_args != 1) {
            diag_emit(DIAG_ERROR, call->span,
                      "continuation '%s' takes exactly one argument (the resume value)",
                      fn_binding->name->name);
            return NULL;
        }
        Expr *karg = elab_form(e, call->as.list.items[1]);
        if (!karg) return NULL;
        /* CC4.4: (k v) consumes the continuation.  This sugar builds the EX_VAR
         * by hand (below), bypassing the shared var-use consumption path, so
         * account for linearity here: invoking a ^linear k marks it consumed, and
         * a second invocation is a use-after-consume (TUR-E0101). */
        if (g_linear_enabled && fn_binding->is_linear) {
            if (fn_binding->is_linear_consumed) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                                    TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                                    "linear value '%s' used after being consumed",
                                    fn_binding->name->name);
                return NULL;
            }
            fn_binding->is_linear_consumed = true;
        }
        /* The handle, viewed as its int64 carrier so the resume builtin types. */
        Expr *kvar = expr_new(e->arena, EX_VAR, TYPE_INT, call->span);
        kvar->as.var.binding = fn_binding;
        /* CC4: dispatch to the resume runtime selected by the cont flavor. */
        const char *resume_name;
        switch ((ContFlavor)fn_type.as.cont.flavor) {
            case CONT_ESCAPE: resume_name = "tur_escape_resume"; break;
            case CONT_SERIAL: resume_name = "tur_serial_cont_resume"; break;
            case CONT_CLONEABLE:
            default:          resume_name = "tur_cloneable_cont_resume"; break;
        }
        const BuiltinSpec *rspec =
            builtin_first_with_name(intern_cstr(e->st, resume_name));
        if (!rspec) {
            diag_emit(DIAG_ERROR, call->span,
                      "internal: continuation resume builtin missing");
            return NULL;
        }
        TypeKind res_kind = (fn_type.as.cont.returns != TY_UNKNOWN)
                            ? fn_type.as.cont.returns : TY_INT;
        /* The resume builtins are int64-carried. For a value-typed cont<T> with
         * T != int (e.g. cstr), bit-cast the resume value into the int carrier on
         * the way in and bit-cast the int result back to T on the way out, so the
         * emitted C is clean (no -Wint-conversion). */
        Expr *karg_c = call_wrap_reinterpret(e, karg, TY_INT, call->span);
        Expr **bargs = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        bargs[0] = kvar;
        bargs[1] = karg_c;
        Expr *out = expr_new(e->arena, EX_BUILTIN, TYPE_INT, call->span);
        out->as.builtin.spec = rspec;
        out->as.builtin.args = bargs;
        out->as.builtin.n = 2;
        if (res_kind != TY_INT)
            out = call_wrap_reinterpret(e, out, res_kind, call->span);
        return out;
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

    /* AR7: For variadic functions the rest param counts as one fixed slot;
     * partial application is allowed up to n_required (= arity - 1) args,
     * and any call with n_args >= n_required dispatches variadically. */
    bool fn_is_variadic = (fn_type.kind == TY_FN && fn_type.as.fn.is_variadic);
    uint8_t n_required = (fn_is_variadic && expected_arity > 0)
                         ? (uint8_t)(expected_arity - 1)
                         : expected_arity;

    if (n_args < n_required && fn_type.kind == TY_FN) {
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
    /* AR8: Variadic dispatch -- build a cons list for surplus args */
    if (fn_is_variadic && (uint32_t)n_args >= (uint32_t)n_required) {
        uint32_t n_rest = n_args - n_required;
        uint32_t n_call_args = n_required + 1;  /* fixed args + the cons-list arg */
        Expr **call_args = (Expr **)arena_alloc(e->arena,
                            (n_call_args ? n_call_args : 1) * sizeof(Expr *));
        /* Elaborate fixed args */
        for (uint32_t i = 0; i < n_required; i++) {
            call_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!call_args[i]) return NULL;
        }
        /* Build cons-list expression for rest args */
        Expr *rest_expr;
        if (n_rest == 0) {
            rest_expr = expr_new(e->arena, EX_INT_LIT, TYPE_INT, call->span);
            rest_expr->as.i = 0;  /* nil = 0 */
        } else {
            Expr **rest_items = (Expr **)arena_alloc(e->arena, n_rest * sizeof(Expr *));
            TypeKind rk = fn_type.as.fn.rest_kind;
            /* Typed-variadic: when the rest element is a user-defined type the
             * declaration carries its full Type so we can compare identity
             * (struct/ADT def pointer, applied type args) rather than only the
             * coarse TypeKind.  Primitive rest keeps rest_full_type == NULL and
             * uses the fast TypeKind path below. */
            Type *rest_full = fn_type.as.fn.rest_full_type;
            bool rest_err = false;
            /* Homogeneity: a polymorphic-tyvar rest (`[& xs :A]`) names a
             * single type variable A, so all rest args must share one type.
             * Bind A to the first rest arg and require the rest to match it. */
            Type tyvar_first;
            bool tyvar_first_set = false;
            for (uint32_t i = 0; i < n_rest; i++) {
                rest_items[i] = elab_form(e, call->as.list.items[1 + n_required + i]);
                if (!rest_items[i]) return NULL;
                /* AR10: type-check each rest arg against the declared rest element type */
                TypeKind ak = rest_items[i]->type.kind;
                bool rest_ok;
                if (rest_full) {
                    /* Full-type comparison for user-defined rest (opaque /
                     * struct / ADT / type application). */
                    rest_ok = type_eq(rest_items[i]->type, *rest_full);
                } else {
                    rest_ok = (ak == rk);
                    /* Polymorphic rest element (TY_TYVAR): A is one type
                     * variable, so all rest args must unify to a single type.
                     * Bind A to the first arg; compare the rest by identity. */
                    if (!rest_ok && rk == TY_TYVAR) {
                        if (!tyvar_first_set) {
                            tyvar_first = rest_items[i]->type;
                            tyvar_first_set = true;
                            rest_ok = true;
                        } else {
                            rest_ok = type_eq(rest_items[i]->type, tyvar_first);
                        }
                    }
                }
                if (!rest_ok) {
                    const char *fn_name = (fn_binding && fn_binding->name) ? fn_binding->name->name : "?";
                    const char *expected = rest_full
                        ? type_name(*rest_full)
                        : (rk == TY_TYVAR && tyvar_first_set
                            ? type_name(tyvar_first)
                            : typekind_to_string(rk));
                    const char *got = type_name(rest_items[i]->type);
                    diag_emit(DIAG_ERROR,
                              call->as.list.items[1 + n_required + i]->span,
                              "variadic call to '%s': rest arg %u has wrong type "
                              "(expected %s, got %s)",
                              fn_name, i, expected, got);
                    /* Keep checking the remaining rest args so every mismatch
                     * is reported in one pass, then fail. */
                    rest_err = true;
                }
            }
            if (rest_err) return NULL;
            rest_expr = expr_new(e->arena, EX_CONS_LIST, TYPE_INT, call->span);
            rest_expr->as.cons_list_.items = rest_items;
            rest_expr->as.cons_list_.n = n_rest;
            rest_expr->as.cons_list_.item_kind = fn_type.as.fn.rest_kind;
        }
        call_args[n_required] = rest_expr;
        /* Determine result type */
        Type result_type = fn_type.as.fn.result_full_type
                           ? *fn_type.as.fn.result_full_type
                           : type_from_kind(fn_type.as.fn.result_kind);
        Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
        out->as.call_.fn_binding = fn_binding;
        out->as.call_.args = call_args;
        out->as.call_.n_args = n_call_args;
        out->as.call_.fn_expr = NULL;
        out->as.call_.dict_arg = NULL;
        out->as.call_.is_poly_call = false;
        out->as.call_.poly_arg_mask = 0;
        out->as.call_.abi_bindings = NULL;
        out->as.call_.n_abi_bindings = 0;
        return out;
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
    CallTypeBinding type_bindings[16];
    uint8_t n_type_bindings = 0;
    for (uint8_t bi = 0; bi < 16; bi++) type_bindings[bi].name = NULL;
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
        /* Nominal identity: a same-kind struct/opaque/ADT argument must be the
         * *same* type, not merely the same TypeKind.  Full param types come from
         * arg_full_types (Phase 1).  Placed before the escape hatches: those only
         * ever set arg_ok from false->true for cross-kind coercions, so demoting a
         * spurious same-kind match here cannot resurrect a real coercion.
         * See docs/upcoming/positional-nominal-type-identity-fix-plan.md. */
        if (arg_ok && (expected_arg_kind == TY_STRUCT || expected_arg_kind == TY_ADT) &&
                fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t nidx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (nidx < fn_type.as.fn.arity) {
                Type *ef = fn_type.as.fn.arg_full_types[nidx];
                if (ef && (ef->kind == TY_STRUCT || ef->kind == TY_ADT) &&
                        !type_eq(args[i]->type, *ef)) {
                    arg_ok = false;
                }
            }
        }
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
        if (!arg_ok && expected_arg_kind == TY_FN && args[i]->type.kind == TY_PTR_VOID) {
            /* A#1: a fat (^fat) parameter consumes a closure in fat-box form.  A
             * capturing closure value (EX_CLOSURE, TY_PTR_VOID) is already a fat
             * box, so accept it at a fn-typed ^fat parameter -- the ^fat call
             * site fat-dispatches through slot 0.  Without this, a capturing
             * closure could not be passed to a directly-callable closure
             * parameter at all (only captureless lambda literals, which are
             * auto-shimmed via EX_FN_TO_FAT).  Gated on arg_fat so a plain fn
             * parameter still rejects a bare :ptr<void>. */
            uint32_t fn_arg_idx_fp = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_arg_idx_fp < fn_type.as.fn.arity &&
                fn_type.as.fn.arg_fat[fn_arg_idx_fp]) {
                arg_ok = true;
            }
        }
        /* TS4P1: For a polymorphic ADT constructor, the field is stored as TY_INT
         * but its full_type is TY_TYVAR.  Accept any concrete type for such a field
         * so that e.g. (Just 1.5) at :float does not produce a type error.
         * The value will be reinterpret-cast to the concrete field type at codegen. */
        if (!arg_ok && expected_arg_kind == TY_INT) {
            uint32_t fn_arg_idx_tv = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_tv < fn_type.as.fn.arity) {
                Type *aft2 = fn_type.as.fn.arg_full_types[fn_arg_idx_tv];
                if (aft2 && aft2->kind == TY_TYVAR) {
                    arg_ok = true;
                }
            }
        }
        if (!arg_ok && expected_arg_kind == TY_INT &&
                (args[i]->type.kind == TY_FN || args[i]->type.kind == TY_PTR_VOID)) {
            /* Phase TY5: Allow passing a function reference (TY_FN) or capturing closure
             * (TY_PTR_VOID) where int64_t is expected.  HKT typeclass method signatures
             * spell function parameters as :int (opaque int64_t); both raw function
             * pointers and fat-closure env pointers are cast to int64_t via
             * (int64_t)(intptr_t) in emit_expr.c. */
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
        /* GS2: when the callee preserved a full applied parameter type, compare
         * the full TY_APP structure rather than accepting any TY_APP argument. */
        if (arg_ok && fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t fn_arg_idx_app = fn_binding->closure_fn_binding ? i + 1 : i;
            Type *expected_full = (fn_arg_idx_app < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[fn_arg_idx_app] : NULL;
            if (expected_full && call_type_has_named_tyvar(expected_full)) {
                arg_ok = call_collect_type_bindings(expected_full, args[i]->type,
                                                    type_bindings, &n_type_bindings);
            } else if (arg_ok && expected_arg_kind == TY_APP &&
                       args[i]->type.kind == TY_APP &&
                       expected_full && expected_full->kind == TY_APP) {
                arg_ok = type_eq(args[i]->type, *expected_full);
            }
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
                    Type first_mismatch = TYPE_UNKNOWN;
                    bool have_mismatch = false;
                    for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                        Type *mem = isect_t->as.intersection_.members[im];
                        if (!mem) continue;
                        /* For concrete member types, the arg must have an equal or
                         * compatible type.  For typeclass members we cannot yet do
                         * instance resolution here, so skip them. */
                        if (!typekind_is_concrete_for_disjoint(mem->kind)) continue;
                        if (!type_eq(args[i]->type, *mem)) {
                            all_ok = false;
                            first_mismatch = *mem;
                            have_mismatch = true;
                            break;
                        }
                    }
                    if (all_ok) {
                        arg_ok = true;
                    } else {
                        /* PH2.2: build composite type names into owned buffers
                         * (see PH2.1) so this error path does not leak. */
                        Buf got_buf; buf_init(&got_buf);
                        type_print(&got_buf, args[i]->type);
                        buf_putc(&got_buf, '\0');
                        Buf mem_buf; buf_init(&mem_buf);
                        if (have_mismatch) type_print(&mem_buf, first_mismatch);
                        else buf_puts(&mem_buf, "?");
                        buf_putc(&mem_buf, '\0');
                        diag_emit_with_code(DIAG_ERROR, args[i]->span,
                            TUR_E0351_INTERSECTION_MEMBER_MISMATCH,
                            "function '%s' arg %u: value of type %s does not satisfy "
                            "intersection member %s",
                            fn_binding->name->name, i + 1,
                            got_buf.data,
                            mem_buf.data);
                        buf_free(&got_buf);
                        buf_free(&mem_buf);
                        return NULL;
                    }
                }
            }
        }

        /* IT4: A <: any — any value satisfies the top type.
         * Wrap with EX_UNION_INJECT (via the shared coercion helper) using the
         * TypeKind of the value as the tag, so (type-of) and (cast) can retrieve
         * it at runtime.  TY2.2: by-value structs are heap-boxed by the helper. */
        if (!arg_ok && (g_union_types_enabled || g_intersection_types_enabled) &&
            expected_arg_kind == TY_ANY) {
            arg_ok = true;
            args[i] = elab_coerce_to_any(e, args[i]);
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
                    /* PH2.2: owned buffer for the composite (fn) type name. */
                    Buf exp_buf; buf_init(&exp_buf);
                    type_print(&exp_buf, *expected_fn);
                    buf_putc(&exp_buf, '\0');
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: linear function type mismatch"
                                        " -- expected %s but got a function with different"
                                        " linearity annotations; ^linear parameters must match exactly",
                                        fn_binding->name->name, i + 1,
                                        exp_buf.data);
                    buf_free(&exp_buf);
                    return NULL;
                }
            }
        }

        /* PH1.2: Row-precise handler argument checking. When both expected and
         * actual argument types are handlers, the kind-only `arg_ok` above is
         * not enough -- any handler would satisfy any handler parameter. Refine
         * it with `type_is_subtype` (PH0.2) so the handled-effect row and
         * value/result kinds must be compatible (FH4.1 relation: set-equality +
         * TY_UNKNOWN wildcards). The declared handler type is threaded into
         * arg_full_types by PH1.1. */
        if (arg_ok && g_effect_types_enabled &&
            expected_arg_kind == TY_HANDLER && args[i]->type.kind == TY_HANDLER) {
            uint32_t fn_arg_idx_h = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_h < fn_type.as.fn.arity) {
                Type *expected_h = fn_type.as.fn.arg_full_types[fn_arg_idx_h];
                if (expected_h && expected_h->kind == TY_HANDLER) {
                    arg_ok = type_is_subtype(args[i]->type, *expected_h);
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
            /* Compute expected type for the diagnostic.
             * For compound types (union, intersection, app, handler) that store
             * their full type in arg_full_types, look it up there so the name
             * includes member/row types. */
            Type expected_ty = type_from_kind(expected_arg_kind);
            if ((expected_arg_kind == TY_UNION || expected_arg_kind == TY_INTERSECTION ||
                 expected_arg_kind == TY_APP || expected_arg_kind == TY_HANDLER ||
                 expected_arg_kind == TY_STRUCT || expected_arg_kind == TY_ADT) &&
                fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                uint32_t fn_arg_idx4 = fn_binding->closure_fn_binding ? i + 1 : i;
                Type *ct = (fn_arg_idx4 < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fn_arg_idx4] : NULL;
                if (ct) expected_ty = *ct;
            }
            /* PH2.1: Build the type names into owned local buffers via
             * type_print rather than type_name. type_name returns a strdup-ed
             * heap string for composite kinds (handler, union, fn, ...) that no
             * caller frees -- a real LeakSanitizer-visible leak on every
             * composite-type diagnostic. type_print writes into a Buf we own and
             * free here, so this error path is leak-clean. */
            Buf expected_buf; buf_init(&expected_buf);
            type_print(&expected_buf, expected_ty);
            buf_putc(&expected_buf, '\0');
            Buf actual_buf; buf_init(&actual_buf);
            type_print(&actual_buf, args[i]->type);
            buf_putc(&actual_buf, '\0');
            diag_emit_with_code(DIAG_ERROR, args[i]->span, err_code,
                                "function '%s' arg %u: expected %s, got %s",
                                fn_binding->name->name, i + 1,
                                expected_buf.data,
                                actual_buf.data);
            buf_free(&expected_buf);
            buf_free(&actual_buf);
            /* List-macro tuple hint: tcons arg 1 is the element; when it fails
             * to unify with the expected :int type, the caller is most likely
             * mixing types in a (list ...) or hand-written (tcons ...) chain.
             * Suggest tupleN for heterogeneous fixed-arity needs. */
            if (strcmp(fn_binding->name->name, "tcons") == 0 && i == 0) {
                diag_emit(DIAG_HELP, args[i]->span,
                          "for heterogeneous fixed-arity collections, "
                          "consider tuple2, tuple3, tuple4, or tuple5 "
                          "instead of (list ...)");
            }
            return NULL;
        }

        if (fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx_cast = fn_binding->closure_fn_binding ? i + 1 : i;
            Type *expected_full = (fn_type.as.fn.arg_full_types &&
                                   fn_arg_idx_cast < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[fn_arg_idx_cast] : NULL;
            if ((expected_arg_kind == TY_TYVAR ||
                 (expected_full && expected_full->kind == TY_TYVAR)) &&
                args[i]->type.kind != TY_INT) {
                args[i] = call_wrap_reinterpret(e, args[i], TY_INT, args[i]->span);
            }
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

        /* A#1: ^fat parameter -- auto-shim a bare (non-capturing) fn into a fat
         * closure so a fat-call consumer (reactor cb, free-bind kont, ...) reads a
         * valid { thunk, env } layout instead of a bare function pointer.  A
         * capturing closure (TY_PTR_VOID) is already fat and passes through; nil is
         * a null callback.  Any other argument kind to a ^fat parameter is the
         * diagnostic half of A#1 -- a typed error instead of a runtime segfault.
         * EX_FN_TO_FAT carries TY_PTR_VOID, mirroring EX_CLOSURE, so it reuses the
         * same arg-emission casts that already feed closures to :fn/:int/:ptr. */
        if (!is_rank2_param && fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx_fat = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_arg_idx_fat < fn_type.as.fn.arity &&
                fn_type.as.fn.arg_fat[fn_arg_idx_fat]) {
                TypeKind ak = args[i]->type.kind;
                bool arg_is_poly_fn = (args[i]->kind == EX_VAR &&
                                       args[i]->as.var.binding &&
                                       args[i]->as.var.binding->is_poly_fn);
                if (arg_is_poly_fn) {
                    /* SC7: a tur_poly_fn_t value (a typeclass-method closure
                     * param, marked is_poly_fn) is a 16-byte {env,fn} struct,
                     * not a single-int64 fat handle.  Box it into a fat-closure
                     * handle so the ^fat consumer's TUR_APPLY can fat-call it --
                     * this is what lets a Functor/Applicative instance hand its
                     * closure argument to a ^fat schema combinator. */
                    Expr *conv = expr_new(e->arena, EX_POLY_TO_FAT, TYPE_PTR_VOID,
                                          args[i]->span);
                    conv->as.poly_to_fat_.inner = args[i];
                    args[i] = conv;
                } else if (ak == TY_FN && !args[i]->type.as.fn.boxed) {
                    /* A bare (non-capturing) fn reference -- auto-shim it into a
                     * fat box.  A *boxed* TY_FN (CRU B-1: a capturing closure
                     * value) is already a fat { thunk, env... } box, so it falls
                     * through to the pass-through branch below exactly as a
                     * TY_PTR_VOID closure did pre-B-1; shimming it here would
                     * double-box and segfault. */
                    uint8_t inner_arity = args[i]->type.as.fn.arity;
                    if (inner_arity > 5) {
                        diag_emit(DIAG_ERROR, args[i]->span,
                            "fat (^fat) parameter of '%s' cannot shim an arity-%u "
                            "function (auto-shim supports up to 5 arguments)",
                            fn_binding->name->name, (unsigned)inner_arity);
                        return NULL;
                    }
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID,
                                          args[i]->span);
                    shim->as.fn_to_fat_.inner = args[i];
                    args[i] = shim;
                } else if (ak == TY_PTR_VOID || (ak == TY_FN && args[i]->type.as.fn.boxed) ||
                           ak == TY_NIL ||
                           (ak == TY_INT && args[i]->kind == EX_INT_LIT &&
                            args[i]->as.i == 0) ||
                           (ak == TY_INT && args[i]->kind != EX_INT_LIT)) {
                    /* Pass through unchanged: a fat closure (TY_PTR_VOID), nil, a
                     * null (literal 0) callback, or an already-erased :int
                     * fat-closure handle (a computed value, e.g. a handler that
                     * compose-middleware/compose-middleware-of has already boxed).
                     * The :int-handle case lets a ^fat boundary param sit on the
                     * same plumbing that threads composed handlers as :int without
                     * re-boxing them; a bare non-capturing fn still arrives as
                     * TY_FN at its first boundary and is shimmed above. */
                } else {
                    Buf gb; buf_init(&gb);
                    type_print(&gb, args[i]->type);
                    buf_putc(&gb, '\0');
                    diag_emit(DIAG_ERROR, args[i]->span,
                        "argument %u to fat (^fat) parameter of '%s' must be a "
                        "function or closure, got %s",
                        i + 1, fn_binding->name->name, gb.data);
                    buf_free(&gb);
                    return NULL;
                }
            }
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
            /* LB1: a ^borrow parameter reads its argument without taking
             * ownership, so it must NOT move (poison) the binding -- otherwise a
             * move-typed (e.g. :affine) handle could not be read by a borrowing
             * accessor and then used again (TUR-E0005). This is the move-checker
             * half of the borrow form; the linear/affine usage rollback below
             * handles the -Xlinear / substructural budgets. */
            bool param_is_borrow = false;
            if (fn_type.kind == TY_FN) {
                uint32_t fn_borrow_idx = fn_binding->closure_fn_binding ? i + 1 : i;
                if (fn_borrow_idx < fn_type.as.fn.arity)
                    param_is_borrow = fn_type.as.fn.arg_borrow[fn_borrow_idx];
            }
            if (!param_is_unique_mut && !arg_is_unique_mut && !param_is_borrow) {
                binding_mark_moved(arg_b2, args[i]->span);
            }
        }

        /* LB1: ^borrow parameter -- the argument is read but NOT consumed.
         * The var-use elaboration above already recorded a consumption on a
         * fresh linear binding (or emitted TUR-E0101 and bailed if the handle
         * was already consumed, e.g. a free-then-borrow ordering).  Reaching
         * here means the borrow is legal, so roll back the consumption: the
         * single-consumption obligation is preserved for a later consuming op
         * (fs/tmpfile-free, mutex-free, ...).  This is the call-site half of
         * the borrow form -- see docs/reported/stdlib-linear-handle-borrows.md. */
        if (args[i]->kind == EX_VAR && fn_type.kind == TY_FN) {
            uint32_t fn_borrow_idx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_borrow_idx < fn_type.as.fn.arity &&
                    fn_type.as.fn.arg_borrow[fn_borrow_idx]) {
                Binding *arg_b3 = args[i]->as.var.binding;
                if (g_linear_enabled && arg_b3->is_linear) {
                    arg_b3->is_linear_consumed = false;
                }
                /* Substructural affine: undo the single use the var-use path
                 * recorded so the borrow does not spend the at-most-once budget. */
                if (g_substructural_enabled && arg_b3->is_affine
                        && arg_b3->usage_state == USAGE_USED_ONCE) {
                    arg_b3->usage_state = USAGE_UNUSED;
                }
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
            if (call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
                n_type_bindings > 0) {
                result_type = call_instantiate_type(e, fn_type.as.fn.result_full_type,
                                                    type_bindings, n_type_bindings);
            } else {
                result_type = *fn_type.as.fn.result_full_type;
            }
        } else {
            result_type = type_from_kind(result_kind);
        }
    } else if (fn_type.kind == TY_CONT) {
        /* Calling a continuation returns its result type (though in practice it jumps) */
        result_type = type_from_kind(fn_type.as.cont.returns);
    } else {
        result_type = TYPE_NIL;
    }

    Type call_result_type = result_type;
    bool wrap_generic_result = false;
    if (fn_type.kind == TY_FN &&
        fn_type.as.fn.result_kind == TY_TYVAR) {
        call_result_type = TYPE_INT;
        wrap_generic_result = (result_type.kind != TY_INT);
    }

    /* Report guard (poly-defn-shares-inner-closure-body-across-monomorphizations):
     * a generic defn that *returns* an (fn ...) whose declared result type is one
     * of the defn's type parameters emits a single shared inner closure body,
     * carried with the integer thunk ABI (the result tyvar lowers to int64_t).
     * The outer defn is monomorphized per concrete type, but the inner body is
     * not; a float specialization dispatches the shared body through a
     * `double (*)(...)` pointer (xmm0) while the body returns through rax -- a
     * silent register-class miscompile.  cstr/ptr/int specializations share the
     * integer register and round-trip, so only a floating-point binding of the
     * inner result tyvar is rejected here.  See the report for fix directions. */
    if (n_type_bindings > 0 && fn_binding && fn_binding->returns_closure_fn_binding) {
        Binding *inner = fn_binding->returns_closure_fn_binding;
        const Type *inner_res = (inner->type.kind == TY_FN)
            ? inner->type.as.fn.result_full_type : NULL;
        if (inner_res && inner_res->kind == TY_TYVAR && inner_res->as.tyvar_.name) {
            uint8_t bidx = 0;
            if (call_find_type_binding(type_bindings, n_type_bindings,
                                       inner_res->as.tyvar_.name, &bidx)) {
                TypeKind bk = type_bindings[bidx].type.kind;
                if (bk == TY_FLOAT || bk == TY_FLOAT32 || bk == TY_FLOAT64) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0705_POLY_CLOSURE_RESULT_TYVAR,
                        "polymorphic closure-returning function '%s' specialized at a "
                        "floating-point type (TUR-E0705): the returned (fn ...) result "
                        "type is the type parameter '%s', but its body is emitted once "
                        "with the integer-register closure ABI; a float specialization "
                        "is a silent register-class miscompile (xmm0 vs rax). Work "
                        "around by writing a monomorphic defn per concrete result type.",
                        fn_binding->name ? fn_binding->name->name : "?",
                        inner_res->as.tyvar_.name);
                }
            }
        }
    }

    Expr *out = expr_new(e->arena, EX_CALL, call_result_type, call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    /* GS5/CS3: hand the named-tyvar substitution to emit so it can drive ABI
     * specialization without re-deriving it from the call's argument types. */
    if (n_type_bindings > 0) {
        AbiTypeBinding *saved = (AbiTypeBinding *)arena_alloc(
            e->arena, n_type_bindings * sizeof(AbiTypeBinding));
        for (uint8_t bi = 0; bi < n_type_bindings; bi++) saved[bi] = type_bindings[bi];
        out->as.call_.abi_bindings = saved;
        out->as.call_.n_abi_bindings = n_type_bindings;
    }
    if (wrap_generic_result) {
        return call_wrap_reinterpret(e, out, result_type.kind, call->span);
    }
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
