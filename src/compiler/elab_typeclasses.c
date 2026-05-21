/* elab_typeclasses.c -- typeclass declarations, instances, and method-call dispatch. */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span,
    uint32_t *out_body_start);
static Expr *make_dict_expr(Elab *e, TypeClassInstance *inst, Span span);

/* Phase 15: Typeclasses */

/* Parse a single typeclass method definition from a Form.
 * Syntax: (method-name [param1 : type1, param2 : type2, ...] : return-type)
 * or: (method-name [param1 param2 ...] : return-type) - types inferred from usage
 */
static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span,
                                               uint32_t *out_body_start) {
    if (method_form->tag != F_LIST || method_form->as.list.len < 3) {
        diag_emit(DIAG_ERROR, span,
                  "typeclass method requires (name [params...] : return-type)");
        return NULL;
    }
    
    /* Parse method name */
    Form *name_form = method_form->as.list.items[0];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "typeclass method name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    
    /* Parse parameter vector */
    Form *params_form = method_form->as.list.items[1];
    if (params_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_form->span,
                  "typeclass method parameter list must be a vector");
        return NULL;
    }
    
    /* Parse parameters */
    uint8_t n_params = params_form->as.list.len;
    const Symbol **param_names = NULL;
    Type *param_types = NULL;
    /* Phase CCL: callable-param flag array — parallel to param_names/param_types.
     * param_is_fn[i] = true when the i-th param is declared with [name :fn] syntax,
     * marking it as a single-argument callable that should receive tur_poly_fn_t. */
    bool *param_is_fn = NULL;

    if (n_params > 0) {
        param_names = (const Symbol **)arena_alloc(e->arena, n_params * sizeof(const Symbol *));
        param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
        param_is_fn = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        for (uint8_t i = 0; i < n_params; i++) param_is_fn[i] = false;

        /* actual_p: number of real parameters encountered (keywords don't count). */
        uint8_t actual_p = 0;
        for (uint8_t i = 0; i < n_params; i++) {
            Form *p = params_form->as.list.items[i];
            if (p->tag == F_SYM) {
                param_names[actual_p] = p->as.sym;
                /* Default to int for now - type inference for method params deferred */
                param_types[actual_p] = TYPE_INT;
                param_is_fn[actual_p] = false;
                actual_p++;
            } else if (p->tag == F_KEYWORD) {
                /* Inline type annotation for the previous parameter:
                 * e.g. [b :ptr<void>] where :ptr<void> annotates b */
                if (actual_p == 0) {
                    diag_emit(DIAG_ERROR, p->span,
                              "type annotation without preceding parameter");
                    return NULL;
                }
                uint8_t prev = actual_p - 1;
                const Symbol *kw = p->as.sym;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    param_types[prev] = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    param_types[prev] = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    param_types[prev] = TYPE_CSTR;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    param_types[prev] = TYPE_PTR_VOID;
                } else if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                    param_types[prev] = TYPE_PTR_VOID;
                    param_is_fn[prev] = true;
                } else {
                    diag_emit(DIAG_ERROR, p->span,
                              "unsupported type in typeclass method parameter");
                    return NULL;
                }
            } else if (p->tag == F_VEC && p->as.list.len >= 2) {
                /* [name : type] or [name :fn] nested vector syntax */
                Form *name_f = p->as.list.items[0];
                Form *type_f = p->as.list.items[1];
                if (name_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, name_f->span,
                              "parameter name must be a symbol");
                    return NULL;
                }
                param_names[actual_p] = name_f->as.sym;
                param_is_fn[actual_p] = false;
                /* Parse type annotation */
                if (type_f->tag == F_KEYWORD) {
                    const Symbol *kw = type_f->as.sym;
                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                        param_types[actual_p] = TYPE_INT;
                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                        param_types[actual_p] = TYPE_BOOL;
                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                        param_types[actual_p] = TYPE_CSTR;
                    } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                               (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                        param_types[actual_p] = TYPE_PTR_VOID;
                    } else if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                        /* Phase CCL: :fn marks this param as a single-argument
                         * callable; it will be passed as tur_poly_fn_t at call
                         * sites so that capturing closures work transparently. */
                        param_types[actual_p] = TYPE_PTR_VOID;
                        param_is_fn[actual_p] = true;
                    } else {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type in typeclass method parameter");
                        return NULL;
                    }
                } else if (type_f->tag == F_TYPE_ANN) {
                    /* `: type-expr` compound annotation */
                    Type *ft = (type_f->as.list.len > 0)
                        ? type_expr_from_form(e, type_f->as.list.items[0], NULL, NULL, NULL, 0)
                        : NULL;
                    if (!ft) {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type form in typeclass method parameter");
                        return NULL;
                    }
                    param_types[actual_p] = *ft;
                } else if (type_f->tag == F_LIST || type_f->tag == F_VEC) {
                    /* Phase HRT3: allow forall/exists type forms as parameter types */
                    Type *ft = type_expr_from_form(e, type_f, NULL, NULL, NULL, 0);
                    if (!ft) {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type form in typeclass method parameter");
                        return NULL;
                    }
                    param_types[actual_p] = *ft;
                } else {
                    param_types[actual_p] = TYPE_INT; /* default */
                }
                actual_p++;
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "parameter must be a symbol or [name : type] vector");
                return NULL;
            }
        }
        n_params = actual_p;
    }
    
    /* Parse return type - must be after params */
    /* Syntax: (method [params] : return-type)
     *      or (method [params] : #{Effect...} return-type)  -- effect row annotation
     *      or (method [params] #{Effect...} : return-type)  -- effect row annotation alt
     *
     * ER3: #{...} (F_MAP) in return-type position is now parsed and stored on
     * the method so effect_check_pass can enforce it against instance method bodies. */
    EffectRow *method_effect_row = NULL;
    Type return_type = TYPE_NIL;
    uint32_t ret_idx = 2;   /* first element after params vector */
    if (method_form->as.list.len > ret_idx) {
        Form *maybe_row = method_form->as.list.items[ret_idx];
        if (maybe_row->tag == F_MAP) {
            /* #{Effect...} effect-row annotation -- parse and store it. */
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    syms[n_valid++] = item->as.sym;
                }
            }
            method_effect_row = effect_row_unresolved(e->arena, syms, n_valid);
            ret_idx++;
        }
    }
    if (method_form->as.list.len > ret_idx) {
        Form *ret_form = method_form->as.list.items[ret_idx];
        if (ret_form->tag == F_MAP) {
            /* another effect row or #{} after the params — skip silently */
            /* (ignore the rest; return type stays TYPE_NIL) */
        } else if (ret_form->tag == F_KEYWORD) {
            const Symbol *kw = ret_form->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_type = TYPE_INT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_type = TYPE_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_type = TYPE_CSTR;
            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                return_type = TYPE_NIL;
            } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                       (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                return_type = TYPE_PTR_VOID;
            } else {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type in typeclass method");
                return NULL;
            }
        } else if (ret_form->tag == F_TYPE_ANN) {
            /* `: type-expr` compound return type annotation */
            Type *ft = (ret_form->as.list.len > 0)
                ? type_expr_from_form(e, ret_form->as.list.items[0], NULL, NULL, NULL, 0)
                : NULL;
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            return_type = *ft;
        } else if (ret_form->tag == F_LIST || ret_form->tag == F_VEC) {
            /* Phase HRT3: allow forall/exists type forms as return types */
            Type *ft = type_expr_from_form(e, ret_form, NULL, NULL, NULL, 0);
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            return_type = *ft;
        } else {
            diag_emit(DIAG_ERROR, ret_form->span,
                      "typeclass method return type must be a keyword like :int");
            return NULL;
        }
    }
    
    /* ER3: Report where body forms start so elab_defclass can elaborate defaults.
     * body_start_idx is the index of the first form after the return type (or
     * method_form->as.list.len if there are no body forms). */
    uint32_t body_start_idx = ret_idx + 1;
    if (out_body_start) *out_body_start = body_start_idx;

    TypeClassMethod *method = (TypeClassMethod *)arena_alloc(e->arena, sizeof(TypeClassMethod));
    method->name = name;
    method->param_names = param_names;
    method->param_types = param_types;
    method->param_is_fn = param_is_fn;
    method->n_params = n_params;
    method->return_type = return_type;
    method->effect_row = method_effect_row;  /* ER3: NULL if not annotated */
    method->default_fn_expr = NULL;          /* ER3: set by elab_defclass if body forms exist */
    return method;
}

/* Elaborate (defclass Name [type-params...] (method1 ...) (method2 ...) ...)
 *
 * Defines a new typeclass with type parameters and methods.
 * Syntax: (defclass Eq [a] (eq? [x : a, y : a] : bool))
 *         (defclass Show [a] (show [x : a] : cstr))
 */
Expr *elab_defclass(Elab *e, const Form *call) {
    /* Minimum: (defclass Name) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires a name: (defclass Name [...])");
        return NULL;
    }
    
    /* Parse typeclass name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defclass name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Phase HKT H3: Functor, Applicative, Monad, Traversable, Foldable are now
     * defined (in stdlib/typeclass.tur), not reserved.  The only guard remaining
     * is the standard "already defined" check below. */

    /* Check if already defined */
    TypeClass *existing = typeclass_env_lookup_typeclass(&e->typeclass_env, name);
    if (existing) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "typeclass '%s' is already defined", name->name);
        return NULL;
    }
    
    /* Parse type parameters (optional) */
    const Symbol **type_params = NULL;
    Kind         *type_param_kinds = NULL;
    uint8_t n_type_params = 0;
    uint32_t methods_start = 2;

    if (call->as.list.len >= 3) {
        Form *params_form = call->as.list.items[2];
        if (params_form->tag == F_VEC) {
            n_type_params = params_form->as.list.len;
            if (n_type_params > 0) {
                type_params = (const Symbol **)arena_alloc(e->arena,
                    n_type_params * sizeof(const Symbol *));
                /* Phase PTC2: Explicitly initialize all type_param_kinds to KIND_STAR.
                 * Note: arena_alloc does NOT zero memory, contrary to the old comment. */
                type_param_kinds = (Kind *)arena_alloc(e->arena,
                    n_type_params * sizeof(Kind));
                for (uint8_t i = 0; i < n_type_params; i++) {
                    type_param_kinds[i] = KIND_STAR;  /* Default kind for all params */
                }
                
                for (uint8_t i = 0; i < n_type_params; i++) {
                    Form *p = params_form->as.list.items[i];
                    /* Phase HKT H1: [f :kind] vector form — lowered to the same
                     * internal representation as '^f' (KIND_ARROW) or '^^f' (KIND_ARROW2).
                     * Accepted forms: [f :kind] and [f :kind2]. */
                    if (p->tag == F_VEC) {
                        if (p->as.list.len != 2 ||
                            p->as.list.items[0]->tag != F_SYM ||
                            p->as.list.items[1]->tag != F_KEYWORD) {
                            diag_emit(DIAG_ERROR, p->span,
                                      "[f :kind] form requires exactly two elements: "
                                      "a symbol and a kind keyword (:kind or :kind2)");
                            return NULL;
                        }
                        const Symbol *bare = p->as.list.items[0]->as.sym;
                        const char   *kw   = p->as.list.items[1]->as.sym->name;
                        Kind          kt;
                        if (strcmp(kw, "kind2") == 0) {
                            kt = KIND_ARROW2;
                        } else if (strcmp(kw, "kind") == 0) {
                            kt = KIND_ARROW;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "unknown kind keyword ':%s'; expected :kind or :kind2",
                                      kw);
                            return NULL;
                        }
                        if (bare->len == 0 || bare->name[0] < 'a' || bare->name[0] > 'z') {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use a lowercase name in [name :kind]",
                                      bare->name);
                            return NULL;
                        }
                        type_params[i]      = bare;
                        type_param_kinds[i] = kt;
                        continue;
                    }
                    if (p->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "type parameter must be a symbol");
                        return NULL;
                    }
                    /* Phase HKT H1: '^name' prefix marks a kind * -> * (type constructor)
                     * parameter.  The canonical name stored is 'name' (without '^').
                     * Phase HKT H5: '^^name' prefix marks a kind * -> * -> * (binary
                     * type constructor) parameter. */
                    if (p->as.sym->len > 2 && p->as.sym->name[0] == '^' && p->as.sym->name[1] == '^') {
                        const char  *bare     = p->as.sym->name + 2;
                        uint32_t     bare_len = p->as.sym->len  - 2;
                        /* Only lowercase-leading names are kind variables. */
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW2;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^^name' for a kind '* -> * -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
                        const char  *bare     = p->as.sym->name + 1;
                        uint32_t     bare_len = p->as.sym->len  - 1;
                        /* Only lowercase-leading names are kind variables. */
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW;
                        } else {
                            /* Uppercase — treat as a constraint annotation in wrong place. */
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^name' for a kind '* -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else {
                        type_params[i]      = p->as.sym;
                        type_param_kinds[i] = KIND_STAR;
                    }
                }
            }
            methods_start = 3;
        }
    }
    
    /* Parse methods */
    TypeClassMethod *methods = NULL;
    uint8_t n_methods = 0;
    
    /* First pass: count methods */
    for (uint32_t i = methods_start; i < call->as.list.len; i++) {
        n_methods++;
    }
    
    if (n_methods == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires at least one method");
        return NULL;
    }
    
    /* Allocate methods array */
    methods = (TypeClassMethod *)arena_alloc(e->arena, n_methods * sizeof(TypeClassMethod));
    
    /* Second pass: parse each method and elaborate any default bodies */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[methods_start + i];
        uint32_t body_start = 0;
        TypeClassMethod *method = parse_typeclass_method(e, method_form, call->span, &body_start);
        if (!method) return NULL;
        methods[i] = *method;

        /* ER3: If the method form has forms after the return type, elaborate
         * them as a default body.  This mirrors elab_definstance's method
         * elaboration so that effect_check_pass finds it as a normal FnDef. */
        if (body_start < method_form->as.list.len) {
            /* Build a synthetic function name: __default_<TypeClass>_<method> */
            char default_name_buf[192];
            snprintf(default_name_buf, sizeof(default_name_buf),
                     "__default_%s_%s", name->name, method->name->name);
            const Symbol *default_sym = symtab_intern(e->st,
                strslice(default_name_buf, strlen(default_name_buf)));

            /* Build parameter bindings from the method signature */
            uint8_t n_mp = methods[i].n_params;
            Binding **mp = n_mp > 0
                ? (Binding **)arena_alloc(e->arena, n_mp * sizeof(Binding *)) : NULL;
            Type *mp_types = n_mp > 0
                ? (Type *)arena_alloc(e->arena, n_mp * sizeof(Type)) : NULL;

            /* Parse body parameter names from the method form's param vector */
            Form *pbody_params = method_form->as.list.items[1]; /* the [params] vector */
            uint8_t actual_p = 0;
            for (uint8_t j = 0; j < pbody_params->as.list.len && actual_p < n_mp; j++) {
                Form *pf = pbody_params->as.list.items[j];
                const Symbol *pname = NULL;
                Type ptype = methods[i].n_params > actual_p
                    ? methods[i].param_types[actual_p] : TYPE_INT;
                if (pf->tag == F_SYM) {
                    pname = pf->as.sym;
                } else if (pf->tag == F_VEC && pf->as.list.len >= 1
                           && pf->as.list.items[0]->tag == F_SYM) {
                    pname = pf->as.list.items[0]->as.sym;
                }
                if (!pname) continue;
                mp[actual_p] = binding_new(e, pname, ptype, false, false, pf->span);
                mp_types[actual_p] = ptype;
                actual_p++;
            }
            n_mp = actual_p;

            /* Push scope with parameters */
            Scope def_scope;
            scope_init(&def_scope, e->scope);
            e->scope = &def_scope;
            for (uint8_t j = 0; j < n_mp; j++)
                scope_add(&def_scope, mp[j]);
            e->fn_body_depth++;

            /* Elaborate body forms */
            uint32_t n_body = method_form->as.list.len - body_start;
            Expr *def_body = e_nil(e, method_form->span);
            if (n_body == 1) {
                def_body = elab_form(e, method_form->as.list.items[body_start]);
            } else if (n_body > 1) {
                Expr **body_items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
                for (uint32_t k = 0; k < n_body; k++) {
                    body_items[k] = elab_form(e, method_form->as.list.items[body_start + k]);
                    if (!body_items[k]) {
                        e->fn_body_depth--;
                        e->scope = def_scope.parent;
                        scope_free(&def_scope);
                        return NULL;
                    }
                }
                def_body = expr_new(e->arena, EX_DO,
                    body_items[n_body - 1]->type, method_form->span);
                def_body->as.do_.items = body_items;
                def_body->as.do_.n = n_body;
            }

            e->fn_body_depth--;
            e->scope = def_scope.parent;
            scope_free(&def_scope);

            /* Build FnDef and register it as a file-level function */
            TypeKind pk[MAX_FN_ARITY];
            for (uint8_t j = 0; j < n_mp; j++) pk[j] = mp_types[j].kind;
            Type fn_t = type_fn(pk, n_mp, methods[i].return_type.kind);

            FnDef *def_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
            Binding *def_b = binding_new(e, default_sym, fn_t, false, true,
                                          method_form->span);
            def_fd->binding        = def_b;
            def_fd->params         = mp;
            def_fd->n_params       = n_mp;
            def_fd->body           = def_body;
            def_fd->is_variadic    = false;
            def_fd->closure        = NULL;
            def_fd->param_types    = mp_types;
            def_fd->may_capture    = false;
            def_fd->inferred_effect_row = NULL;
            constraint_set_init(&def_fd->constraints);

            scope_add(&e->global, def_b);
            Expr *def_expr = expr_new(e->arena, EX_FN_DEF, fn_t, method_form->span);
            def_expr->as.fn_def_.fn = def_fd;
            elab_register_file_def(e, def_expr);

            methods[i].default_fn_expr = def_expr;
        }
    }
    
    /* Register the typeclass in the environment */
    TypeClass *tc = typeclass_env_register_typeclass(&e->typeclass_env, name);
    if (!tc) {
        diag_emit(DIAG_ERROR, call->span,
                  "failed to register typeclass '%s'", name->name);
        return NULL;
    }
    
    tc->type_params       = type_params;
    tc->type_param_kinds  = type_param_kinds;
    tc->n_type_params     = n_type_params;
    tc->methods           = methods;
    tc->n_methods         = n_methods;
    /* Phase HKT-P4: record the file that defined this typeclass. */
    tc->origin_file_id    = call->span.file_id;

    /* Create a TYPECLASS_DEF expression for codegen */
    Expr *tc_expr = expr_new(e->arena, EX_TYPECLASS_DEF, TYPE_NIL, call->span);
    tc_expr->as.typeclass_def_.typeclass = tc;
    elab_register_file_def(e, tc_expr);
    
    /* Create a nil expression as the result (defclass returns nothing) */
    return e_nil(e, call->span);
}

/* Elaborate (definstance ClassName [type-args...] (method1 [args...] body...) ...)
 *
 * Defines an instance of a typeclass for concrete types.
 * Syntax: (definstance Eq int (eq? [x y] (== x y)))
 *         (definstance Show int (show [x] (int->str x)))
 */
Expr *elab_definstance(Elab *e, const Form *call) {
    /* Minimum: (definstance ClassName) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance requires a typeclass name: (definstance ClassName ...)");
        return NULL;
    }
    
    /* Parse typeclass name */
    Form *tc_form = call->as.list.items[1];
    if (tc_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, tc_form->span,
                  "definstance typeclass name must be a symbol");
        return NULL;
    }
    const Symbol *tc_name = tc_form->as.sym;
    
    /* Look up the typeclass */
    TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, tc_name);
    if (!tc) {
        diag_emit(DIAG_ERROR, tc_form->span,
                  "typeclass '%s' is not defined", tc_name->name);
        return NULL;
    }
    
    /* Parse type arguments (optional) */
    Type *type_args = NULL;
    /* Phase HKT H3: track original symbol for each type arg so method name
     * mangling can use "option", "vec", etc. instead of the generic "T".
     * Only allocated when needed (at least one unknown/constructor type arg). */
    const Symbol **type_arg_syms = NULL;
    uint8_t n_type_args = 0;
    uint32_t impls_start = 2;
    
    if (call->as.list.len >= 3) {
        Form *args_form = call->as.list.items[2];
        if (args_form->tag == F_VEC) {
            n_type_args = args_form->as.list.len;
            if (n_type_args > 0) {
                type_args = (Type *)arena_alloc(e->arena, n_type_args * sizeof(Type));
                type_arg_syms = (const Symbol **)arena_alloc(e->arena,
                    n_type_args * sizeof(const Symbol *));
                for (uint8_t i = 0; i < n_type_args; i++) {
                    type_arg_syms[i] = NULL;  /* NULL means use default name */
                }
                for (uint8_t i = 0; i < n_type_args; i++) {
                    Form *arg = args_form->as.list.items[i];
                    /* Parse type keywords or symbols */
                    if (arg->tag == F_KEYWORD || arg->tag == F_SYM) {
                        const Symbol *kw = arg->as.sym;
                        if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                            type_args[i] = TYPE_INT;
                        } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                            type_args[i] = TYPE_BOOL;
                        } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                            type_args[i] = TYPE_CSTR;
                        } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                   (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                            type_args[i] = TYPE_NIL;
                        } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                            type_args[i] = TYPE_PTR_VOID;
                        } else {
                            /* Phase N4: Try new numeric type names first. */
                            TypeKind nk = typekind_from_symbol(kw->name);
                            if (nk != TY_UNKNOWN) {
                                type_args[i] = type_simple(nk, CK_COPY);
                            } else {
                                /* Check if this name refers to a known struct type.
                                 * If so, preserve the StructDef pointer so the kind
                                 * system can distinguish concrete structs (kind *)
                                 * from opaque type constructors (kind * -> *). */
                                Binding *sb = scope_lookup(e->scope, kw);
                                if (sb && sb->type.kind == TY_STRUCT && sb->type.as.struct_.def) {
                                    type_args[i] = sb->type;
                                } else {
                                    /* Phase HKT H3: Unknown name — treat as an opaque type constructor.
                                     * TY_STRUCT without a StructDef causes codegen to emit 'void *' for
                                     * all parameters that inherit this type, which is the correct C type
                                     * for containers represented as heap pointers (option, vec, etc.).
                                     * Track the symbol name so method name mangling can use it. */
                                    memset(&type_args[i], 0, sizeof(type_args[i]));
                                    type_args[i].kind = TY_STRUCT;
                                    type_args[i].copy_kind = CK_MOVE;
                                    type_args[i].as.struct_.def = NULL;
                                }
                                type_arg_syms[i] = kw;
                            }
                        }
                    } else if (arg->tag == F_LIST && arg->as.list.len == 2) {
                        /* Phase HKT §3: (constructor arg) — partial type application.
                         * Parses `(result int)` in type position as TY_APP where
                         * fn = TY_STRUCT(constructor, KIND_ARROW2) and arg = concrete type.
                         * This allows `(definstance Functor [(result int)] ...)`. */
                        Form *ctor_form = arg->as.list.items[0];
                        Form *aarg_form = arg->as.list.items[1];
                        if (ctor_form->tag != F_SYM && ctor_form->tag != F_KEYWORD) {
                            diag_emit(DIAG_ERROR, ctor_form->span,
                                      "type application constructor must be a symbol");
                            return NULL;
                        }
                        const Symbol *ctor_sym = ctor_form->as.sym;
                        /* Parse the argument type (must be a primitive or known sym) */
                        Type app_arg_type;
                        if (aarg_form->tag == F_SYM || aarg_form->tag == F_KEYWORD) {
                            const Symbol *akw = aarg_form->as.sym;
                            if (akw->len == 3 && memcmp(akw->name, "int", 3) == 0) {
                                app_arg_type = TYPE_INT;
                            } else if (akw->len == 4 && memcmp(akw->name, "bool", 4) == 0) {
                                app_arg_type = TYPE_BOOL;
                            } else if (akw->len == 4 && memcmp(akw->name, "cstr", 4) == 0) {
                                app_arg_type = TYPE_CSTR;
                            } else if ((akw->len == 4 && memcmp(akw->name, "void", 4) == 0) ||
                                       (akw->len == 3 && memcmp(akw->name, "nil", 3) == 0)) {
                                app_arg_type = TYPE_NIL;
                            } else {
                                /* Unknown type arg — treat as opaque struct */
                                memset(&app_arg_type, 0, sizeof(app_arg_type));
                                app_arg_type.kind = TY_STRUCT;
                                app_arg_type.copy_kind = CK_MOVE;
                                app_arg_type.as.struct_.def = NULL;
                            }
                        } else {
                            diag_emit(DIAG_ERROR, aarg_form->span,
                                      "type application argument must be a type keyword or symbol");
                            return NULL;
                        }
                        /* Build fn type: TY_STRUCT with no def, KIND_ARROW2 (binary constructor) */
                        Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                        memset(fn_type, 0, sizeof(Type));
                        fn_type->kind = TY_STRUCT;
                        fn_type->copy_kind = CK_MOVE;
                        fn_type->hkt_kind = KIND_ARROW2;
                        fn_type->as.struct_.def = NULL;
                        /* Build arg type on arena */
                        Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *arg_type_ptr = app_arg_type;
                        /* Assemble TY_APP */
                        memset(&type_args[i], 0, sizeof(type_args[i]));
                        type_args[i].kind = TY_APP;
                        type_args[i].copy_kind = CK_MOVE;
                        /* fn of KIND_ARROW2 applied to one arg → KIND_ARROW */
                        type_args[i].hkt_kind = KIND_ARROW;
                        type_args[i].as.app.fn  = fn_type;
                        type_args[i].as.app.arg = arg_type_ptr;
                        /* Store constructor sym for name mangling (e.g. "result") */
                        type_arg_syms[i] = ctor_sym;
                    } else {
                        diag_emit(DIAG_ERROR, arg->span,
                                  "unsupported type argument in definstance");
                        return NULL;
                    }
                }
            }
            /* Phase HKT-P1: After parsing individual type arguments, combine consecutive
             * symbols into TY_APP for implicit type application syntax [result int].
             * This allows both [(result int)] (explicit) and [result int] (implicit). */
            if (n_type_args > 0) {
                for (uint8_t i = 0; i < n_type_args; ) {
                    if (i + 1 < n_type_args) {
                        /* Check if current is TY_STRUCT (potential constructor) and next is a type */
                        if (type_args[i].kind == TY_STRUCT && type_args[i].as.struct_.def == NULL) {
                            Type *next_type = &type_args[i + 1];
                            /* Next can be any concrete type (primitive, TY_STRUCT, or TY_APP) */
                            if (next_type->kind != TY_UNKNOWN) {
                                /* Combine into TY_APP */
                                Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *fn_type = type_args[i];  /* Copy the constructor type */
                                fn_type->hkt_kind = KIND_ARROW2;  /* Assume binary constructor */
                                
                                Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *arg_type_ptr = type_args[i + 1];
                                
                                /* Create TY_APP */
                                type_args[i].kind = TY_APP;
                                type_args[i].copy_kind = CK_MOVE;
                                type_args[i].hkt_kind = KIND_ARROW;  /* ARROW2 applied to 1 arg */
                                type_args[i].as.app.fn = fn_type;
                                type_args[i].as.app.arg = arg_type_ptr;
                                /* Store constructor sym for name mangling if we have it */
                                /* type_arg_syms[i] already contains the constructor symbol */
                                
                                /* Remove the second type arg by shifting */
                                for (uint8_t j = i + 1; j < n_type_args - 1; j++) {
                                    type_args[j] = type_args[j + 1];
                                    if (type_arg_syms) {
                                        type_arg_syms[j] = type_arg_syms[j + 1];
                                    }
                                }
                                n_type_args--;
                                /* Don't advance i - recheck current position */
                                continue;
                            }
                        }
                    }
                    i++;
                }
            }
            impls_start = 3;
        }
    }
    
    /* Phase PTC1: Parse type parameter constraints (optional)
     * Syntax: (definstance Clone [Pair a b] [(Clone a) (Clone b)] (clone [x] ...))
     * Constraint vector is a vector of lists: [(Clone a) (Clone b)]
     * Each constraint is a list (Clone a) where Clone is the typeclass and a is the type param.
     * After type args at index 2, check for a constraint vector at index impls_start.
     */
    TypeConstraint *type_param_constraints = NULL;
    uint8_t n_type_param_constraints = 0;
    
    if (call->as.list.len > impls_start) {
        Form *next_form = call->as.list.items[impls_start];
        if (next_form->tag == F_VEC && next_form->as.list.len > 0) {
            /* Check if this is a constraint vector by looking at the first item */
            /* If the first item is a list (F_LIST), it's likely [(Clone a) (Clone b)] */
            /* If the first item is a symbol (F_SYM), it might be a flat [Clone a Clone b] vector */
            bool is_constraint_vector = false;
            if (next_form->as.list.len > 0) {
                Form *first_item = next_form->as.list.items[0];
                if (first_item->tag == F_LIST) {
                    /* Vector of lists format: [(Clone a) (Clone b)] */
                    is_constraint_vector = true;
                    n_type_param_constraints = next_form->as.list.len;
                } else if (next_form->as.list.len >= 2) {
                    /* Could be flat format [Clone a Clone b ...] */
                    /* Check if all items alternate between SYM (typeclass) and SYM/KEYWORD (type arg) */
                    is_constraint_vector = true;
                    n_type_param_constraints = next_form->as.list.len / 2;
                }
            }
            
            if (is_constraint_vector && n_type_param_constraints > 0) {
                type_param_constraints = (TypeConstraint *)arena_alloc(
                    e->arena, n_type_param_constraints * sizeof(TypeConstraint));
                
                Form *first_item = next_form->as.list.items[0];
                if (first_item->tag == F_LIST) {
                    /* Parse as vector of lists: [(Clone a) (Clone b)] */
                    for (uint8_t i = 0; i < n_type_param_constraints; i++) {
                        Form *constraint_form = next_form->as.list.items[i];
                        if (constraint_form->tag != F_LIST || constraint_form->as.list.len < 1) {
                            diag_emit(DIAG_ERROR, constraint_form->span,
                                      "definstance: constraint must be a list like (Clone a), got tag %d with %d items",
                                      constraint_form->tag, constraint_form->as.list.len);
                            return NULL;
                        }
                        
                        Form *tc_name_form = constraint_form->as.list.items[0];
                        if (tc_name_form->tag != F_SYM) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass name must be a symbol");
                            return NULL;
                        }
                        
                        TypeClass *constraint_tc = typeclass_env_lookup_typeclass(
                            &e->typeclass_env, tc_name_form->as.sym);
                        if (!constraint_tc) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass '%s' is not defined",
                                      tc_name_form->as.sym->name);
                            return NULL;
                        }
                        
                        /* Type argument being constrained (optional, at index 1) */
                        Type constrained_type = TYPE_INT; /* Default */
                        if (constraint_form->as.list.len >= 2) {
                            Form *type_arg_form = constraint_form->as.list.items[1];
                            if (type_arg_form->tag == F_SYM) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        break;
                                    }
                                }
                                if (constrained_type.kind == TY_INT) {
                                    if (type_arg_form->as.sym->len == 3 &&
                                        memcmp(type_arg_form->as.sym->name, "int", 3) == 0) {
                                        constrained_type = TYPE_INT;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "bool", 4) == 0) {
                                        constrained_type = TYPE_BOOL;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "cstr", 4) == 0) {
                                        constrained_type = TYPE_CSTR;
                                    }
                                }
                            }
                        }
                        
                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type
                        };
                    }
                } else {
                    /* Parse as flat vector: [Clone a Clone b ...] */
                    uint8_t n_items = next_form->as.list.len;
                    if (n_items % 2 != 0) {
                        diag_emit(DIAG_ERROR, next_form->span,
                                  "definstance: flat constraint vector must have an even number of items");
                        return NULL;
                    }
                    n_type_param_constraints = n_items / 2;
                    /* Reallocate for flat format - old allocation will be GC'd with arena */
                    type_param_constraints = (TypeConstraint *)arena_alloc(
                        e->arena, n_type_param_constraints * sizeof(TypeConstraint));
                    
                    for (uint8_t i = 0; i < n_type_param_constraints; i++) {
                        uint8_t idx = i * 2;
                        Form *tc_name_form = next_form->as.list.items[idx];
                        if (tc_name_form->tag != F_SYM) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass name must be a symbol");
                            return NULL;
                        }
                        
                        TypeClass *constraint_tc = typeclass_env_lookup_typeclass(
                            &e->typeclass_env, tc_name_form->as.sym);
                        if (!constraint_tc) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass '%s' is not defined",
                                      tc_name_form->as.sym->name);
                            return NULL;
                        }
                        
                        Type constrained_type = TYPE_INT;
                        if (idx + 1 < n_items) {
                            Form *type_arg_form = next_form->as.list.items[idx + 1];
                            if (type_arg_form->tag == F_SYM) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        break;
                                    }
                                }
                                if (constrained_type.kind == TY_INT) {
                                    if (type_arg_form->as.sym->len == 3 &&
                                        memcmp(type_arg_form->as.sym->name, "int", 3) == 0) {
                                        constrained_type = TYPE_INT;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "bool", 4) == 0) {
                                        constrained_type = TYPE_BOOL;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "cstr", 4) == 0) {
                                        constrained_type = TYPE_CSTR;
                                    }
                                }
                            }
                        }
                        
                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type
                        };
                    }
                }
                impls_start++; /* Skip past the constraint vector */
            }
        }
    }

    /* Phase PTC2: Validate type parameter constraints */
    if (type_param_constraints && n_type_param_constraints > 0) {
        for (uint8_t i = 0; i < n_type_param_constraints; i++) {
            TypeClass *constraint_tc = type_param_constraints[i].typeclass;
            Type constrained_type = type_param_constraints[i].type_arg;
            bool is_primitive = (constrained_type.kind == TY_INT ||
                                 constrained_type.kind == TY_BOOL ||
                                 constrained_type.kind == TY_CSTR ||
                                 constrained_type.kind == TY_NIL ||
                                 constrained_type.kind == TY_FLOAT ||
                                 constrained_type.kind == TY_PTR_VOID);
            
            /* PTC2: For primitive types, validate that a constraint instance exists.
             * For user-defined types (structs, etc.), defer validation to PTC3.
             * Phase B1: float is treated as a primitive for constraint purposes. */
            if (is_primitive) {
                Type lookup_type = constrained_type;
                if (constrained_type.kind == TY_BOOL) {
                    lookup_type = TYPE_BOOL;
                } else if (constrained_type.kind == TY_CSTR) {
                    lookup_type = TYPE_CSTR;
                } else if (constrained_type.kind == TY_NIL) {
                    lookup_type = TYPE_NIL;
                } else if (constrained_type.kind == TY_PTR_VOID) {
                    lookup_type = TYPE_PTR_VOID;
                } else if (constrained_type.kind == TY_FLOAT) {
                    lookup_type = TYPE_FLOAT;
                }
                
                TypeClassInstance *inst = typeclass_env_lookup_instance(
                    &e->typeclass_env, constraint_tc, &lookup_type, 1);
                if (!inst) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                        "typeclass constraint not satisfied: no instance of '%s' for type '%s'",
                        constraint_tc->name->name, type_name(constrained_type));
                    return NULL;
                }
            }
            /* For user-defined types, the constraint is stored on the instance
             * but not validated here (deferred to PTC3 for constraint propagation). */
        }
    }

    /* Validate type argument count matches typeclass parameters */
    if (n_type_args != tc->n_type_params) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d type arguments for '%s', got %d",
                  tc->n_type_params, tc_name->name, n_type_args);
        return NULL;
    }

    /* Phase HKT H1: Kind constraint validation.
     * If the typeclass has kind-annotated parameters (type_param_kinds != NULL),
     * verify that each type argument satisfies the expected kind.
     * Primitive types (int, bool, cstr, nil, float) have kind *.
     * Struct types and other user-defined types are treated as kind * -> *.
     * A definstance that supplies a primitive where kind '* -> *' is expected
     * is a compile-time error (TUR-E0012). */
    if (tc->type_param_kinds != NULL) {
        for (uint8_t i = 0; i < n_type_args; i++) {
            Kind expected = tc->type_param_kinds[i];
            if (expected == KIND_ARROW || expected == KIND_ARROW2) {
                TypeKind tk = type_args[i].kind;
                bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                                     tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID);
                if (is_primitive) {
                    diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0012_KIND_MISMATCH,
                        "kind mismatch (TUR-E0012): typeclass '%s' parameter %d expects kind "
                        "'%s' (a type constructor), but '%s' has kind '*'",
                        tc_name->name, (int)(i + 1),
                        kind_to_string(expected),
                        type_name(type_args[i]));
                    return NULL;
                }
            }
        }
    }

    /* Parse method implementations */
    /* Each method impl is a function definition without the 'defn' keyword */
    /* Syntax: (method-name [param1 param2 ...] body...)
     * The number of methods must match the typeclass definition.
     */
    
    if (call->as.list.len - impls_start < tc->n_methods) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d method implementations for '%s', got %d",
                  tc->n_methods, tc_name->name, call->as.list.len - impls_start);
        return NULL;
    }
    
    /* For Phase 15 v1, we store method implementations as FnDef pointers.
     * In a full implementation, these would be stored in the instance and
     * codegen would generate dictionary structs. For now, we validate syntax.
     */
    FnDef **method_impls = NULL;
    
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        Form *impl_form = call->as.list.items[impls_start + i];
        if (impl_form->tag != F_LIST || impl_form->as.list.len < 3) {
            diag_emit(DIAG_ERROR, impl_form->span,
                      "method implementation requires (name [params...] body...)");
            return NULL;
        }
        
        /* Parse the method implementation as a function */
        /* For now, we just validate the name matches */
        Form *impl_name_form = impl_form->as.list.items[0];
        if (impl_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, impl_name_form->span,
                      "method implementation name must be a symbol");
            return NULL;
        }
        
        if (impl_name_form->as.sym != tc->methods[i].name) {
            diag_emit(DIAG_ERROR, impl_name_form->span,
                      "method implementation name '%s' doesn't match typeclass method '%s'",
                      impl_name_form->as.sym->name, tc->methods[i].name->name);
            return NULL;
        }
        
        /* Elaborate the method implementation as a function */
        /* The form is (method-name [params...] body...) */
        if (!method_impls) {
            method_impls = (FnDef **)arena_alloc(e->arena, tc->n_methods * sizeof(FnDef *));
        }
        
        /* Create a synthetic name for this method implementation */
        /* Format: __inst_<typeclass>_<method>_<typeargs> e.g. __inst_MyEq_eq_int */
        enum {
            MAX_INSTANCE_METHOD_NAME_LEN = 192,
            MAX_SANITIZED_METHOD_NAME_LEN = 64,
            MAX_INSTANCE_TYPE_SUFFIX_LEN = 64,
        };
        char method_name[MAX_INSTANCE_METHOD_NAME_LEN];
        
        /* Sanitize method name for C identifier (replace invalid chars with _) */
        char sanitized_method_name[MAX_SANITIZED_METHOD_NAME_LEN];
        const char *method_name_str = tc->methods[i].name->name;
        uint32_t method_name_len = tc->methods[i].name->len;
        if (method_name_len >= sizeof(sanitized_method_name)) {
            method_name_len = sizeof(sanitized_method_name) - 1;
        }
        memcpy(sanitized_method_name, method_name_str, method_name_len);
        sanitized_method_name[method_name_len] = '\0';
        for (char *p = sanitized_method_name; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_') {
                *p = '_';
            }
        }
        
        /* Build type arg suffix */
        char type_suffix[MAX_INSTANCE_TYPE_SUFFIX_LEN] = "";
        size_t type_suffix_len = 0;
        for (uint8_t j = 0; j < n_type_args; j++) {
            const char *type_component = NULL;
            char ctor_name_buf[32];  /* for TY_STRUCT/constructor names */
            switch (type_args[j].kind) {
                case TY_INT: type_component = "int"; break;
                case TY_BOOL: type_component = "bool"; break;
                case TY_CSTR: type_component = "cstr"; break;
                case TY_NIL: type_component = "nil"; break;
                case TY_PTR_VOID: type_component = "ptr_void"; break;
                case TY_STRUCT:
                    /* Phase HKT H3: use the original symbol name when available,
                     * falling back to "T" for unnamed struct type args. */
                    if (type_arg_syms && type_arg_syms[j]) {
                        /* Sanitise the symbol name to a valid C identifier component */
                        uint32_t sym_len = type_arg_syms[j]->len;
                        if (sym_len >= sizeof(ctor_name_buf))
                            sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                        memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                        ctor_name_buf[sym_len] = '\0';
                        for (char *p = ctor_name_buf; *p; p++) {
                            if (!isalnum((unsigned char)*p)) *p = '_';
                        }
                        type_component = ctor_name_buf;
                    } else if (type_args[j].as.struct_.def) {
                        type_component = type_args[j].as.struct_.def->name;
                    } else {
                        type_component = "T";
                    }
                    break;
                case TY_APP: {
                    /* Phase HKT §3: partial type application — encode as "ctor_arg" */
                    const char *ctor_part = "T";
                    const char *arg_part  = "T";
                    if (type_arg_syms && type_arg_syms[j]) {
                        ctor_part = type_arg_syms[j]->name;
                    }
                    if (type_args[j].as.app.arg) {
                        const char *n = type_name(*type_args[j].as.app.arg);
                        if (n) arg_part = n;
                    }
                    snprintf(ctor_name_buf, sizeof(ctor_name_buf), "%s_%s", ctor_part, arg_part);
                    for (char *p = ctor_name_buf; *p; p++) {
                        if (!isalnum((unsigned char)*p)) *p = '_';
                    }
                    type_component = ctor_name_buf;
                    break;
                }
                default: type_component = "T"; break;
            }
            int written = snprintf(type_suffix + type_suffix_len,
                                   sizeof(type_suffix) - type_suffix_len,
                                   "%s%s", j == 0 ? "_" : "", type_component);
            if (written < 0 || (size_t)written >= sizeof(type_suffix) - type_suffix_len) {
                diag_emit(DIAG_ERROR, impl_form->span,
                          "typeclass instance method name is too long");
                return NULL;
            }
            type_suffix_len += (size_t)written;
        }
        int method_name_written =
            snprintf(method_name, sizeof(method_name), "__inst_%.*s_%s%s",
                     (int)tc_name->len, tc_name->name, sanitized_method_name, type_suffix);
        if (method_name_written < 0 || (size_t)method_name_written >= sizeof(method_name)) {
            diag_emit(DIAG_ERROR, impl_form->span,
                      "typeclass instance method name is too long");
            return NULL;
        }
        
        const Symbol *method_sym = symtab_intern(e->st, 
            strslice(method_name, (uint32_t)strlen(method_name)));
        
        /* Parse the method implementation form */
        /* impl_form is (method-name [params...] :return-type body...) */
        /* or (method-name [params...] body...) if no return type */
        Form *impl_params_form = impl_form->as.list.items[1];
        uint32_t impl_body_start = 2;
        Type return_type = tc->methods[i].return_type;  /* Default from typeclass */
        
        /* Check for return type annotation after params */
        if (impl_form->as.list.len >= 3) {
            Form *ret_or_body = impl_form->as.list.items[2];
            if (ret_or_body->tag == F_KEYWORD) {
                /* This is a return type annotation */
                const Symbol *kw = ret_or_body->as.sym;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    return_type = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    return_type = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    return_type = TYPE_CSTR;
                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                    return_type = TYPE_NIL;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    return_type = TYPE_PTR_VOID;
                }
                impl_body_start = 3;
            }
        }
        
        /* Parse parameters */
        Binding **method_params = NULL;
        uint8_t n_method_params = 0;
        Type *method_param_types = NULL;
        
        if (impl_params_form->tag == F_VEC) {
            n_method_params = impl_params_form->as.list.len;
            if (n_method_params > 0) {
                method_params = (Binding **)arena_alloc(e->arena, 
                    n_method_params * sizeof(Binding *));
                method_param_types = (Type *)arena_alloc(e->arena, 
                    n_method_params * sizeof(Type));
                
                for (uint8_t j = 0; j < n_method_params; j++) {
                    Form *p = impl_params_form->as.list.items[j];
                    Type param_type = TYPE_INT;
                    
                    /* Phase 15: Try to use type from typeclass method definition */
                    if (tc->methods[i].param_types && j < tc->methods[i].n_params) {
                        param_type = tc->methods[i].param_types[j];
                    }
                    
                    /* Phase 15: Substitute type variables with type args */
                    /* For v1: if the param type is TYPE_INT (default) and we have type args,
                     * use the first type arg */
                    if (param_type.kind == TY_INT && n_type_args > 0) {
                        param_type = type_args[0];
                    }
                    
                    /* Phase HRT3: if the param type is TY_FORALL, treat it as a poly fn param.
                     * Phase CCL: also treat :fn-annotated params (param_is_fn) as poly fn. */
                    bool param_is_poly = (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                    if (!param_is_poly
                        && tc->methods[i].param_is_fn
                        && j < tc->methods[i].n_params
                        && tc->methods[i].param_is_fn[j]) {
                        param_is_poly = true;
                        param_type = TYPE_PTR_VOID;
                    }
                    Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                    if (p->tag == F_SYM) {
                        /* Simple parameter name */
                        method_params[j] = binding_new(e, p->as.sym, c_param_type, false, false, p->span);
                        if (param_is_poly) {
                            method_params[j]->is_poly_fn = true;
                            Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *pt = param_type;
                            method_params[j]->poly_type = pt;
                        }
                        method_param_types[j] = c_param_type;
                    } else if (p->tag == F_VEC && p->as.list.len >= 1) {
                        /* Parameter with type annotation: [name : type] */
                        Form *name_f = p->as.list.items[0];
                        if (name_f->tag == F_SYM) {
                            /* Check for type annotation */
                            if (p->as.list.len >= 2 && p->as.list.items[1]->tag == F_KEYWORD) {
                                const Symbol *kw = p->as.list.items[1]->as.sym;
                                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                    param_type = TYPE_INT;
                                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                    param_type = TYPE_BOOL;
                                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                    param_type = TYPE_CSTR;
                                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                                    param_type = TYPE_NIL;
                                }
                            }
                            /* Phase CCL: :fn-annotated params also become poly fn */
                            if (!param_is_poly
                                && tc->methods[i].param_is_fn
                                && j < tc->methods[i].n_params
                                && tc->methods[i].param_is_fn[j]) {
                                param_is_poly = true;
                                param_type = TYPE_PTR_VOID;
                            }
                            param_is_poly = param_is_poly
                                || (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                            c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                            method_params[j] = binding_new(e, name_f->as.sym, c_param_type, false, false, p->span);
                            if (param_is_poly) {
                                method_params[j]->is_poly_fn = true;
                                Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *pt = param_type;
                                method_params[j]->poly_type = pt;
                            }
                            method_param_types[j] = c_param_type;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "method parameter name must be a symbol");
                            return NULL;
                        }
                    } else {
                        diag_emit(DIAG_ERROR, p->span,
                                  "method parameter must be a symbol or vector");
                        return NULL;
                    }
                }
            }
        }
        
        /* Elaborate the body - push a scope with method parameters */
        Scope method_scope;
        scope_init(&method_scope, e->scope);
        e->scope = &method_scope;

        /* Add method parameters to scope */
        for (uint8_t j = 0; j < n_method_params; j++) {
            scope_add(&method_scope, method_params[j]);
        }

        /* ER3: Increment fn_body_depth so that (perform ...) inside an instance
         * method body does not trigger TUR-E0008 (unhandled effect at top level).
         * The handler is expected to be provided at the call site. */
        e->fn_body_depth++;

        Expr *method_body = e_nil(e, impl_form->span);
        uint32_t n_body = impl_form->as.list.len - impl_body_start;
        if (n_body > 0) {
            if (n_body == 1) {
                method_body = elab_form(e, impl_form->as.list.items[impl_body_start]);
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
                for (uint32_t k = 0; k < n_body; k++) {
                    items[k] = elab_form(e, impl_form->as.list.items[impl_body_start + k]);
                    if (!items[k]) { e->fn_body_depth--; e->scope = method_scope.parent; scope_free(&method_scope); return NULL; }
                }
                method_body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, impl_form->span);
                method_body->as.do_.items = items;
                method_body->as.do_.n = n_body;
            }
        }

        e->fn_body_depth--;

        /* Pop method scope */
        e->scope = method_scope.parent;
        scope_free(&method_scope);
        
        /* Create a proper function type for the method */
        TypeKind param_kinds[MAX_FN_ARITY];
        for (uint8_t j = 0; j < n_method_params; j++) {
            param_kinds[j] = method_param_types[j].kind;
        }
        Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);
        
        /* Create FnDef for the method implementation */
        FnDef *method_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
        Binding *method_binding = binding_new(e, method_sym, fn_type, false, true, impl_form->span);
        method_fd->binding = method_binding;
        method_fd->params = method_params;
        method_fd->n_params = n_method_params;
        method_fd->body = method_body;
        method_fd->is_variadic = false;
        method_fd->closure = NULL;
        method_fd->param_types = method_param_types;
        method_fd->may_capture = false;
        method_fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
        constraint_set_init(&method_fd->constraints);
        
        /* Register the method function at file scope */
        scope_add(&e->global, method_binding);
        
        /* Create a file-scope definition expression */
        Expr *method_def_expr = expr_new(e->arena, EX_FN_DEF, fn_type, impl_form->span);
        method_def_expr->as.fn_def_.fn = method_fd;
        elab_register_file_def(e, method_def_expr);
        
        method_impls[i] = method_fd;
    }
    
    /* Register the instance */
    TypeClassInstance *inst = typeclass_env_register_instance(&e->typeclass_env, tc);
    if (!inst) {
        diag_emit(DIAG_ERROR, call->span,
                  "failed to register instance for '%s'", tc_name->name);
        return NULL;
    }
    
    inst->type_args = type_args;
    inst->n_type_args = n_type_args;
    inst->type_arg_syms = type_arg_syms;  /* Phase HKT §1: store for dict naming */
    inst->method_impls = method_impls;
    inst->n_method_impls = tc->n_methods;
    /* Phase PTC1: Store type parameter constraints */
    inst->type_param_constraints = type_param_constraints;
    inst->n_type_param_constraints = n_type_param_constraints;
    /* Phase HKT-P4: record the file that defined this instance. */
    inst->origin_file_id = call->span.file_id;

    /* Phase HKT-P4: Orphan instance check.
     *
     * Rule: an instance is "orphan" when NEITHER the typeclass NOR any
     * struct-type type argument was defined in the current compilation unit.
     * In Rust terms: you may only define Foo<Bar> if you own Foo or Bar.
     *
     * Now a hard DIAG_ERROR since the module system (P19-6) has landed. */
    if (tc->origin_file_id != 0 && tc->origin_file_id != call->span.file_id) {
        /* The typeclass is from a different file.
         * Check if any struct type-arg was defined here. */
        bool owns_a_type_arg = false;
        for (uint8_t i = 0; i < n_type_args && !owns_a_type_arg; i++) {
            if (type_args[i].kind == TY_STRUCT && type_args[i].as.struct_.def) {
                if (type_args[i].as.struct_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
            }
        }
        if (!owns_a_type_arg) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                      TUR_E0013_ORPHAN_INSTANCE,
                      "orphan instance: typeclass '%s' is defined in a different "
                      "module and none of the type arguments belong to this module; "
                      "move the instance to the module that defines the typeclass or "
                      "one of the type arguments",
                      tc_name->name);
        }
    }
    
    /* Create an INSTANCE_DEF expression for codegen */
    Expr *inst_expr = expr_new(e->arena, EX_INSTANCE_DEF, TYPE_NIL, call->span);
    inst_expr->as.instance_def_.instance = inst;
    elab_register_file_def(e, inst_expr);
    
    /* Create a nil expression as the result (definstance returns nothing) */
    return e_nil(e, call->span);
}

/* Phase 15: Elaborate (.method obj arg1 arg2 ...) - typeclass method call
 * 
 * Syntax: (.method obj arg1 arg2 ...)
 * Looks up the method in the typeclass for the type of obj, and generates a call.
 * For v1, we use direct method function calls (monomorphic only).
 * Full dictionary passing deferred to v2.
 */
/* Phase 12: EX_GET_FIELD — struct field access via (.fieldname s)
 *
 * Syntax: (.field s)  where s has type TY_STRUCT
 * Returns: the type of the named field
 * Also resolves immutable/mutable borrow of a field:
 *   (& (.field s))   → EX_BORROW_IMMUT wrapping EX_GET_FIELD
 *   (&mut (.field s)) → EX_BORROW_MUT wrapping EX_GET_FIELD
 */

/* Phase H §1: Build an EX_DICT node for a typeclass instance singleton.
 * The dict_name field is computed from the instance's typeclass and type args
 * using the same naming convention as emit.c (emit_dict_name / EX_INSTANCE_DEF).
 * Returns a TY_PTR_VOID-typed Expr that, when emitted, yields the address of
 * the global dictionary singleton cast to int64_t. */
static Expr *make_dict_expr(Elab *e, TypeClassInstance *inst, Span span) {
    Expr *d = expr_new(e->arena, EX_DICT, type_from_kind(TY_PTR_VOID), span);
    d->as.dict_.instance = inst;

    /* Compute dict_name: "dict_<TypeClass>_<typearg>..." */
    const TypeClass *tc = inst->typeclass;
    char *dst = d->as.dict_.dict_name;
    size_t dstlen = sizeof(d->as.dict_.dict_name);
    char type_suffix[64] = "";
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_STRUCT:
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.struct_.def &&
                         inst->type_args[i].as.struct_.def->name)
                    component = inst->type_args[i].as.struct_.def->name;
                break;
            default: break;
        }
        char comp_buf[32];
        strncpy(comp_buf, component, sizeof(comp_buf) - 1);
        comp_buf[sizeof(comp_buf) - 1] = '\0';
        for (char *p = comp_buf; *p; p++) {
            if (!isalnum((unsigned char)*p)) *p = '_';
        }
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(dst, dstlen, "dict_%s%s", tc->name->name, type_suffix);
    return d;
}

Expr *elab_method_call(Elab *e, const Form *call) {
    /* call is (.method obj arg1 arg2 ...)
     * call->as.list.items[0] is the symbol .method
     */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "method call requires (.method obj arg1 ...)");
        return NULL;
    }
    
    /* Parse method name from the symbol (skip the leading '.') */
    Form *head = call->as.list.items[0];
    const Symbol *method_sym = head->as.sym;
    const char *method_name = method_sym->name + 1;  /* Skip '.' */
    uint32_t method_name_len = method_sym->len - 1;

    /* Phase D1: Type witness @TypeName at call sites.
     * The reader converts @TypeName into (deref TypeName), so we detect
     * the pattern (deref sym) at items[1] where sym names a registered
     * typeclass instance type argument for this method.  When found the
     * witness pins the dispatch directly to that instance; the receiver
     * is items[2] and extra arguments start at items[3]. */
    if (call->as.list.len >= 3 &&
        call->as.list.items[1]->tag == F_LIST &&
        call->as.list.items[1]->as.list.len == 2 &&
        call->as.list.items[1]->as.list.items[0]->tag == F_SYM &&
        strcmp(call->as.list.items[1]->as.list.items[0]->as.sym->name, "deref") == 0 &&
        call->as.list.items[1]->as.list.items[1]->tag == F_SYM) {

        const Symbol *witness_sym = call->as.list.items[1]->as.list.items[1]->as.sym;
        const char   *witness_name = witness_sym->name;
        uint32_t      witness_len  = witness_sym->len;

        /* Walk all registered instances looking for one whose type arg symbol
         * (or primitive type name) matches the witness identifier. */
        TypeClassInstance *witness_inst   = NULL;
        FnDef             *witness_method_fn = NULL;
        bool               any_inst_for_method = false;

        for (TypeClassInstance *inst = e->typeclass_env.instances;
             inst != NULL && !witness_inst; inst = inst->next) {
            for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                const TypeClassMethod *m = &inst->typeclass->methods[mi];
                if (m->name->len != method_name_len ||
                    memcmp(m->name->name, method_name, method_name_len) != 0) continue;
                any_inst_for_method = true;
                /* Check if a type arg name matches the witness identifier. */
                bool name_match = false;
                for (uint8_t ti = 0; ti < inst->n_type_args && !name_match; ti++) {
                    if (inst->type_arg_syms && inst->type_arg_syms[ti] &&
                        inst->type_arg_syms[ti]->len == witness_len &&
                        memcmp(inst->type_arg_syms[ti]->name, witness_name, witness_len) == 0) {
                        name_match = true;
                    }
                    if (!name_match) {
                        /* Primitive type names that have no symbol (e.g. int, bool). */
                        const char *prim = NULL;
                        switch (inst->type_args[ti].kind) {
                            case TY_INT:   prim = "int";   break;
                            case TY_BOOL:  prim = "bool";  break;
                            case TY_CSTR:  prim = "cstr";  break;
                            case TY_NIL:   prim = "nil";   break;
                            case TY_FLOAT: prim = "float"; break;
                            default: break;
                        }
                        if (prim && strcmp(prim, witness_name) == 0) name_match = true;
                    }
                }
                if (name_match) {
                    witness_inst      = inst;
                    witness_method_fn = inst->method_impls[mi];
                }
                break; /* one method match per instance is enough */
            }
        }

        if (witness_inst) {
            /* Witness resolved: receiver is items[2], extra args are items[3..]. */
            Expr *obj_w = elab_form(e, call->as.list.items[2]);
            if (!obj_w) return NULL;

            uint32_t n_args_w = call->as.list.len - 3;
            Expr **args_w = (Expr **)arena_alloc(e->arena, n_args_w * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args_w; i++) {
                args_w[i] = elab_form(e, call->as.list.items[3 + i]);
                if (!args_w[i]) return NULL;
            }

            /* Determine result type from the method's binding. */
            Type result_type_w;
            if (witness_method_fn->binding->type.kind == TY_FN) {
                result_type_w = type_from_kind(witness_method_fn->binding->type.as.fn.result_kind);
            } else {
                result_type_w = witness_method_fn->body ? witness_method_fn->body->type : TYPE_INT;
                if (result_type_w.kind == TY_UNKNOWN || result_type_w.kind == TY_NIL)
                    result_type_w = TYPE_INT;
            }

            /* Build the EX_DICT node for the pinned instance. */
            Expr *dict_w = make_dict_expr(e, witness_inst, call->span);
            strncpy(dict_w->as.dict_.method_name, method_name,
                    sizeof(dict_w->as.dict_.method_name) - 1);
            dict_w->as.dict_.method_name[sizeof(dict_w->as.dict_.method_name) - 1] = '\0';
            for (char *p = dict_w->as.dict_.method_name; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
            }

            /* Build EX_CALL: args array is [obj_w, args_w...]. */
            Expr **call_args_w = (Expr **)arena_alloc(e->arena,
                                                       (n_args_w + 1) * sizeof(Expr *));
            call_args_w[0] = obj_w;
            for (uint32_t i = 0; i < n_args_w; i++) call_args_w[i + 1] = args_w[i];

            Expr *out_w = expr_new(e->arena, EX_CALL, result_type_w, call->span);
            out_w->as.call_.fn_binding = NULL;
            out_w->as.call_.fn_expr    = dict_w;
            out_w->as.call_.args       = call_args_w;
            out_w->as.call_.n_args     = n_args_w + 1;
            out_w->as.call_.dict_arg   = dict_w;
            return out_w;

        } else if (any_inst_for_method) {
            /* Instances exist for this method but none match the witness type name.
             * The user clearly intended a witness (not a deref); emit a specific error. */
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "no instance of typeclass method '.%.*s' for type '%s' -- "
                      "check that (definstance ... [%s] ...) is in scope",
                      (int)method_name_len, method_name, witness_name, witness_name);
            return NULL;
        }
        /* No instances at all for this method: fall through to the normal path
         * so that (deref sym) is elaborated as the receiver and the existing
         * "no typeclass method found" error is emitted. */
    }

    /* Phase HKT H2: Elaborate the receiver object first so we can use its
     * type to select the correct typeclass instance (type-based dispatch).
     * This replaces the old "name-only / first-match" approach with a lookup
     * that distinguishes multiple instances of the same typeclass for
     * different types (e.g. MyShow[int] vs MyShow[bool]). */
    Expr *obj = elab_form(e, call->as.list.items[1]);
    if (!obj) return NULL;

    /* Phase 12: EX_GET_FIELD — if the form is exactly (.field s) with no extra
     * args, try to resolve it as a struct field access first. */
    if (call->as.list.len == 2) {
        /* Unwrap borrow types */
        Type base = obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
        }
        if (base.kind == TY_STRUCT) {
            /* Object is a struct — try field lookup */
            StructDef *def = base.as.struct_.def;
            for (uint32_t i = 0; i < def->n_fields; i++) {
                if (strcmp(def->fields[i].name, method_name) == 0) {
                    /* Found matching field — build EX_GET_FIELD */
                    TypeKind fkind = def->fields[i].kind;
                    TypeKind finner = def->fields[i].inner_kind;
                    Type field_type;
                    if (fkind == TY_REF || fkind == TY_LREF || fkind == TY_RC || fkind == TY_WEAK) {
                        field_type.kind = fkind;
                        field_type.copy_kind = typekind_default_copy_kind(fkind);
                        field_type.as.ref.inner = finner;
                    } else {
                        field_type = type_from_kind(fkind);
                    }
                    Expr *out = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
                    out->as.get_field_.struct_expr = obj;
                    out->as.get_field_.field_idx = i;
                    out->as.get_field_.def = def;
                    /* LT1/T3: Extracting an lref<T> field from a :move struct transfers
                     * linear ownership of that field out of the struct.  Mark the struct
                     * binding as moved so a second extraction of the same field triggers
                     * TUR_E0005 (use-after-move).  :linear struct receivers are already
                     * handled by the F_SYM is_linear_consumed path above. */
                    if (g_linear_enabled && fkind == TY_LREF &&
                            obj->kind == EX_VAR && type_is_move(obj->as.var.binding->type)) {
                        binding_mark_moved(obj->as.var.binding, call->span);
                    }
                    return out;
                }
            }
            /* Struct but no matching field — fall through to typeclass method lookup */
            /* In Phase PTC4, this allows (.method obj) to dispatch to typeclass
             * methods even when obj is a struct that doesn't have that field. */
        }
    }

    /* Phase 16 v2: capability field call — (.field-name cap arg1 arg2 ...)
     * When the receiver is a struct and the named field is :fn (TY_FN), and
     * there are arguments, emit an indirect function-pointer call through the
     * field. The call carries the EX_GET_FIELD as fn_expr for effect-row
     * propagation. Effect rows on the field are advisory in v1 (codegen erases
     * to a plain function pointer call). */
    if (call->as.list.len > 2) {
        Type base = obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
        }
        if (base.kind == TY_STRUCT) {
            StructDef *def = base.as.struct_.def;
            for (uint32_t i = 0; i < def->n_fields; i++) {
                if (strcmp(def->fields[i].name, method_name) == 0 &&
                    def->fields[i].kind == TY_FN) {
                    /* Build EX_GET_FIELD for the function pointer */
                    Type field_type = type_from_kind(TY_FN);
                    Expr *get_field = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
                    get_field->as.get_field_.struct_expr = obj;
                    get_field->as.get_field_.field_idx = i;
                    get_field->as.get_field_.def = def;

                    /* Elaborate arguments */
                    uint32_t n_args = call->as.list.len - 2;
                    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
                    for (uint32_t j = 0; j < n_args; j++) {
                        args[j] = elab_form(e, call->as.list.items[2 + j]);
                        if (!args[j]) return NULL;
                    }

                    /* Build indirect EX_CALL through fn_expr */
                    Expr *call_out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
                    call_out->as.call_.fn_binding = NULL;
                    call_out->as.call_.fn_expr = get_field;
                    call_out->as.call_.args = args;
                    call_out->as.call_.n_args = n_args;
                    return call_out;
                }
            }
        }
    }

    /* Phase 16 v2 fallback: when receiver has TY_UNKNOWN or TY_INT type
     * (untyped parameters default to TY_INT), search all registered struct defs
     * for a :fn field matching the method name.
     * This allows (.method-name cap args...) when cap has no explicit type annotation,
     * as long as exactly one struct in scope has a TY_FN field with that name. */
    if (call->as.list.len > 2 &&
        (obj->type.kind == TY_UNKNOWN || obj->type.kind == TY_INT)) {
        StructDef *matched_def = NULL;
        uint32_t matched_idx = 0;
        bool ambiguous = false;
        for (uint32_t sd = 0; sd < e->n_struct_defs; sd++) {
            StructDef *sdef = e->struct_defs[sd];
            for (uint32_t i = 0; i < sdef->n_fields; i++) {
                if (sdef->fields[i].kind == TY_FN &&
                    strlen(sdef->fields[i].name) == method_name_len &&
                    memcmp(sdef->fields[i].name, method_name, method_name_len) == 0) {
                    if (matched_def != NULL && matched_def != sdef) {
                        ambiguous = true;
                    } else {
                        matched_def = sdef;
                        matched_idx = i;
                    }
                }
            }
        }
        if (matched_def && !ambiguous) {
            Type field_type = type_from_kind(TY_FN);
            Expr *get_field = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
            get_field->as.get_field_.struct_expr = obj;
            get_field->as.get_field_.field_idx = matched_idx;
            get_field->as.get_field_.def = matched_def;

            uint32_t n_args = call->as.list.len - 2;
            Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
            for (uint32_t j = 0; j < n_args; j++) {
                args[j] = elab_form(e, call->as.list.items[2 + j]);
                if (!args[j]) return NULL;
            }

            Expr *call_out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
            call_out->as.call_.fn_binding = NULL;
            call_out->as.call_.fn_expr = get_field;
            call_out->as.call_.args = args;
            call_out->as.call_.n_args = n_args;
            return call_out;
        }
    }

    /* IT4: Typeclass intersection dispatch on union types.
     * When obj : (A | B), and every member type has an instance for .method,
     * generate a tag-dispatched EX_MATCH that calls the right instance per arm.
     * The synthetic scrutinee is `obj`; each arm unboxes the value and calls the
     * per-member method implementation directly (bypassing dictionary dispatch
     * to avoid nested tur_tagged_t complications). */
    if (obj->type.kind == TY_UNION) {
        uint8_t n_members = obj->type.as.union_.n_members;
        /* Elaborate the extra arguments once (they are shared across arms). */
        uint32_t n_extra = call->as.list.len - 2;
        Expr **extra_args = (Expr **)arena_alloc(e->arena, n_extra * sizeof(Expr *));
        for (uint32_t i = 0; i < n_extra; i++) {
            extra_args[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!extra_args[i]) return NULL;
        }

        /* For each member, find a matching typeclass instance. */
        FnDef **member_methods = (FnDef **)arena_alloc(e->arena, n_members * sizeof(FnDef *));
        for (uint8_t um = 0; um < n_members; um++) {
            Type *mem_t = obj->type.as.union_.members[um];
            if (!mem_t) { member_methods[um] = NULL; continue; }
            FnDef *found = NULL;
            for (TypeClassInstance *inst = e->typeclass_env.instances;
                 inst != NULL && !found; inst = inst->next) {
                for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                    const TypeClassMethod *meth = &inst->typeclass->methods[mi];
                    if (meth->name->len != method_name_len ||
                        memcmp(meth->name->name, method_name, method_name_len) != 0) continue;
                    if (inst->n_type_args > 0 &&
                        inst->type_args[0].kind != mem_t->kind) continue;
                    found = inst->method_impls[mi];
                    break;
                }
            }
            member_methods[um] = found;
            if (!found) {
                diag_emit(DIAG_ERROR, call->span,
                          "typeclass method '%.*s' not available for union member '%s'",
                          (int)method_name_len, method_name, type_name(*mem_t));
                return NULL;
            }
        }

        /* Determine result type from the first member's method. */
        Type result_type = TYPE_NIL;
        if (n_members > 0 && member_methods[0]) {
            FnDef *m0 = member_methods[0];
            if (m0->binding->type.kind == TY_FN)
                result_type = type_from_kind(m0->binding->type.as.fn.result_kind);
            else if (m0->body)
                result_type = m0->body->type;
        }
        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_INT;

        /* Build a fresh binding name for the unboxed arm variable. */
        static uint32_t union_dispatch_ctr = 0;
        char arm_name_buf[32];
        snprintf(arm_name_buf, sizeof(arm_name_buf), "__udisp_%u", union_dispatch_ctr++);
        const Symbol *arm_sym = intern_cstr(e->st, arm_name_buf);

        /* Build the arms array. */
        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_members * sizeof(MatchArm));
        for (uint8_t um = 0; um < n_members; um++) {
            Type *mem_t = obj->type.as.union_.members[um];
            FnDef *meth = member_methods[um];

            /* Pattern: type-narrowing, binds arm_sym to the unboxed value. */
            MatchArm *arm = &arms[um];
            memset(arm, 0, sizeof(*arm));
            arm->pattern.is_var = true;
            arm->pattern.var_sym = arm_sym;
            arm->pattern.union_member_idx = (int)um;
            arm->pattern.n_bindings = 1;
            arm->pattern.bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
            Binding *var_b = binding_new(e, arm_sym, *mem_t, false, false, call->span);
            arm->pattern.bindings[0] = var_b;
            arm->pattern.var_binding = var_b;
            arm->guard = NULL;

            /* Body: call meth with (var_b, extra_args...) */
            Expr *var_expr = expr_new(e->arena, EX_VAR, *mem_t, call->span);
            var_expr->as.var.binding = var_b;

            uint32_t total_args = 1 + n_extra;
            Expr **call_args = (Expr **)arena_alloc(e->arena, total_args * sizeof(Expr *));
            call_args[0] = var_expr;
            for (uint32_t ei = 0; ei < n_extra; ei++) call_args[1 + ei] = extra_args[ei];

            Expr *body_call = expr_new(e->arena, EX_CALL, result_type, call->span);
            body_call->as.call_.fn_binding = meth->binding;
            body_call->as.call_.fn_expr = NULL;
            body_call->as.call_.args = call_args;
            body_call->as.call_.n_args = total_args;
            arm->body = body_call;
        }

        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = obj;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_members;
        return out;
    }

    /* Phase HKT H2: Type-based instance lookup.
     * Build a TypeClassDispatchKey from the obj type, then use
     * typeclass_env_lookup_instance_by_key for a two-level search.
     * Fall back to name-only search if the type-based lookup yields nothing
     * (e.g. TY_UNKNOWN during forward-reference elaboration). */
    FnDef *best_method = NULL;
    /* Phase H §1: Track the selected instance so we can build an EX_DICT node. */
    TypeClassInstance *best_inst = NULL;
    /* Phase D0: count fallback candidates and track whether an exact match was found. */
    int fallback_count = 0;
    bool exact_match_found = false;

    /* Determine the effective constructor kind from the obj type. */
    Kind obj_ck = KIND_STAR;
    {
        TypeKind tk = obj->type.kind;
        bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                             tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID ||
                             tk == TY_UNKNOWN);
        obj_ck = is_primitive ? KIND_STAR : KIND_ARROW;
    }

    /* Search instances — prefer the one whose type_args[0] matches obj's type. */
    for (TypeClassInstance *inst = e->typeclass_env.instances; inst != NULL; inst = inst->next) {
        for (uint8_t i = 0; i < inst->typeclass->n_methods; i++) {
            const TypeClassMethod *method = &inst->typeclass->methods[i];
            if (method->name->len != method_name_len ||
                memcmp(method->name->name, method_name, method_name_len) != 0) {
                continue;
            }
            /* Name matched.  Now check if this instance's first type_arg
             * matches the obj type.  For KIND_STAR we compare TypeKind
             * exactly; for KIND_ARROW we accept any non-primitive. */
            if (inst->n_type_args > 0 && obj->type.kind != TY_UNKNOWN) {
                bool type_ok;
                if (obj_ck == KIND_STAR) {
                    type_ok = (inst->type_args[0].kind == obj->type.kind);
                } else {
                    /* KIND_ARROW: accept non-primitive instance type_args */
                    TypeKind itk = inst->type_args[0].kind;
                    bool inst_is_primitive =
                        (itk == TY_INT  || itk == TY_BOOL || itk == TY_CSTR ||
                         itk == TY_NIL  || itk == TY_FLOAT || itk == TY_PTR_VOID);
                    type_ok = !inst_is_primitive;
                }
                if (!type_ok) {
                    /* Record as fallback but keep searching. */
                    fallback_count++;
                    if (!best_method) { best_method = inst->method_impls[i]; best_inst = inst; }
                    continue;
                }
            }
            /* Phase PTC3: Check type parameter constraints on this instance.
             * If the instance has constraints, verify they are satisfied before
             * selecting it. For v1, we use the obj type as the only type argument.
             * Phase PTC4 will handle full parameterized type matching. */
            if (inst->type_param_constraints && inst->n_type_param_constraints > 0) {
                Type obj_type = obj->type;
                if (!typeclass_instance_constraints_satisfied(inst, &obj_type, 1, &e->typeclass_env)) {
                    /* Constraints not satisfied - skip this instance */
                    continue;
                }
            }
            /* Good match (or no type_args to check). */
            best_method = inst->method_impls[i];
            best_inst = inst;
            exact_match_found = true;
            goto found_method;
        }
    }
found_method:;
    
    if (!best_method) {
        /* No matching method found */
        diag_emit(DIAG_ERROR, call->span,
                  "no typeclass method found for '%.*s'",
                  method_name_len, method_name);
        return NULL;
    }

    /* Phase D0: Ambiguous dispatch diagnostic.
     * If we reached here via the fallback path (no exact type match) and
     * more than one instance matched by name, emit TUR_E0020 so the user
     * gets a clear error instead of a silent wrong-instance selection. */
    if (!exact_match_found && fallback_count > 1) {
        /* Build a comma-separated list of matching instance names for the message. */
        char inst_list[512];
        int pos = 0;
        int listed = 0;
        for (TypeClassInstance *ci = e->typeclass_env.instances;
             ci != NULL && pos < (int)sizeof(inst_list) - 2; ci = ci->next) {
            for (uint8_t mi = 0; mi < ci->typeclass->n_methods; mi++) {
                const TypeClassMethod *cm = &ci->typeclass->methods[mi];
                if (cm->name->len != method_name_len ||
                    memcmp(cm->name->name, method_name, method_name_len) != 0) continue;
                if (listed > 0 && pos < (int)sizeof(inst_list) - 3) {
                    inst_list[pos++] = ','; inst_list[pos++] = ' ';
                }
                int wrote = 0;
                if (ci->n_type_args > 0 && ci->type_arg_syms && ci->type_arg_syms[0]) {
                    wrote = snprintf(inst_list + pos, sizeof(inst_list) - (size_t)pos,
                                     "%s[%s]", ci->typeclass->name->name,
                                     ci->type_arg_syms[0]->name);
                } else {
                    wrote = snprintf(inst_list + pos, sizeof(inst_list) - (size_t)pos,
                                     "%s[?]", ci->typeclass->name->name);
                }
                if (wrote > 0) pos += wrote;
                listed++;
                break; /* one method match per instance is enough */
            }
        }
        inst_list[pos] = '\0';
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0020_AMBIGUOUS_DISPATCH,
                            "ambiguous method dispatch: '.%.*s' matches %d instances "
                            "(%s) -- receiver type is erased (int64_t). "
                            "Hint: annotate the receiver's type or use @TypeName syntax (see D1).",
                            (int)method_name_len, method_name, fallback_count, inst_list);
        return NULL;
    }

    /* obj was already elaborated above for dispatch; elaborate the remaining args. */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[2 + i]);
        if (!args[i]) return NULL;
    }

    /* Phase HRT3/HRT4: For methods with rank-N (poly fn) parameters, wrap matching args
     * as EX_POLY_WRAP so they can be passed as tur_poly_fn_t.
     * params[0] is the receiver (obj), so method param i+1 matches arg i.
     * Phase HRT4: if the arg is already is_poly_fn, use pass-through (wrapper_binding=NULL). */
    bool has_poly_params = false;
    /* Check params[0] which corresponds to obj (the first/receiver argument). */
    if (best_method->n_params > 0 && best_method->params[0]->is_poly_fn) {
        has_poly_params = true;
        Binding *inner_b = poly_arg_fn_binding(obj);
        if (!inner_b) {
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "rank-N typeclass method argument must be a named function");
            return NULL;
        }
        Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, obj->span);
        wrap->as.poly_wrap_.inner = obj;
        if (inner_b->is_poly_fn) {
            wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
        } else {
            uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                ? (uint8_t)inner_b->type.as.fn.arity : 1;
            Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, obj->span);
            if (!wrapper_b) return NULL;
            wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
        }
        obj = wrap;
    }
    for (uint32_t i = 0; i < n_args; i++) {
        uint8_t param_idx = 1 + (uint8_t)i;  /* params[0] is the receiver */
        if (param_idx < best_method->n_params && best_method->params[param_idx]->is_poly_fn) {
            has_poly_params = true;
            Binding *inner_b = poly_arg_fn_binding(args[i]);
            if (!inner_b) {
                /* Phase CCL: no named-function binding found.  If the argument
                 * is a fat closure (TY_PTR_VOID — capturing or non-capturing
                 * lambda), wrap it for tur_poly_fn_t packing in the emitter. */
                if (args[i]->type.kind == TY_PTR_VOID) {
                    Expr *orig2 = args[i];
                    Expr *cwrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig2->span);
                    cwrap->as.poly_wrap_.inner = orig2;
                    cwrap->as.poly_wrap_.wrapper_binding = NULL;
                    cwrap->as.poly_wrap_.is_closure = true;
                    args[i] = cwrap;
                    continue;
                }
                diag_emit(DIAG_ERROR, call->as.list.items[2 + i]->span,
                          "rank-N typeclass method argument must be a named function or closure");
                return NULL;
            }
            Expr *orig = args[i];
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig->span);
            wrap->as.poly_wrap_.inner = orig;
            if (inner_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
            } else {
                uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                    ? (uint8_t)inner_b->type.as.fn.arity : 1;
                Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            args[i] = wrap;
        }
    }

    /* Allocate arguments array with obj prepended */
    Expr **call_args = (Expr **)arena_alloc(e->arena, (n_args + 1) * sizeof(Expr *));
    call_args[0] = obj;
    for (uint32_t i = 0; i < n_args; i++) {
        call_args[i + 1] = args[i];
    }
    
    /* Create a call to the method function */
    /* The result type is the return type of the method.
     * For inline-C bodies the body type is TYPE_NIL, so prefer the
     * declared return type from the method's binding function type. */
    Type result_type;
    if (best_method->binding->type.kind == TY_FN) {
        result_type = type_from_kind(best_method->binding->type.as.fn.result_kind);
    } else {
        result_type = best_method->body->type;
        if (result_type.kind == TY_UNKNOWN || result_type.kind == TY_NIL) {
            result_type = TYPE_INT;
        }
    }
    
    /* Phase H §1 (dict load): Build an EX_DICT node that carries both the
     * singleton identity AND the method field name.  When fn_expr is this
     * node, emit.c dispatches through the dictionary struct at the call site:
     *   dict_<Class>_<type>_singleton.<method>(args...)
     * instead of a direct call to the impl function.  Fall back to the direct
     * binding if, for some reason, no instance was resolved. */
    Expr *dict_expr = NULL;
    if (best_inst) {
        dict_expr = make_dict_expr(e, best_inst, call->span);
        /* Copy the method name into the EX_DICT node and sanitize for C. */
        strncpy(dict_expr->as.dict_.method_name, method_name,
                sizeof(dict_expr->as.dict_.method_name) - 1);
        dict_expr->as.dict_.method_name[sizeof(dict_expr->as.dict_.method_name) - 1] = '\0';
        for (char *p = dict_expr->as.dict_.method_name; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
        }
    }

    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    if (dict_expr && !has_poly_params) {
        /* Dictionary dispatch: indirect call through the vtable field. */
        out->as.call_.fn_binding = NULL;
        out->as.call_.fn_expr    = dict_expr;
    } else {
        /* Direct call: either no instance resolved, or method has rank-N (poly fn)
         * params that require tur_poly_fn_t calling convention — dictionary dispatch
         * doesn't support tur_poly_fn_t params, so bypass it. */
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    }
    out->as.call_.args    = call_args;
    out->as.call_.n_args  = n_args + 1;
    out->as.call_.dict_arg = dict_expr;  /* annotation for downstream passes */
    return out;
}
