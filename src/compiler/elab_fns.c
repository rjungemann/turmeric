/* elab_fns.c -- function definition forms: defn, fn, extern-c, def. */
#include "elab_internal.h"

/* Phase 2: defn — (defn name [param1 param2 ...] : return-type body...)
 * For now, we only support : int return type annotation. Param types are
 * inferred from usage. */
Expr *elab_defn(Elab *e, const Form *call) {
    /* Phase R5: Check for #[no-unwind] attribute before name */
    uint32_t name_idx = 1;  /* index of name in items (after 'defn') */
    bool no_unwind = false;
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag == F_SYM && name_f->as.sym == e->sym_no_unwind_attr) {
        no_unwind = true;
        name_idx++;
        name_f = call->as.list.items[name_idx];
    }

    /* Phase M6: Check for (export-as "c_name") attribute before name.
     * Syntax: (defn (export-as "c_name") fname [...] :ret body...)
     * The attribute is a list whose head is the symbol export-as. */
    const char *c_export_name = NULL;
    if (name_f->tag == F_LIST && name_f->as.list.len == 2 &&
        name_f->as.list.items[0]->tag == F_SYM &&
        name_f->as.list.items[0]->as.sym == e->sym_export_as_attr) {
        Form *cname_arg = name_f->as.list.items[1];
        if (cname_arg->tag != F_STR) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "^:export-as argument must be a string literal: (export-as \"c_name\")");
            return NULL;
        }
        char *cname_buf = (char *)arena_alloc(e->arena, cname_arg->as.s.len + 1);
        memcpy(cname_buf, cname_arg->as.s.p, cname_arg->as.s.len);
        cname_buf[cname_arg->as.s.len] = '\0';
        c_export_name = cname_buf;
        name_idx++;
        name_f = call->as.list.items[name_idx];
    }

    /* Minimum: (defn name []) or (defn #[no-unwind] name []) */
    if (name_idx + 2 >= call->as.list.len) {  /* need name, params, body */
        diag_emit(DIAG_ERROR, call->span,
                  "defn requires (defn name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defn name must be a symbol");
        return NULL;
    }
    Binding *existing = scope_lookup(e->scope, name_f->as.sym);
    if (existing) {
        /* Allow forward-declared bindings from pass 1 to be redefined */
        /* Forward declarations have TY_FN type (from pass 1) */
        if (existing->type.kind == TY_FN && existing->is_global) {
            /* This is a forward declaration - proceed with the real definition */
        } else {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is already defined", name_f->as.sym->name);
            return NULL;
        }
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[name_idx + 1];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params - Phase 15 supports typeclass constraints
     * Syntax: [^Eq a x : a, y : a] means:
     *   - ^Eq is a constraint annotation (symbol starting with ^)
     *   - a is a type variable
     *   - x : a is parameter x with type annotation :a
     *   - y : a is parameter y with type annotation :a
     * 
     * We parse sequentially, collecting constraints and creating parameters.
     */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];
    /* Phase HRT1: full type annotations for rank-2 poly params (NULL if not poly) */
    Type *param_poly_types[MAX_FN_ARITY];
    for (uint8_t _i = 0; _i < MAX_FN_ARITY; _i++) param_poly_types[_i] = NULL;

    /* CT0: Contract type predicates from param annotations { v : T | p }.
     * Collected during param parsing; injected as pre-checks before the body. */
    const Form *ct_param_preds[MAX_FN_ARITY];     /* predicate forms */
    const char *ct_param_varnames[MAX_FN_ARITY];  /* contract var names (for substitution) */
    uint8_t ct_param_param_idx[MAX_FN_ARITY];     /* which param index the predicate belongs to */
    uint8_t n_ct_param_preds = 0;
    for (uint8_t _ci = 0; _ci < MAX_FN_ARITY; _ci++) {
        ct_param_preds[_ci] = NULL;
        ct_param_varnames[_ci] = NULL;
        ct_param_param_idx[_ci] = 0;
    }

    /* Phase 15: Constraint parsing */
    /* Track pending constraints that apply to the next type variable */
    TypeClass *pending_constraints[8];  /* Max 8 constraints per function */
    uint8_t n_pending = 0;

    /* Map from type variable name to its index for constraint association */
    /* For v1, we use a simple approach: each constraint applies to the next type var */
    /* const Symbol *current_type_var = NULL; */  /* Deferred to v2 */

    /* Constraint set for this function - allocated on arena */
    TypeConstraint *constraint_list = NULL;
    uint8_t n_constraints = 0;

    /* Phase G3: Equality constraint env built from (: a T) param items */
    SkolemEnv param_constraint_env;
    param_constraint_env.n = 0;

    /* Phase HKT: kind-variable names collected from ^f annotations.
     * These are type-level variables with kind * -> *; no runtime param is
     * created.  They are pushed into a temporary scope so that return-type
     * annotations such as (Equal (f a) (f b)) can resolve (f a) as TY_APP. */
    const Symbol *kind_var_names[8];
    uint8_t n_kind_vars = 0;

    /* LT0: ^linear annotation applies to the next parameter */
    bool next_param_linear = false;
    /* UT0: ^unique annotation applies to the next parameter */
    bool next_param_unique = false;
    /* UT2: ^mut annotation applies to the next parameter */
    bool next_param_mut = false;
    /* ST0: ^affine / ^relevant annotations apply to the next parameter */
    bool next_param_affine    = false;
    bool next_param_relevant  = false;

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];

        /* Phase G3: Handle equality constraint (: a T) in params.
         * Syntax: (: a int) means "type variable a equals int".
         * This is a type-level constraint; no runtime parameter is created.
         *
         * Two reader representations:
         *  Legacy:  F_LIST([sym(":"), sym("a"), sym("int")])  — 3-item list
         *  New:     F_LIST([F_TYPE_ANN(sym("a")), sym("int")])  — 2-item list
         *           (reader folds `: a` into a single F_TYPE_ANN node)
         */
        {
            Form *var_form = NULL;
            Form *type_form = NULL;
            if (p->tag == F_LIST && p->as.list.len == 3 &&
                p->as.list.items[0]->tag == F_SYM &&
                p->as.list.items[0]->as.sym == e->sym_colon) {
                var_form  = p->as.list.items[1];
                type_form = p->as.list.items[2];
            } else if (p->tag == F_LIST && p->as.list.len == 2 &&
                       p->as.list.items[0]->tag == F_TYPE_ANN &&
                       p->as.list.items[0]->as.list.len == 1) {
                var_form  = p->as.list.items[0]->as.list.items[0];
                type_form = p->as.list.items[1];
            } else if (p->tag == F_LIST && p->as.list.len == 2 &&
                       p->as.list.items[0]->tag == F_UNQUOTE &&
                       p->as.list.items[0]->as.list.len == 1 &&
                       p->as.list.items[0]->as.list.items[0]->tag == F_SYM) {
                /* Phase G3: (~ a T) equality constraint — reader turns (~ a T) into
                 * F_LIST[F_UNQUOTE(a), T] because ~ is the unquote reader macro */
                var_form  = p->as.list.items[0]->as.list.items[0]; /* symbol inside ~a */
                type_form = p->as.list.items[1];
            }
            if (var_form && type_form &&
                var_form->tag == F_SYM &&
                (type_form->tag == F_SYM || type_form->tag == F_KEYWORD)) {
                const char *tn = (type_form->tag == F_KEYWORD)
                    ? type_form->as.sym->name
                    : type_form->as.sym->name;
                TypeKind ck = typekind_from_symbol(tn);
                if (ck != TY_UNKNOWN && param_constraint_env.n < MAX_SKOLEM_BINDINGS) {
                    param_constraint_env.bindings[param_constraint_env.n].name =
                        var_form->as.sym->name;
                    param_constraint_env.bindings[param_constraint_env.n].kind = ck;
                    param_constraint_env.n++;
                }
                continue;
            }
        }

        /* LT0: ^linear annotation marks the next parameter as linear */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_linear) {
            next_param_linear = true;
            continue;
        }

        /* UT0: ^unique annotation marks the next parameter as unique */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_unique) {
            next_param_unique = true;
            continue;
        }

        /* UT2: ^mut annotation marks the next parameter as mutable (used with ^unique ^mut) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_mut) {
            next_param_mut = true;
            continue;
        }

        /* ST0: ^affine annotation marks the next parameter as affine (no duplication) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_affine) {
            next_param_affine = true;
            continue;
        }

        /* ST0: ^relevant annotation marks the next parameter as relevant (must be used) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_relevant) {
            next_param_relevant = true;
            continue;
        }

        /* MS2: ^multishot is not valid as a function parameter annotation */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_multishot) {
            diag_emit_with_code(DIAG_ERROR, p->span,
                TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,
                "'^multishot' annotation is only valid on a handler continuation, "
                "not on a function parameter");
            return NULL;
        }

        /* Phase 15: Handle constraint annotations (^Eq, ^Show, etc.) */
        if (p->tag == F_SYM && p->as.sym->len > 0 && p->as.sym->name[0] == '^') {
            /* This is a constraint annotation like ^Eq */
            const Symbol *constraint_name = p->as.sym;
            /* Look up the typeclass (skip the ^ character) */
            const char *tc_name_str = constraint_name->name + 1;  /* Skip '^' */
            uint32_t tc_name_len = constraint_name->len - 1;

            /* Phase HKT H4: Kind variable annotation — type-level only, erased at runtime.
             * A kind variable like `^f` declares that `f` ranges over type constructors
             * of kind '* -> *'.  It creates no runtime parameter; it is used as a
             * kind annotation on the function and may be followed by a ^Typeclass f
             * constraint (which adds a regular typeclass constraint handled below). */
            if (tc_name_len > 0 && tc_name_str[0] >= 'a' && tc_name_str[0] <= 'z') {
                /* Phase HKT: Kind variable (e.g. ^f).  Record the bare name so
                 * the return-type parser can resolve (f a) as TY_APP.  No
                 * runtime parameter is created. */
                if (n_kind_vars < 8) {
                    kind_var_names[n_kind_vars++] = symtab_intern(e->st,
                        strslice(tc_name_str, tc_name_len));
                }
                continue;
            }

            /* Create symbol for typeclass name */
            char tmp_name[64];
            snprintf(tmp_name, sizeof(tmp_name), "%.*s", tc_name_len, tc_name_str);
            const Symbol *tc_sym = symtab_intern(e->st, strslice(tmp_name, tc_name_len));
            TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, tc_sym);
            if (!tc) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: typeclass '%.*s' in constraint is not defined",
                          tc_name_len, tc_name_str);
                return NULL;
            }
            if (n_pending < 8) {
                pending_constraints[n_pending++] = tc;
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: too many constraints (max 8)");
                return NULL;
            }
            continue;
        }
        
        /* Phase 15: Handle type variable declarations */
        /* After constraints, the next symbol is a type variable name */
        if (n_pending > 0 && p->tag == F_SYM) {
            /* This is a type variable that the pending constraints apply to */
            /* For v1, we ignore the type variable name and just record constraints */
            
            /* Register all pending constraints for this type variable */
            /* Allocate space for new constraints */
            uint8_t new_count = n_constraints + n_pending;
            TypeConstraint *new_list = (TypeConstraint *)arena_alloc(e->arena,
                new_count * sizeof(TypeConstraint));
            if (constraint_list) {
                memcpy(new_list, constraint_list, n_constraints * sizeof(TypeConstraint));
            }
            constraint_list = new_list;
            
            for (uint8_t c = 0; c < n_pending; c++) {
                /* For v1, we just record the constraint - type_arg will be resolved later */
                /* We use TYPE_UNKNOWN as a placeholder for the type variable */
                constraint_list[n_constraints + c].typeclass = pending_constraints[c];
                constraint_list[n_constraints + c].type_arg = TYPE_UNKNOWN;
            }
            n_constraints = new_count;
            n_pending = 0;
            continue;
        }
        
        /* Phase HRT1: Handle complex type annotation (list form or F_TYPE_ANN) for previous param.
         * Syntax: [param-name (forall [a] (-> a a))] — list form follows a symbol.
         * Also: [param-name : (-> a b)] — F_TYPE_ANN wrapping any type form.
         * CT0: [param-name { v : T | pred }] — contract type annotation. */
        if (p->tag == F_LIST || p->tag == F_VEC || p->tag == F_TYPE_ANN || p->tag == F_CONTRACT_TYPE) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* CT0: For F_CONTRACT_TYPE, use the base type for the param kind.
             * The predicate is collected for injection as a precondition. */
            /* For F_CONTRACT_TYPE: collect predicate, use base type */
            if (p->tag == F_CONTRACT_TYPE && p->as.list.len >= 4) {
                /* items: [var, type-ann, "|", pred] */
                const char *ct_var = NULL;
                if (p->as.list.items[0]->tag == F_SYM) {
                    ct_var = p->as.list.items[0]->as.sym->name;
                }
                if (n_ct_param_preds < MAX_FN_ARITY) {
                    ct_param_preds[n_ct_param_preds]     = p->as.list.items[3];
                    ct_param_varnames[n_ct_param_preds]  = ct_var;
                    ct_param_param_idx[n_ct_param_preds] = n_params - 1;
                    n_ct_param_preds++;
                }
            }
            /* For F_TYPE_ANN, unwrap to the inner type form first */
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            /* Parse as a type expression — supports (forall [a] (-> a a)), (-> a b), etc. */
            Type *ann = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
            if (!ann) return NULL;
            /* CT0: For contract types, use base type for C-level representation */
            if (ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
                TypeKind base_kind = ann->as.contract_.base_type->kind;
                param_kinds[n_params - 1] = base_kind;
                params[n_params - 1]->type = *ann->as.contract_.base_type;
                continue;
            }
            if (ann->kind == TY_FORALL || ann->kind == TY_EXISTS) {
                /* Rank-2 polymorphic parameter: represented as tur_poly_fn_t at C level */
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = ann;
                param_poly_types[n_params - 1] = ann;
            } else if (ann->kind == TY_FN) {
                /* Plain function type annotation */
                param_kinds[n_params - 1] = TY_FN;
                params[n_params - 1]->type = *ann;
                /* LT2: For function-typed parameters, store full type in param_poly_types
                 * so it propagates into arg_full_types for linearity subtyping checks
                 * at call sites (-Xlinear). */
                if (g_linear_enabled) {
                    param_poly_types[n_params - 1] = ann;
                }
            } else {
                /* Other type annotation — use the kind */
                param_kinds[n_params - 1] = ann->kind;
                params[n_params - 1]->type = *ann;
                /* IT1: For union-typed parameters, store full type in param_poly_types
                 * so it propagates into arg_full_types for subtyping checks at call sites. */
                if (g_union_types_enabled && ann->kind == TY_UNION) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* IT2: For intersection-typed parameters, same propagation. */
                if (g_intersection_types_enabled && ann->kind == TY_INTERSECTION) {
                    param_poly_types[n_params - 1] = ann;
                }
            }
            /* LT3: Propagate linearity from the type annotation (e.g., [p : (lref int)]) */
            if (g_linear_enabled && params[n_params - 1]->type.copy_kind == CK_LINEAR) {
                params[n_params - 1]->is_linear = true;
            }
            /* ST2: Under -Xsubstructural, ref<T> params without an explicit discipline
             * annotation are inferred as SK_LINEAR. */
            if (g_substructural_enabled
                    && !params[n_params - 1]->is_linear
                    && !params[n_params - 1]->is_affine
                    && !params[n_params - 1]->is_relevant
                    && params[n_params - 1]->type.kind == TY_REF) {
                params[n_params - 1]->is_linear = true;
                params[n_params - 1]->type.substruct = SK_LINEAR;
            }
            /* UT0: Propagate uniqueness from the type annotation */
            if (g_unique_enabled && next_param_unique) {
                params[n_params - 1]->is_unique = true;
                params[n_params - 1]->type.copy_kind = CK_UNIQUE;
                next_param_unique = false;
            }
            /* UT2: Propagate mutability from the ^mut annotation */
            if (next_param_mut) {
                params[n_params - 1]->is_mut = true;
                next_param_mut = false;
            }
            continue;
        }

        if (p->tag != F_SYM && p->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: parameter must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }

        /* Handle type annotations: if this is a keyword like :int, it's a type for the previous param */
        if (p->tag == F_KEYWORD) {
            /* This is a type annotation for the previous parameter */
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* Update the type of the last parameter */
            const Symbol *kw = p->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                param_kinds[n_params - 1] = TY_INT;
                params[n_params - 1]->type = TYPE_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                param_kinds[n_params - 1] = TY_FLOAT;
                params[n_params - 1]->type = TYPE_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                param_kinds[n_params - 1] = TY_BOOL;
                params[n_params - 1]->type = TYPE_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                param_kinds[n_params - 1] = TY_CSTR;
                params[n_params - 1]->type = TYPE_CSTR;
            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) || 
                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                param_kinds[n_params - 1] = TY_NIL;
                params[n_params - 1]->type = TYPE_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                param_kinds[n_params - 1] = TY_NEVER;
                params[n_params - 1]->type = TYPE_NEVER;
            } else if (kw->len == 3 && memcmp(kw->name, "ref", 3) == 0) {
                /* Phase 5: :ref keyword — owning heap pointer (inner type unknown, use int) */
                param_kinds[n_params - 1] = TY_REF;
                params[n_params - 1]->type = type_ref(TY_INT);
                /* ST2: Under -Xsubstructural, :ref params without an explicit discipline
                 * annotation are inferred as SK_LINEAR. */
                if (g_substructural_enabled && !params[n_params - 1]->is_linear
                        && !params[n_params - 1]->is_affine
                        && !params[n_params - 1]->is_relevant) {
                    params[n_params - 1]->is_linear = true;
                    params[n_params - 1]->type.substruct = SK_LINEAR;
                }
            } else if (kw->len == 4 && memcmp(kw->name, "lref", 4) == 0) {
                /* LT3: :lref keyword — linear owning pointer (inner type unknown, use int) */
                param_kinds[n_params - 1] = TY_LREF;
                params[n_params - 1]->type = type_lref(TY_INT);
                /* Mark as linear: lref<T> is always exactly-once */
                if (g_linear_enabled) params[n_params - 1]->is_linear = true;
            } else if (kw->len == 3 && memcmp(kw->name, "set", 3) == 0) {
                /* Phase X3: set type annotation */
                Type set_type = { .kind = TY_SET, .copy_kind = CK_MOVE };
                param_kinds[n_params - 1] = TY_SET;
                params[n_params - 1]->type = set_type;
            } else {
                /* Phase G3: Try constraint env first (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                if (ck != TY_UNKNOWN) {
                    /* Resolved via equality constraint */
                    param_kinds[n_params - 1] = ck;
                    params[n_params - 1]->type = type_from_kind(ck);
                    params[n_params - 1]->type.copy_kind = typekind_default_copy_kind(ck);
                } else {
                    /* Try to look up as ADT name */
                    AdtDef *param_adt = NULL;
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            param_adt = e->adt_defs[ai];
                            break;
                        }
                    }
                    if (param_adt) {
                        param_kinds[n_params - 1] = TY_ADT;
                        params[n_params - 1]->type = type_adt(param_adt);
                    } else {
                        /* Phase HRT/G2: Unknown keyword -- treat as an implicit type variable.
                         * A parameter annotation like :a where 'a' is not a known type or ADT
                         * is an implicit type variable. Mark the binding TY_TYVAR so that inside
                         * a GADT match arm, the per-arm skolem env can resolve it to a concrete type. */
                        param_kinds[n_params - 1] = TY_TYVAR;
                        params[n_params - 1]->type = type_tyvar_named(kw->name);
                    }
                }
            }
            continue;
        }

        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: too many parameters (max %d)", MAX_FN_ARITY);
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* For phase 2, default to int */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        /* LT0: If the previous ^linear annotation applied to this parameter, mark it linear */
        if (next_param_linear) {
            b->is_linear = true;
            b->type.copy_kind = CK_LINEAR;
            next_param_linear = false;
        }
        /* UT0: If the previous ^unique annotation applied to this parameter, mark it unique */
        if (next_param_unique) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
            next_param_unique = false;
        }
        /* UT2: If the previous ^mut annotation applied to this parameter, mark it mutable */
        if (next_param_mut) {
            b->is_mut = true;
            next_param_mut = false;
        }
        /* ST0: If the previous ^affine annotation applied to this parameter, mark it affine */
        if (next_param_affine) {
            b->is_affine = true;
            b->type.substruct = SK_AFFINE;
            next_param_affine = false;
        }
        /* ST0: If the previous ^relevant annotation applied to this parameter, mark it relevant */
        if (next_param_relevant) {
            b->is_relevant = true;
            b->type.substruct = SK_RELEVANT;
            next_param_relevant = false;
        }
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }
    
    /* Phase 13: Lifetime annotations parsing deferred - restore original simple parsing */

    /* Parse return type annotation and body */
    /* body_start is the index of the first element after params (could be return type or body) */
    TypeKind return_kind = TY_NIL;
    AdtDef *return_adt_def = NULL; /* Phase G3: set when return type is an ADT name */
    StructDef *return_struct_def = NULL; /* LT4: set when return type is a struct name */
    Type *return_session_type = NULL; /* SS3a: full session return type (Session[P]) */
    uint32_t body_start = name_idx + 2;  /* name_idx + 1 = params, +1 = after params */

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type.
     * Uppercase names are concrete effects; lowercase are row variables.
     * The row is stored as ERK_UNRESOLVED and resolved after PASS_EFFECT_LOWER. */
    EffectRow *declared_effect_row_defn = NULL;
    if (call->as.list.len >= body_start + 1) {
        Form *maybe_row = call->as.list.items[body_start];
        if (maybe_row->tag == F_MAP) {
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
            declared_effect_row_defn = effect_row_unresolved(e->arena, syms, n_valid);
            body_start++;  /* skip past the effect row map */
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_defn, e->sym_effect_unsafe);

    /* Phase HKT: Create the inner scope early so that kind-variable bindings
     * (^f → TY_TYVAR/KIND_ARROW) are visible when the return-type annotation
     * is parsed below.  Regular parameter bindings are added to this same
     * scope after the annotation is parsed (see "Push params" below). */
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t kvi = 0; kvi < n_kind_vars; kvi++) {
        Type kv_type = type_tyvar_named(kind_var_names[kvi]->name);
        kv_type.hkt_kind = KIND_ARROW;
        Binding *kvb = binding_new(e, kind_var_names[kvi], kv_type,
                                   false, true, call->span);
        scope_add(&inner, kvb);
    }

    /* Check for : return-type annotation */
    if (call->as.list.len >= (body_start + 1)) {
        Form *ret_f = call->as.list.items[body_start];
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                return_kind = TY_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_kind = TY_CSTR;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 2 && memcmp(kw->name, "rc", 2) == 0) {
                return_kind = TY_RC;
            } else if (kw->len == 4 && memcmp(kw->name, "weak", 4) == 0) {
                return_kind = TY_WEAK;
            } else if (kw->len == 4 && memcmp(kw->name, "lref", 4) == 0) {
                /* LT3: :lref return type keyword — lref<T> (linear owning pointer) */
                return_kind = TY_LREF;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else if (kw->len == 3 && memcmp(kw->name, "set", 3) == 0) {
                /* Phase X3: set return type */
                return_kind = TY_SET;
            } else {
                /* Phase G3: Try constraint env (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                if (ck != TY_UNKNOWN) {
                    return_kind = ck;
                } else {
                    /* Try to look up as ADT name */
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            return_adt_def = e->adt_defs[ai];
                            return_kind = TY_ADT;
                            break;
                        }
                    }
                    /* LT4: Try to look up as struct name */
                    if (!return_adt_def) {
                        for (uint32_t si = 0; si < e->n_struct_defs; si++) {
                            if (strcmp(e->struct_defs[si]->name, kw->name) == 0) {
                                return_struct_def = e->struct_defs[si];
                                return_kind = TY_STRUCT;
                                break;
                            }
                        }
                    }
                    if (!return_adt_def && !return_struct_def) {
                        if (g_gadt_enabled) {
                            /* Phase HRT/G2: Unknown return type keyword -- named type variable.
                             * e.g., :a means the function returns the type variable a.
                             * For codegen, we fall through and let the body type determine the
                             * concrete return kind (see the TY_TYVAR inference below). */
                            return_kind = TY_TYVAR;
                        } else {
                            diag_emit(DIAG_ERROR, ret_f->span,
                                      "defn: unsupported return type keyword :%s",
                                      kw->name);
                            return NULL;
                        }
                    }
                }
            }
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0);
                if (ann) {
                    return_kind = ann->kind;
                    /* SS3a: Capture full session return type so callers see the complete
                     * protocol type (e.g. Session[Rec[self, ...]]) rather than a bare
                     * TY_SESSION shell with a NULL protocol pointer. */
                    if (g_sessions_enabled && ann->kind == TY_SESSION) {
                        return_session_type = ann;
                    }
                }
            }
            body_start++;
        }
    }

    /* CT0/CT1: Parse :pre and :post clauses between return annotation and body.
     * Scan items[body_start..] for F_KEYWORD(:pre) or F_KEYWORD(:post) pairs
     * and advance body_start past them. */
    const Form *ct_pre_form  = NULL;  /* :pre predicate form */
    const Form *ct_post_form = NULL;  /* :post predicate form */
    {
        while (body_start + 1 < call->as.list.len) {
            Form *maybe_kw = call->as.list.items[body_start];
            if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_pre) {
                if (body_start + 1 >= call->as.list.len) break;
                ct_pre_form = call->as.list.items[body_start + 1];
                body_start += 2;
            } else if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_post) {
                if (body_start + 1 >= call->as.list.len) break;
                ct_post_form = call->as.list.items[body_start + 1];
                body_start += 2;
            } else {
                break;
            }
        }
    }

    /* CT1: Param contract predicates will be injected as pre-checks below. */

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn: missing body");
        e->scope = inner.parent;
        scope_free(&inner);
        return NULL;
    }

    /* Phase HRT5: Early-update a forward-declared binding's arity and poly param
     * types before elaborating the body.  Without this, recursive calls inside
     * the body see the stale arity-1 / no-arg_full_types from pass-1, which
     * causes spurious arity-mismatch errors for functions with poly fn params. */
    if (existing && existing->type.kind == TY_FN && existing->is_global) {
        existing->type.as.fn.arity = n_params;
        for (uint8_t _ei = 0; _ei < n_params; _ei++) {
            existing->type.as.fn.arg_kinds[_ei] = param_kinds[_ei];
        }
        bool _any_poly = false;
        for (uint8_t _ei = 0; _ei < n_params; _ei++) {
            if (param_poly_types[_ei]) { _any_poly = true; break; }
        }
        if (_any_poly) {
            Type **_aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint8_t _ei = 0; _ei < n_params; _ei++) _aFT[_ei] = param_poly_types[_ei];
            existing->type.as.fn.arg_full_types = _aFT;
        }
    }

    /* Push params into the inner scope (created earlier for kind-var bindings). */
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    e->fn_body_depth++;
    /* Phase R6: Track current function name for linting */
    e->current_fn_name = name_f->as.sym;
    if (fn_declared_unsafe) e->unsafe_depth++;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) {
            if (fn_declared_unsafe) e->unsafe_depth--;
            e->fn_body_depth--;
            /* Phase R6: Reset current function name */
            e->current_fn_name = NULL;
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                /* Phase R6: Reset current function name */
                e->current_fn_name = NULL;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
        /* Phase R6: Warn on discarded result values in function bodies */
        if (g_warn_unused_result) {
            for (uint32_t i = 0; i < n_body - 1; i++) {
                if (items[i]->type.kind == TY_PTR_VOID) {
                    diag_emit(DIAG_WARNING, items[i]->span,
                              "discarded result value of type ptr<void>; use ignore! to suppress this warning");
                }
            }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    /* Phase R6: Reset current function name */
    e->current_fn_name = NULL;

    /* CT1: Inject contract checks into body.
     * Determine whether to emit checks based on build mode. */
    {
        bool should_check = g_contracts_enabled;
#ifdef NDEBUG
        if (!g_keep_contracts_in_release) should_check = false;
#endif
        if (should_check && body) {
            /* Look up tur-contract-check binding */
            Binding *check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);

            /* CT1: Param contract type predicates — inject as pre-checks.
             * For each { v : T | pred } param annotation, inject:
             *   (tur-contract-check (let [v param] pred) "Contract violated") */
            if (check_fn) {
                for (uint8_t ct_pi = 0; ct_pi < n_ct_param_preds; ct_pi++) {
                    if (ct_param_preds[ct_pi] == NULL) continue;
                    const char *var_nm = ct_param_varnames[ct_pi];
                    const Form *pred_f = ct_param_preds[ct_pi];
                    uint8_t pi = ct_param_param_idx[ct_pi];
                    /* Add contract var binding (alias for param) */
                    Binding *cv_b = NULL;
                    if (var_nm) {
                        StrSlice vnsl = strslice(var_nm, (uint32_t)strlen(var_nm));
                        const Symbol *cv_sym = symtab_intern(e->st, vnsl);
                        /* Only add if different from param name */
                        Binding *existing_cv = scope_lookup(e->scope, cv_sym);
                        if (!existing_cv || existing_cv != params[pi]) {
                            cv_b = binding_new(e, cv_sym, params[pi]->type, false, false, call->span);
                            /* Make cv_b reference same value as param by sharing the binding.
                             * We create a var expr below to read params[pi]. */
                            scope_add(e->scope, cv_b);
                        }
                    }
                    Expr *pred_e = elab_form(e, (Form *)pred_f);
                    if (pred_e) {
                        Expr **ck_args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                        /* If cv_b was added, wrap in let: (let [v param] pred) */
                        Expr *check_expr = pred_e;
                        if (cv_b) {
                            /* Build: let [cv_b = param_var] in pred_e */
                            Expr *param_var_e = expr_new(e->arena, EX_VAR, params[pi]->type, call->span);
                            param_var_e->as.var.binding = params[pi];
                            LetBinding *cv_lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
                            cv_lb->binding = cv_b;
                            cv_lb->init = param_var_e;
                            Expr *let_cv = expr_new(e->arena, EX_LET, pred_e->type, call->span);
                            let_cv->as.let_.bindings = cv_lb;
                            let_cv->as.let_.n = 1;
                            let_cv->as.let_.body = pred_e;
                            check_expr = let_cv;
                        }
                        ck_args[0] = check_expr;
                        Expr *ck_msg = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, call->span);
                        ck_msg->as.s.p = "Contract violated";
                        ck_msg->as.s.len = 17;
                        ck_args[1] = ck_msg;
                        Expr *ck_call = expr_new(e->arena, EX_CALL, TYPE_NIL, call->span);
                        ck_call->as.call_.fn_binding = check_fn;
                        ck_call->as.call_.args = ck_args;
                        ck_call->as.call_.n_args = 2;
                        ck_call->as.call_.fn_expr = NULL;
                        ck_call->as.call_.dict_arg = NULL;
                        ck_call->as.call_.is_poly_call = false;
                        ck_call->as.call_.poly_arg_mask = 0;
                        /* Prepend to body */
                        Expr **do2 = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                        do2[0] = ck_call;
                        do2[1] = body;
                        Expr *new_b = expr_new(e->arena, EX_DO, body->type, call->span);
                        new_b->as.do_.items = do2;
                        new_b->as.do_.n = 2;
                        body = new_b;
                    }
                }
            }

            /* CT1: :pre — prepend (tur-contract-check pre_pred "Precondition failed") */
            if (ct_pre_form && check_fn) {
                Expr *pred_e = elab_form(e, (Form *)ct_pre_form);
                if (pred_e) {
                    /* Build call: (tur-contract-check pred "Precondition failed") */
                    Expr **check_args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                    check_args[0] = pred_e;
                    /* String arg */
                    Expr *msg_e = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, call->span);
                    msg_e->as.s.p = "Precondition failed";
                    msg_e->as.s.len = 19;
                    check_args[1] = msg_e;
                    Expr *check_call = expr_new(e->arena, EX_CALL, TYPE_NIL, call->span);
                    check_call->as.call_.fn_binding = check_fn;
                    check_call->as.call_.args = check_args;
                    check_call->as.call_.n_args = 2;
                    check_call->as.call_.fn_expr = NULL;
                    check_call->as.call_.dict_arg = NULL;
                    check_call->as.call_.is_poly_call = false;
                    check_call->as.call_.poly_arg_mask = 0;
                    /* Prepend check to body as EX_DO */
                    Expr **do_items = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                    do_items[0] = check_call;
                    do_items[1] = body;
                    Expr *new_body = expr_new(e->arena, EX_DO, body->type, call->span);
                    new_body->as.do_.items = do_items;
                    new_body->as.do_.n = 2;
                    body = new_body;
                }
            }

            /* CT1: :post — wrap body as:
             *   (let [result body] (tur-contract-check post_pred "Postcondition failed") result) */
            if (ct_post_form && check_fn) {
                /* Create 'result' binding for the return value */
                Binding *result_b = binding_new(e, e->sym_result, body->type, false, false, call->span);
                /* Elaborate post predicate with 'result' in scope */
                scope_add(e->scope, result_b);
                Expr *post_pred_e = elab_form(e, (Form *)ct_post_form);
                /* Remove 'result' from scope (done via scope exit, but we patch manually) */
                /* Note: scope is already cleaned up below; we just need the expr */
                if (post_pred_e) {
                    /* Build (tur-contract-check post_pred "Postcondition failed") */
                    Expr **post_args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                    post_args[0] = post_pred_e;
                    Expr *post_msg = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, call->span);
                    post_msg->as.s.p = "Postcondition failed";
                    post_msg->as.s.len = 20;
                    post_args[1] = post_msg;
                    Expr *post_check = expr_new(e->arena, EX_CALL, TYPE_NIL, call->span);
                    post_check->as.call_.fn_binding = check_fn;
                    post_check->as.call_.args = post_args;
                    post_check->as.call_.n_args = 2;
                    post_check->as.call_.fn_expr = NULL;
                    post_check->as.call_.dict_arg = NULL;
                    post_check->as.call_.is_poly_call = false;
                    post_check->as.call_.poly_arg_mask = 0;
                    /* result_var: reference to the result binding */
                    Expr *result_var = expr_new(e->arena, EX_VAR, body->type, call->span);
                    result_var->as.var.binding = result_b;
                    /* do: (tur-contract-check ...) then result */
                    Expr **inner_items = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
                    inner_items[0] = post_check;
                    inner_items[1] = result_var;
                    Expr *inner_do = expr_new(e->arena, EX_DO, body->type, call->span);
                    inner_do->as.do_.items = inner_items;
                    inner_do->as.do_.n = 2;
                    /* let binding: result = body */
                    LetBinding *lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
                    lb->binding = result_b;
                    lb->init = body;
                    Expr *let_e = expr_new(e->arena, EX_LET, body->type, call->span);
                    let_e->as.let_.bindings = lb;
                    let_e->as.let_.n = 1;
                    let_e->as.let_.body = inner_do;
                    body = let_e;
                }
            }
        }
    }

    /* LT1: At function scope exit, verify all linear params were consumed */
    bool lt1_param_fail = false;
    if (g_linear_enabled && body) {
        for (uint8_t _li = 0; _li < n_params; _li++) {
            if (params[_li]->is_linear && !params[_li]->is_linear_consumed && !params[_li]->is_moved) {
                /* SS0b: Session channels get a distinct error code.
                 * SS1: include the current protocol state in the message. */
                if (g_sessions_enabled && params[_li]->type.kind == TY_SESSION) {
                    Type *proto = params[_li]->type.as.session_.fst;
                    diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                        TUR_E0211_SESSION_DROPPED,
                                        "session channel '%s' dropped before protocol completion "
                                        "(at %s, expected Close)",
                                        params[_li]->name->name,
                                        proto ? type_name(*proto) : "?");
                } else {
                    diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                        TUR_E0100_LINEAR_DROPPED,
                                        "linear parameter '%s' dropped without being consumed",
                                        params[_li]->name->name);
                }
                lt1_param_fail = true;
            }
        }
    }

    /* ST1: At function scope exit, verify all relevant params were used at least once */
    bool st1_param_fail = false;
    if (g_substructural_enabled && body) {
        for (uint8_t _li = 0; _li < n_params; _li++) {
            if (params[_li]->is_relevant && params[_li]->usage_state == USAGE_UNUSED
                    && !params[_li]->is_moved) {
                diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                    TUR_E0151_RELEVANT_DROPPED,
                                    "relevant parameter '%s' dropped without being used",
                                    params[_li]->name->name);
                st1_param_fail = true;
            }
        }
    }

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);
    if (lt1_param_fail || st1_param_fail) return NULL;

    /* Infer return type from body if not specified or polymorphic (TY_TYVAR).
     * For TY_TYVAR (named type variable like :a), use the body's concrete type
     * for codegen -- the polymorphic annotation is preserved in the declaration
     * but the C function signature uses the concrete type. */
    if ((return_kind == TY_NIL || return_kind == TY_TYVAR) && body->type.kind != TY_NIL
            && body->type.kind != TY_TYVAR) {
        return_kind = body->type.kind;
    }

    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* Phase G3: attach full ADT return type if declared (for proper def propagation) */
    if (return_adt_def) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        *rft = type_adt(return_adt_def);
        fn_type.as.fn.result_full_type = rft;
    }
    /* LT4: attach full struct return type if declared.
     * Stored so that emit.c can retrieve the StructDef (and hence the struct name)
     * without going through type_from_kind, which doesn't carry the def pointer. */
    if (return_struct_def) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        memset(rft, 0, sizeof(Type));   /* fully zero before writing fields */
        rft->kind = TY_STRUCT;
        rft->copy_kind = return_struct_def->is_linear ? CK_LINEAR
                       : (return_struct_def->is_copy ? CK_COPY : CK_MOVE);
        rft->hkt_kind = KIND_STAR;
        rft->as.struct_.def = return_struct_def;
        fn_type.as.fn.result_full_type = rft;
    }
    /* SS3a: attach full session return type if declared.
     * Without this, callers using type_from_kind(TY_SESSION) get a bare shell
     * with NULL protocol pointer, causing silent elaboration failures when the
     * returned channel is used in subsequent session operations. */
    if (return_session_type) {
        fn_type.as.fn.result_full_type = return_session_type;
    }

    /* Phase HRT1: attach full poly types for rank-2 params */
    {
        bool any_poly = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (param_poly_types[i]) { any_poly = true; break; }
        }
        if (any_poly) {
            Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint8_t i = 0; i < n_params; i++) aFT[i] = param_poly_types[i];
            fn_type.as.fn.arg_full_types = aFT;
        }
    }

    /* LT2: Store arg_linear flags from param bindings into fn_type */
    {
        bool any_linear = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_linear) { any_linear = true; break; }
        }
        if (any_linear) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_linear[i] = params[i]->is_linear;
            }
        }
    }

    /* UT0: Store arg_unique flags from param bindings into fn_type */
    {
        bool any_unique = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique) { any_unique = true; break; }
        }
        if (any_unique) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_unique[i] = params[i]->is_unique;
            }
        }
    }
    /* UT2: Store arg_unique_mut flags (^unique ^mut) from param bindings into fn_type */
    {
        bool any_unique_mut = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique && params[i]->is_mut) { any_unique_mut = true; break; }
        }
        if (any_unique_mut) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_unique_mut[i] = params[i]->is_unique && params[i]->is_mut;
            }
        }
    }
    /* ST0: Store arg_affine flags from param bindings into fn_type */
    {
        bool any_affine = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_affine) { any_affine = true; break; }
        }
        if (any_affine) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_affine[i] = params[i]->is_affine;
            }
        }
    }
    /* ST0: Store arg_relevant flags from param bindings into fn_type */
    {
        bool any_relevant = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_relevant) { any_relevant = true; break; }
        }
        if (any_relevant) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_relevant[i] = params[i]->is_relevant;
            }
        }
    }

    /* Create/update binding for the function.
     * Reuse pass-1 forward bindings in place so subsequent lookups observe
     * updated arity/types from the real definition. */
    Binding *b = NULL;
    if (existing && existing->type.kind == TY_FN && existing->is_global) {
        b = existing;
        b->type = fn_type;
        b->span = name_f->span;
        /* Phase M7: Forward declarations from pass 1 don't know module context.
         * Override defining_module_name with the actual module the defn is in. */
        b->defining_module_name = e->current_module_name;
    } else {
        b = binding_new(e, name_f->as.sym, fn_type, false, true, name_f->span);
        scope_add(&e->global, b);
    }
    /* Phase R5: Store #[no-unwind] attribute on the binding */
    b->no_unwind = no_unwind;
    /* Phase M6: Store ^:export-as C name on the binding */
    b->c_export_name = c_export_name;

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_defn) {
        b->type.as.fn.effect_row = declared_effect_row_defn;
    }
    /* Store param types for codegen.
     * For TY_FN params (annotated with :(fn [...] #{...} :type)), use the full
     * binding type which preserves the result kind and effect row.
     * For TY_STRUCT params, use the binding type which preserves the StructDef
     * pointer (needed so emit.c can emit the struct name in the C signature).
     * For all other kinds, fall back to type_from_kind which is sufficient. */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        if (param_kinds[i] == TY_FN && params[i]->type.kind == TY_FN) {
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_STRUCT && params[i]->type.kind == TY_STRUCT) {
            /* LT4: preserve StructDef so emit.c emits the struct name, not int64_t. */
            fd->param_types[i] = params[i]->type;
        } else {
            fd->param_types[i] = type_from_kind(param_kinds[i]);
        }
    }
    /* Phase 15: Store collected constraints */
    fd->constraints.constraints = constraint_list;
    fd->constraints.n_constraints = n_constraints;
    fd->constraints.cap_constraints = n_constraints;

    Expr *out = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    out->as.fn_def_.fn = fd;
    return out;
}

/* Phase 2: fn — (fn [param1 param2 ...] body...) — no capture for phase 2
 * Lifts to a static function. For now, we require a return type annotation.
 * Example: (fn [x y] :int (+ x y)) */
Expr *elab_fn(Elab *e, const Form *call) {
    /* Minimum: (fn [params...] body...) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn requires (fn [params...] body...)");
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[1];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "fn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: parameter name must be a symbol");
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: too many parameters (max %d)", MAX_FN_ARITY);
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* For phase 2, all params are int by default */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    uint32_t body_start = 2;

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type. */
    EffectRow *declared_effect_row_fn = NULL;
    if (call->as.list.len >= 3) {
        Form *maybe_row = call->as.list.items[2];
        if (maybe_row->tag == F_MAP) {
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
            declared_effect_row_fn = effect_row_unresolved(e->arena, syms, n_valid);
            body_start = 3;
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_fn, e->sym_effect_unsafe);

    /* Check for : return-type annotation */
    if (call->as.list.len >= (body_start + 1)) {
        Form *ret_f = call->as.list.items[body_start];
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                return_kind = TY_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 2 && memcmp(kw->name, "rc", 2) == 0) {
                return_kind = TY_RC;
            } else if (kw->len == 4 && memcmp(kw->name, "weak", 4) == 0) {
                return_kind = TY_WEAK;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "fn: unsupported return type keyword :%s",
                          kw->name);
                /* params is arena-allocated, no need to free */
                return NULL;
            }
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0);
                if (ann) return_kind = ann->kind;
            }
            body_start++;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn: missing body");
        /* params is arena-allocated, no need to free */
        return NULL;
    }

    /* Push a new scope for the function body with params bound */
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;
    e->fn_body_depth++;
    if (fn_declared_unsafe) e->unsafe_depth++;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) {
            if (fn_declared_unsafe) e->unsafe_depth--;
            e->fn_body_depth--;
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;

    /* Phase 3: Capture analysis - collect free variables in the body */
    /* We need to do this before popping the scope */
    uint32_t n_captures = 0;
    Binding **captures = collect_free_vars(body, params, n_params, &n_captures);

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Infer return type from body if not specified */
    if (return_kind == TY_NIL && body->type.kind != TY_NIL) {
        return_kind = body->type.kind;
    }
    
    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = TY_INT;  /* All int for phase 2 */
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* Check if we're at top level */
    bool at_top_level = (e->scope == &e->global);

    /* For anonymous fn, we lift it to a static function with a generated name.
     * We use the arena to allocate a unique name. */
    char fn_name_buf[32];
    snprintf(fn_name_buf, sizeof(fn_name_buf), "__fn_%u", e->next_id++);
    const Symbol *fn_name_sym = symtab_intern(e->st, 
        strslice(fn_name_buf, (uint32_t)strlen(fn_name_buf)));
    
    Binding *b = binding_new(e, fn_name_sym, fn_type, false, true, call->span);
    scope_add(&e->global, b);

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_fn) {
        b->type.as.fn.effect_row = declared_effect_row_fn;
    }
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        fd->param_types[i] = type_from_kind(param_kinds[i]);
    }
    /* Phase 15: Initialize constraints */
    constraint_set_init(&fd->constraints);

    /* Create the FN_DEF expression that will be emitted at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    fn_def_expr->as.fn_def_.fn = fd;

    if (n_captures == 0) {
        /* Register FN_DEF for file-scope emission regardless of scope level.
         * Without this, an anonymous fn used as an argument at top level would
         * return EX_VAR("__fn_N") but never emit the EX_FN_DEF that creates
         * the runtime closure. */
        elab_register_file_def(e, fn_def_expr);
        /* Return VAR reference to the function */
        Expr *var_expr = expr_new(e->arena, EX_VAR, fn_type, call->span);
        var_expr->as.var.binding = b;
        free(captures);
        return var_expr;
    } else {
        /* Phase 3: Closure with captures */
        /* Generate env struct name */
        char env_name_buf[32];
        snprintf(env_name_buf, sizeof(env_name_buf), "__env_%u", e->next_id++);
        const Symbol *env_name_sym = symtab_intern(e->st,
            strslice(env_name_buf, (uint32_t)strlen(env_name_buf)));
        
        /* Modify the FnDef to include env parameter as first parameter */
        uint8_t new_n_params = n_params + 1;
        if (new_n_params > MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, call->span,
                      "fn with captures: too many parameters including env (max %d)", MAX_FN_ARITY);
            free(captures);
            return NULL;
        }
        
        /* Create new params array with env as first parameter */
        Binding **new_params = (Binding **)arena_alloc(e->arena, new_n_params * sizeof(Binding *));
        Type *new_param_types = (Type *)arena_alloc(e->arena, new_n_params * sizeof(Type));
        
        /* First param is env (void*) */
        char env_param_name[32];
        snprintf(env_param_name, sizeof(env_param_name), "__env_p_%u", e->next_id++);
        const Symbol *env_param_sym = symtab_intern(e->st,
            strslice(env_param_name, (uint32_t)strlen(env_param_name)));
        Binding *env_param_binding = binding_new(e, env_param_sym, TYPE_PTR_VOID, false, false, call->span);
        new_params[0] = env_param_binding;
        new_param_types[0] = TYPE_PTR_VOID;
        
        /* Copy existing params */
        for (uint8_t i = 0; i < n_params; i++) {
            new_params[i + 1] = params[i];
            new_param_types[i + 1] = TYPE_INT;
        }
        
        /* Update FnDef with new params */
        fd->params = new_params;
        fd->n_params = new_n_params;
        fd->param_types = new_param_types;
        
        /* Update function type to include env parameter */
        TypeKind new_arg_kinds[MAX_FN_ARITY];
        new_arg_kinds[0] = TY_PTR_VOID;  /* env parameter */
        for (uint8_t i = 0; i < n_params; i++) {
            new_arg_kinds[i + 1] = TY_INT;
        }
        Type new_fn_type = type_fn(new_arg_kinds, new_n_params, return_kind);
        b->type = new_fn_type;
        fd->binding->type = new_fn_type;
        fn_def_expr->type = new_fn_type;
        
        /* Register the modified FN_DEF for file-scope emission */
        if (!at_top_level) {
            elab_register_file_def(e, fn_def_expr);
        }
        
        /* Create Closure struct */
        struct Closure *closure = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
        closure->fn = fd;
        closure->captures = captures;
        closure->n_captures = n_captures;
        closure->env_name = env_name_sym;
        
        /* Store closure reference in FnDef for codegen */
        fd->closure = closure;
        
        /* Create EX_CLOSURE expression */
        /* The closure's type is void* (pointer to closure struct) */
        Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, TYPE_PTR_VOID, call->span);
        closure_expr->as.closure_.closure = closure;
        
        /* Don't free captures - it's now owned by the closure */
        return closure_expr;
    }
}

/* Phase 2: extern-c — (extern-c name [param1 param2 ...] : return-type)
 * Declares an external C function. For phase 2, we don't support capture.
 * Example: (extern-c printf [^cstr fmt] : int)
 * The ^ prefix on a param indicates it's a C type annotation (not yet implemented).
 * For now, all params are treated as int64_t or pointers.
 * 
 * Supported annotations:
 *   ^cstr - const char* (string)
 *   ^ptr  - void* (pointer)
 */
Expr *elab_extern_c(Elab *e, const Form *call) {
    /* Minimum: (extern-c name [params...] : ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "extern-c name must be a symbol");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "extern-c: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "extern-c: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params - support type annotations: [name :type ...]
     * e.g. (extern-c getenv [key :cstr] :cstr) */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        /* Handle type annotation keyword or F_TYPE_ANN after the previous param */
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "extern-c: type annotation without preceding parameter");
                return NULL;
            }
            TypeKind pk;
            if (p->tag == F_TYPE_ANN) {
                Type *ann = (p->as.list.len > 0)
                    ? type_expr_from_form(e, p->as.list.items[0], NULL, NULL, NULL, 0)
                    : NULL;
                if (!ann) return NULL;
                pk = ann->kind;
            } else {
                const Symbol *kw = p->as.sym;
                pk = typekind_from_symbol(kw->name);
                if (pk == TY_UNKNOWN) {
                    /* Legacy fallback names */
                    if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) pk = TY_PTR_VOID;
                    else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) pk = TY_NIL;
                    else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) pk = TY_PTR_VOID;
                    else {
                        diag_emit(DIAG_ERROR, p->span,
                                  "extern-c: unsupported parameter type :%s", kw->name);
                        return NULL;
                    }
                }
            }
            param_kinds[n_params - 1] = pk;
            params[n_params - 1]->type = type_from_kind(pk);
            continue;
        }
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: parameter must be a symbol");
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: too many parameters (max %d)", MAX_FN_ARITY);
            return NULL;
        }

        /* Handle ^type prefix: ^int, ^ptr<void>, etc.
         * When a symbol starts with '^', the rest is a C type annotation and the
         * NEXT symbol in the vector is the parameter name. */
        if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
            const char *tname = p->as.sym->name + 1;
            size_t tlen = p->as.sym->len - 1;
            TypeKind ck;
            if (tlen == 3 && memcmp(tname, "int", 3) == 0) ck = TY_INT;
            else if (tlen == 4 && memcmp(tname, "bool", 4) == 0) ck = TY_BOOL;
            else if (tlen == 4 && memcmp(tname, "cstr", 4) == 0) ck = TY_CSTR;
            else if (tlen == 3 && memcmp(tname, "ptr", 3) == 0) ck = TY_PTR_VOID;
            else if (tlen == 9 && memcmp(tname, "ptr<void>", 9) == 0) ck = TY_PTR_VOID;
            else if (tlen == 4 && memcmp(tname, "void", 4) == 0) ck = TY_NIL;
            else ck = TY_INT; /* unknown ^type: default to int */
            /* Peek at next element to get the param name */
            i++;
            if (i >= params_f->as.list.len) {
                /* ^type at end with no following name — create anonymous param */
                if (n_params == 0) {
                    params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
                }
                param_kinds[n_params] = ck;
                /* Use the ^type symbol itself as a placeholder name */
                Binding *b = binding_new(e, p->as.sym, type_from_kind(ck), false, false, p->span);
                params[n_params++] = b;
                break;
            }
            Form *name_f = params_f->as.list.items[i];
            if (name_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "extern-c: expected parameter name after ^type");
                return NULL;
            }
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
            }
            param_kinds[n_params] = ck;
            Binding *b = binding_new(e, name_f->as.sym, type_from_kind(ck), false, false, name_f->span);
            params[n_params++] = b;
            continue;
        }

        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation; optionally skip #{Effect...} advisory row */
    uint32_t ret_idx = 3;
    if (ret_idx < call->as.list.len && call->as.list.items[ret_idx]->tag == F_MAP) {
        /* #{...} effect-row annotation: skip silently (advisory in v1) */
        ret_idx++;
    }
    if (ret_idx >= call->as.list.len) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }
    Form *ret_f = call->as.list.items[ret_idx];
    if (ret_f->tag != F_KEYWORD && ret_f->tag != F_TYPE_ANN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: return type must be a keyword (:int, :bool, :void, :cstr, :ptr)");
        return NULL;
    }

    TypeKind return_kind;
    if (ret_f->tag == F_TYPE_ANN) {
        Type *ann = (ret_f->as.list.len > 0)
            ? type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0)
            : NULL;
        if (!ann) return NULL;
        return_kind = ann->kind;
    } else {
        return_kind = typekind_from_symbol(ret_f->as.sym->name);
        if (return_kind == TY_UNKNOWN) {
            /* Legacy fallback names */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) return_kind = TY_NIL;
            else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) return_kind = TY_PTR_VOID;
            else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) return_kind = TY_PTR_VOID;
            else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "extern-c: unsupported return type :%s", kw->name);
                return NULL;
            }
        }
    }

    /* Create function type */
    Type fn_type = type_fn(param_kinds, n_params, return_kind);

    /* Create a binding for the extern-c function so it can be looked up and called */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, call->span);
    b->is_extern_c = true;  /* ER6: mark as extern-c for effect inference */
    scope_add(&e->global, b);

    /* CT4: Parse optional :pre and :post clauses after the return type annotation.
     * Syntax: (extern-c name [params...] :ret-type :pre pred :post pred) */
    const Form *ec_pre_form  = NULL;
    const Form *ec_post_form = NULL;
    for (uint32_t ci = ret_idx + 1; ci < call->as.list.len; ci++) {
        const Form *maybe_kw = call->as.list.items[ci];
        if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_pre) {
            if (ci + 1 < call->as.list.len) {
                ec_pre_form = call->as.list.items[++ci];
            }
        } else if (maybe_kw->tag == F_KEYWORD && maybe_kw->as.sym == e->kw_post) {
            if (ci + 1 < call->as.list.len) {
                ec_post_form = call->as.list.items[++ci];
            }
        }
    }

    /* Create ExternC declaration */
    ExternC *ec = (ExternC *)arena_alloc(e->arena, sizeof(ExternC));
    ec->c_name = name_f->as.sym;
    ec->binding = b;
    ec->return_type = type_from_kind(return_kind);
    ec->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        ec->param_types[i] = type_from_kind(param_kinds[i]);
    }
    ec->n_params = n_params;
    ec->is_variadic = false;
    /* CT4: store pre/post predicates for contract check emission */
    ec->pre_cond  = ec_pre_form;
    ec->post_cond = ec_post_form;

    Expr *out = expr_new(e->arena, EX_EXTERN_C, fn_type, call->span);
    out->as.extern_c_.ext = ec;

    /* params was allocated with arena_alloc, so no need to free */
    return out;
}

Expr *elab_def(Elab *e, const Form *call) {
    /* Phase P3: Check for ^persistent annotation before name */
    /* Syntax: (def ^persistent name init) */
    uint32_t name_idx = 1;
    bool is_persistent = false;
    
    if (call->as.list.len > 3) {
        Form *first = call->as.list.items[name_idx];
        if (first->tag == F_SYM && first->as.sym == e->sym_caret_persistent) {
            is_persistent = true;
            name_idx++;
        }
    }
    
    if (name_idx + 2 != call->as.list.len) {
        diag_emit(DIAG_ERROR, call->span, "def takes (def [^persistent] name init)");
        return NULL;
    }
    
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "def name must be a symbol");
        return NULL;
    }
    /* Top-level only — error if not in global scope. */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span, "def is only valid at the top level");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "def: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }
    Expr *init = elab_form(e, call->as.list.items[name_idx + 1]);
    if (!init) return NULL;

    /* Phase 5: ref<T> is scope-local only — disallow at top-level def */
    if (init->type.kind == TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "def: ref<T> values must be scope-local; use let instead of def for '%s'",
                  name_f->as.sym->name);
        return NULL;
    }

    Binding *b = binding_new(e, name_f->as.sym, init->type,
                             /*is_mut=*/false, /*is_global=*/true, name_f->span);
    b->is_persistent = is_persistent;
    scope_add(&e->global, b);

    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = init;
    out->as.def_.struct_def = NULL;
    return out;
}
