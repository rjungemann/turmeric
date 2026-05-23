/* elab_forms.c -- control-flow and basic expression forms (let/if/do/while/case/...). */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form);

/* ---- special forms ---- */

Expr *elab_let(Elab *e, const Form *call) {
    /* (let [b1 i1 b2 i2 ...] body...) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let requires a binding vector");
        return NULL;
    }
    Form *bindings_form = call->as.list.items[1];
    if (bindings_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bindings_form->span,
                  "let bindings must be a vector [name init ...]");
        return NULL;
    }

    /* Parse bindings: walk left-to-right, optional ^mut / ^persistent prefix per entry. */
    LetBinding *binds = NULL;
    bool       *binding_moved_during_init = NULL; /* tracks moves of preceding bindings during each init elaboration */
    uint32_t    n_binds = 0, cap = 0;
    Scope       inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;

    int rc = 0;
    uint32_t i = 0;
    while (i < bindings_form->as.list.len) {
        Form *cur = bindings_form->as.list.items[i];
        bool is_mut = false;
        bool is_persistent = false;
        bool is_linear_ann = false;
        bool is_unique_ann = false;
        bool is_affine_ann = false;   /* ST0 */
        bool is_relevant_ann = false; /* ST0 */
        /* Phase P3 / LT0 / UT0 / UT2 / ST0: Consume binding annotations in any order.
         * Accepted: ^mut, ^persistent, ^linear, ^unique, ^affine, ^relevant -- may
         * appear in any combination before the binding name, e.g. [^affine v ...]. */
        {
            bool keep_going = true;
            while (keep_going && cur->tag == F_SYM) {
                if (cur->as.sym == e->sym_caret_mut) {
                    is_mut = true;
                } else if (cur->as.sym == e->sym_caret_persistent) {
                    is_persistent = true;
                } else if (cur->as.sym == e->sym_caret_linear) {
                    is_linear_ann = true;
                } else if (cur->as.sym == e->sym_caret_unique) {
                    is_unique_ann = true;
                } else if (cur->as.sym == e->sym_caret_affine) {
                    is_affine_ann = true;
                } else if (cur->as.sym == e->sym_caret_relevant) {
                    is_relevant_ann = true;
                } else if (cur->as.sym == e->sym_caret_multishot) {
                    /* MS2: ^multishot is only valid on handler continuation bindings */
                    diag_emit_with_code(DIAG_ERROR, cur->span,
                        TUR_E0501_MULTISHOT_ANN_OUTSIDE_HANDLER,
                        "'^multishot' annotation is only valid on a handler continuation, "
                        "not on a let binding");
                    rc = -1; keep_going = false; break;
                } else {
                    break; /* Not an annotation -- this is the binding name */
                }
                i++;
                if (i >= bindings_form->as.list.len) {
                    diag_emit(DIAG_ERROR, cur->span,
                              "trailing annotation '%s' with no binding name",
                              cur->as.sym->name);
                    rc = -1; keep_going = false; break;
                }
                cur = bindings_form->as.list.items[i];
            }
        }
        /* SS0b: Vector destructuring: (let [[a b] init] ...) */
        if (cur->tag == F_VEC) {
            Form *vec_pat = cur;
            Span  vec_span = cur->span;
            i++;
            if (i >= bindings_form->as.list.len) {
                diag_emit(DIAG_ERROR, vec_span,
                          "let vector destructuring is missing its initializer");
                rc = -1; break;
            }
            Form *init_form_v = bindings_form->as.list.items[i++];
            Expr *init_v = elab_form(e, init_form_v);
            if (!init_v) { rc = -1; break; }
            /* Create one binding per name in the pattern vector with placeholder types.
             * The actual types will be verified by codegen (SS2); for SS0b the
             * type-level operations (send/recv/offer/choose-*) already emit their
             * own diagnostics before returning. */
            uint32_t n_elems = vec_pat->as.list.len;
            /* SS2: For session pair types we track the first binding so vi=1 can
             * reference vi=0's C name via __TUR_CAP_0__. */
            uint32_t first_bind_idx = n_binds;
            for (uint32_t vi = 0; vi < n_elems; vi++) {
                Form *vname_f = vec_pat->as.list.items[vi];
                if (vname_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, vname_f->span,
                              "vector destructuring elements must be symbols");
                    rc = -1; break;
                }
                /* Determine the type for this element. */
                Type elem_type;
                if (init_v->type.kind == TY_SESSION_PAIR ||
                    init_v->type.kind == TY_SESSION_RECV_PAIR) {
                    Type *fst_t = init_v->type.as.session_.fst;
                    /* SS8: N-role make-protocol pairs: get elem_type from global_t directly */
                    if (init_v->type.kind == TY_SESSION_PAIR && fst_t && fst_t->kind == TY_ROLE) {
                        Type *global_t = fst_t->as.role_.global_type;
                        if ((int)vi < global_t->as.global_.n_roles) {
                            elem_type = type_role(global_t, global_t->as.global_.roles[vi],
                                                  global_t->as.global_.body);
                            elem_type.copy_kind = CK_LINEAR;
                        } else {
                            elem_type = type_from_kind(TY_INT);
                        }
                    } else if (vi == 0 && fst_t) {
                        elem_type = *fst_t;
                    } else if (vi == 1 && init_v->type.as.session_.snd) {
                        elem_type = *init_v->type.as.session_.snd;
                    } else {
                        elem_type = type_from_kind(TY_INT);
                    }
                } else {
                    elem_type = (vi == 0) ? init_v->type : type_from_kind(TY_INT);
                }
                Binding *vb = binding_new(e, vname_f->as.sym, elem_type, false, false, vname_f->span);
                if (elem_type.copy_kind == CK_LINEAR) {
                    vb->is_linear = true;
                }
                scope_add(&inner, vb);
                if (n_binds == cap) {
                    cap = cap ? cap * 2 : 4;
                    binds = (LetBinding *)realloc(binds, cap * sizeof(LetBinding));
                    if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
                    binding_moved_during_init = (bool *)realloc(binding_moved_during_init, cap * sizeof(bool));
                    if (!binding_moved_during_init) { fprintf(stderr, "tur: oom\n"); abort(); }
                }
                /* SS2: For session pair types, create separate EX_INLINE_C inits
                 * so each binding gets its own expression (avoids double evaluation). */
                Expr *elem_init = init_v; /* default: share the original init */
                if (init_v->type.kind == TY_SESSION_PAIR) {
                    Type *fst = init_v->type.as.session_.fst;
                    bool is_role_pair = (fst && fst->kind == TY_ROLE);
                    Expr *ic_expr = expr_new(e->arena, EX_INLINE_C, elem_type, vec_span);
                    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
                    ic->return_type = elem_type;
                    ic->val_exprs = NULL; ic->n_val_exprs = 0;
                    ic->captures = NULL; ic->n_captures = 0;
                    if (is_role_pair) {
                        /* SS7: make-protocol destructuring:
                         *   vi=0: tur_make_roles(N, 0) -- allocates router + role 0
                         *   vi=k: tur_get_role(__TUR_VAL_0__, k) -- peer role on same router */
                        int n_roles = fst->as.role_.global_type->as.global_.n_roles;
                        if (vi == 0) {
                            Buf code_buf; buf_init(&code_buf);
                            buf_printf(&code_buf, "tur_make_roles(%d, 0)", n_roles);
                            char *code_str = (char *)arena_alloc(e->arena, code_buf.len + 1);
                            memcpy(code_str, code_buf.data, code_buf.len);
                            code_str[code_buf.len] = '\0';
                            ic->code = strslice(code_str, (uint32_t)code_buf.len);
                            buf_free(&code_buf);
                        } else {
                            /* vi > 0: tur_get_role(__TUR_VAL_0__, vi) referencing vi=0's binding */
                            Buf code_buf; buf_init(&code_buf);
                            buf_printf(&code_buf, "tur_get_role(__TUR_VAL_0__, %d)", (int)vi);
                            char *code_str = (char *)arena_alloc(e->arena, code_buf.len + 1);
                            memcpy(code_str, code_buf.data, code_buf.len);
                            code_str[code_buf.len] = '\0';
                            ic->code = strslice(code_str, (uint32_t)code_buf.len);
                            buf_free(&code_buf);
                            /* Reference vi=0's binding as __TUR_VAL_0__ */
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            Expr *r0_var = expr_new(e->arena, EX_VAR,
                                                    binds[first_bind_idx].binding->type, vec_span);
                            r0_var->as.var.binding = binds[first_bind_idx].binding;
                            ic->val_exprs[0] = r0_var;
                            ic->n_val_exprs = 1;
                        }
                    } else {
                        /* Existing make-session logic */
                        if (vi == 0) {
                            /* SS2: Pass protocol name for debug tag: tur_session_new(TUR_DBGPROTO("...")) */
                            Buf code_buf;
                            buf_init(&code_buf);
                            buf_puts(&code_buf, "tur_session_new(TUR_DBGPROTO(\"");
                            /* init_v->type is TY_SESSION_PAIR; fst is Session[P]; fst->fst is P */
                            Type *sess_p = init_v->type.as.session_.fst;
                            if (sess_p && sess_p->as.session_.fst) {
                                type_print(&code_buf, *sess_p->as.session_.fst);
                            } else {
                                buf_puts(&code_buf, "?");
                            }
                            buf_puts(&code_buf, "\"))");
                            char *code_str = (char *)arena_alloc(e->arena, code_buf.len + 1);
                            memcpy(code_str, code_buf.data, code_buf.len);
                            code_str[code_buf.len] = '\0';
                            ic->code = strslice(code_str, (uint32_t)code_buf.len);
                            buf_free(&code_buf);
                        } else {
                            static const char ref_code[] = "__TUR_VAL_0__";
                            ic->code = strslice(ref_code, sizeof(ref_code) - 1);
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            Expr *chan_var = expr_new(e->arena, EX_VAR,
                                                      binds[first_bind_idx].binding->type, vec_span);
                            chan_var->as.var.binding = binds[first_bind_idx].binding;
                            ic->val_exprs[0] = chan_var;
                            ic->n_val_exprs = 1;
                        }
                    }
                    ic_expr->as.inline_c_.inline_c = ic;
                    elem_init = ic_expr;
                } else if (init_v->type.kind == TY_SESSION_RECV_PAIR) {
                    /* recv / recv-timeout / recv-from destructuring.
                     * Case A -- plain recv (EX_INLINE_C, code is recv-pair sentinel):
                     *   vi=0: tur_session_recv(__TUR_VAL_0__)
                     *   vi=1: __TUR_VAL_0__ (channel ptr)
                     * Case B -- recv-timeout Left arm (init_v is EX_VAR of type RecvPair):
                     *   vi=0: tur__rtv_ (value stashed by tur_session_recv_timeout)
                     *   vi=1: __TUR_VAL_0__ (the arm var = channel ptr)
                     * Case C -- recv-timeout inline_c
                     *   (code starts with "tur_session_recv_timeout"):
                     *   vi=0: tur__rtv_
                     *   vi=1: __TUR_VAL_0__ (channel)
                     * Case D -- SS7 recv-from (EX_INLINE_C, snd type is TY_ROLE):
                     *   vi=0: tur_router_recv(__TUR_VAL_0__, from_idx)
                     *   vi=1: __TUR_VAL_0__ (role channel ptr) */
                    Type *recv_snd = init_v->type.as.session_.snd;
                    bool is_role_recv = (recv_snd && recv_snd->kind == TY_ROLE
                                         && init_v->kind == EX_INLINE_C);
                    bool is_recv_timeout_var = (!is_role_recv && init_v->kind == EX_VAR);
                    bool is_recv_timeout_ic = false;
                    Binding *chan_b = NULL;
                    if (!is_role_recv && !is_recv_timeout_var && init_v->kind == EX_INLINE_C) {
                        InlineC *orig_ic = init_v->as.inline_c_.inline_c;
                        is_recv_timeout_ic = (orig_ic->code.len > 24 &&
                            memcmp(orig_ic->code.p, "tur_session_recv_timeout", 24) == 0);
                        chan_b = (orig_ic->n_val_exprs > 0 && orig_ic->val_exprs[0]
                            && orig_ic->val_exprs[0]->kind == EX_VAR)
                            ? orig_ic->val_exprs[0]->as.var.binding : NULL;
                    }
                    bool is_recv_timeout = is_recv_timeout_var || is_recv_timeout_ic;
                    Expr *ic_expr = expr_new(e->arena, EX_INLINE_C, elem_type, vec_span);
                    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
                    ic->return_type = elem_type;
                    ic->captures = NULL; ic->n_captures = 0;
                    if (is_role_recv) {
                        /* SS7: recv-from destructuring:
                         *   vi=0: the tur_router_recv(...) code embedded by elab_recv_from
                         *   vi=1: __TUR_VAL_0__ (same role channel pointer) */
                        InlineC *orig_ic = init_v->as.inline_c_.inline_c;
                        Binding *role_b = (orig_ic->n_val_exprs > 0 && orig_ic->val_exprs[0]
                            && orig_ic->val_exprs[0]->kind == EX_VAR)
                            ? orig_ic->val_exprs[0]->as.var.binding : NULL;
                        if (vi == 0) {
                            ic->code = orig_ic->code;
                            if (role_b) {
                                ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                                Expr *role_var = expr_new(e->arena, EX_VAR, role_b->type, vec_span);
                                role_var->as.var.binding = role_b;
                                ic->val_exprs[0] = role_var;
                                ic->n_val_exprs = 1;
                            } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                        } else {
                            /* vi=1: same void* role channel pointer */
                            static const char ref_role[] = "__TUR_VAL_0__";
                            ic->code = strslice(ref_role, sizeof(ref_role) - 1);
                            if (role_b) {
                                ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                                Expr *role_var = expr_new(e->arena, EX_VAR, role_b->type, vec_span);
                                role_var->as.var.binding = role_b;
                                ic->val_exprs[0] = role_var;
                                ic->n_val_exprs = 1;
                            } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                        }
                    } else if (vi == 0) {
                        if (is_recv_timeout) {
                            /* SS3c: value is in tur__rtv_ thread-local */
                            static const char rtv_code[] = "tur__rtv_";
                            ic->code = strslice(rtv_code, sizeof(rtv_code) - 1);
                            ic->val_exprs = NULL; ic->n_val_exprs = 0;
                        } else {
                            static const char recv_code[] = "tur_session_recv(__TUR_VAL_0__)";
                            ic->code = strslice(recv_code, sizeof(recv_code) - 1);
                            if (chan_b) {
                                ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                                Expr *chan_var = expr_new(e->arena, EX_VAR, chan_b->type, vec_span);
                                chan_var->as.var.binding = chan_b;
                                ic->val_exprs[0] = chan_var;
                                ic->n_val_exprs = 1;
                            } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                        }
                    } else {
                        /* vi=1: the channel pointer */
                        static const char ref_code2[] = "__TUR_VAL_0__";
                        ic->code = strslice(ref_code2, sizeof(ref_code2) - 1);
                        if (is_recv_timeout_var) {
                            /* init_v IS the arm variable (a void* channel pointer) */
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            ic->val_exprs[0] = init_v;  /* EX_VAR for the arm binding */
                            ic->n_val_exprs = 1;
                        } else if (chan_b) {
                            ic->val_exprs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                            Expr *chan_var = expr_new(e->arena, EX_VAR, chan_b->type, vec_span);
                            chan_var->as.var.binding = chan_b;
                            ic->val_exprs[0] = chan_var;
                            ic->n_val_exprs = 1;
                        } else { ic->val_exprs = NULL; ic->n_val_exprs = 0; }
                    }
                    ic_expr->as.inline_c_.inline_c = ic;
                    elem_init = ic_expr;
                }
                binds[n_binds].binding = vb;
                binds[n_binds].init = elem_init;
                binding_moved_during_init[n_binds] = false;
                n_binds++;
            }
            continue;
        }
        if (cur->tag != F_SYM) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding name must be a symbol, got %s",
                      cur->tag == F_INT ? "an integer" :
                      cur->tag == F_STR ? "a string"   :
                      cur->tag == F_KEYWORD ? "a keyword" : "non-symbol");
            rc = -1; break;
        }
        const Symbol *name = cur->as.sym;
        Span name_span = cur->span;
        i++;
        if (i >= bindings_form->as.list.len) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding for '%s' is missing its initializer",
                      name->name);
            rc = -1; break;
        }
        Form *init_form = bindings_form->as.list.items[i++];

        /* Task 1 (Prereq 1): Snapshot move-state of preceding bindings before elaborating this init.
         * This lets us detect which preceding bindings are moved during this init's elaboration. */
        bool *moved_snapshot = NULL;
        if (n_binds > 0) {
            moved_snapshot = (bool *)malloc(n_binds * sizeof(bool));
            if (!moved_snapshot) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t j = 0; j < n_binds; j++) {
                moved_snapshot[j] = binds[j].binding->is_moved;
            }
        }

        Expr *init = elab_form(e, init_form);

        /* Task 2 (Prereq 1): Capture which preceding bindings were newly moved during this init elaboration. */
        if (moved_snapshot) {
            for (uint32_t j = 0; j < n_binds; j++) {
                if (!moved_snapshot[j] && binds[j].binding->is_moved) {
                    binding_moved_during_init[j] = true;
                }
            }
            free(moved_snapshot);
        }

        if (!init) { rc = -1; break; }

        /* Phase 11: Move tracking - if init is a CK_MOVE binding reference, poison it */
        if (init->kind == EX_VAR && type_is_move(init->as.var.binding->type)) {
            binding_mark_moved(init->as.var.binding, init_form->span);
        }

        /* UT1: Alias tracking.
         * (a) When a CK_COPY (non-unique) binding is copied into a new binding,
         *     mark the source as AS_ALIASED so that passing it as ^unique later
         *     is caught (TUR_E0200).
         * (b) When a ^unique binding is transferred (moved), propagate is_unique
         *     to the destination so uniqueness is preserved through rebinding. */
        if (g_unique_enabled && init->kind == EX_VAR) {
            Binding *src = init->as.var.binding;
            if (!src->is_unique && type_is_copy(src->type)) {
                /* CK_COPY binding copied into new name — record alias */
                src->alias_state = AS_ALIASED;
                src->alias_name  = name;
            }
        }

        Binding *b = binding_new(e, name, init->type, is_mut, false, name_span);
        b->is_persistent = is_persistent;
        /* LT0: Mark binding as linear if annotated with ^linear or if initializer
         * type has CK_LINEAR (e.g., returned from a function returning lref<T>). */
        if (is_linear_ann || init->type.copy_kind == CK_LINEAR) {
            b->is_linear = true;
            /* Upgrade copy_kind to CK_LINEAR on the binding's type */
            b->type.copy_kind = CK_LINEAR;
        }
        /* UT0: Mark binding as unique if annotated with ^unique */
        if (is_unique_ann) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
        }
        /* ST0: Mark binding as affine if annotated with ^affine */
        if (is_affine_ann) {
            b->is_affine = true;
            b->type.substruct = SK_AFFINE;
        }
        /* ST0: Mark binding as relevant if annotated with ^relevant */
        if (is_relevant_ann) {
            b->is_relevant = true;
            b->type.substruct = SK_RELEVANT;
        }
        /* UT1: Propagate is_unique through ownership transfer (let [y x] where x is ^unique) */
        if (g_unique_enabled && !is_unique_ann &&
            init->kind == EX_VAR && init->as.var.binding->is_unique) {
            b->is_unique = true;
            /* copy_kind is already CK_UNIQUE, inherited from init->type */
        }
        /* ST1: Propagate affine/relevant through ownership transfer (let [y x] where x is ^affine/^relevant) */
        if (g_substructural_enabled && !is_affine_ann && !is_relevant_ann &&
                init->kind == EX_VAR) {
            Binding *src = init->as.var.binding;
            if (src->is_affine) {
                b->is_affine = true;
                b->type.substruct = SK_AFFINE;
            }
            if (src->is_relevant) {
                b->is_relevant = true;
                b->type.substruct = SK_RELEVANT;
            }
        }
        /* ST3: Propagate affine/relevant through expression type (e.g. from must-use macro
         * returning an EX_LET whose type carries SK_RELEVANT). Only fires when not already
         * set by the EX_VAR propagation above. */
        if (g_substructural_enabled && !is_affine_ann && !is_relevant_ann &&
                !b->is_affine && !b->is_relevant && init->kind != EX_VAR) {
            if (init->type.substruct == SK_RELEVANT) {
                b->is_relevant = true;
                b->type.substruct = SK_RELEVANT;
            } else if (init->type.substruct == SK_AFFINE) {
                b->is_affine = true;
                b->type.substruct = SK_AFFINE;
            }
        }
        /* ST2: Under -Xsubstructural, ref<T> bindings are inferred as SK_LINEAR
         * unless an explicit substructural annotation is already present. */
        if (g_substructural_enabled && !is_linear_ann && !is_affine_ann && !is_relevant_ann
                && !b->is_linear && init && init->type.kind == TY_REF) {
            b->is_linear = true;
            b->type.substruct = SK_LINEAR;
        }
        scope_add(&inner, b);

        if (n_binds == cap) {
            cap = cap ? cap * 2 : 4;
            binds = (LetBinding *)realloc(binds, cap * sizeof(LetBinding));
            if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
            binding_moved_during_init = (bool *)realloc(binding_moved_during_init, cap * sizeof(bool));
            if (!binding_moved_during_init) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        
        /* Phase 3: If init is a closure, set closure_fn_binding on the binding */
        if (init && init->kind == EX_CLOSURE) {
            struct Closure *closure = init->as.closure_.closure;
            b->closure_fn_binding = closure->fn->binding;
        }
        /* Phase HRT4: propagate poly fn metadata through let-bindings.
         * (let [g f] ...) where f is is_poly_fn → g inherits is_poly_fn.
         * (let [g id] ...) where id is a global TY_FN → g.source_binding = id. */
        if (init && init->kind == EX_VAR) {
            Binding *init_b = init->as.var.binding;
            if (init_b->is_poly_fn) {
                b->is_poly_fn = true;
                b->poly_type  = init_b->poly_type;
            } else if (init_b->type.kind == TY_FN) {
                /* Follow any existing source chain to the root global fn. */
                Binding *root = init_b->source_binding ? init_b->source_binding : init_b;
                if (root->is_global) b->source_binding = root;
            }
        }
        
        binds[n_binds].binding = b;
        binds[n_binds].init = init;
        binding_moved_during_init[n_binds] = false; /* new binding, not yet moved during init */
        n_binds++;
    }

    /* Phase 5: Check if any binding is a ref and needs auto-defer drop */
    bool has_ref_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        /* Skip refs that come from ref/from-rc - they don't own the data */
        /* ST2: Skip refs that are marked linear — LT1 scope-exit check handles those */
        if (binds[k].binding->type.kind == TY_REF &&
            binds[k].init->kind != EX_REF_FROM_RC &&
            !binds[k].binding->is_linear) {
            has_ref_bindings = true;
            break;
        }
    }

    /* The binding_moved_during_init array (built during the binding loop) records which
     * bindings were moved during init elaboration of subsequent bindings in this let form.
     * Combined with is_moved (which also captures body-phase moves), this gives complete
     * move-state tracking across all elaboration phases. */

    Expr *body = NULL;
    if (rc == 0) {
        uint32_t body_count = call->as.list.len - 2;
        if (body_count == 0) {
            body = e_nil(e, call->span);
        } else if (body_count == 1) {
            body = elab_form(e, call->as.list.items[2]);
            if (!body) rc = -1;
        } else {
            Expr **items = (Expr **)arena_alloc(e->arena, body_count * sizeof(Expr *));
            for (uint32_t k = 0; k < body_count; k++) {
                items[k] = elab_form(e, call->as.list.items[2 + k]);
                if (!items[k]) { rc = -1; break; }
            }
            if (rc == 0) {
                body = expr_new(e->arena, EX_DO, items[body_count - 1]->type, call->span);
                body->as.do_.items = items;
                body->as.do_.n = body_count;
            }
        }
        
        /* Phase 5: If we have ref bindings and the body is a single expression
         * (not a do), wrap it in a do so we can add defers */
        if (has_ref_bindings && body && body->kind != EX_DO) {
            Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
            items[0] = body;
            body = expr_new(e->arena, EX_DO, body->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = 1;
        }
        
        /* Phase 5: Inject defers for ref bindings into the do body */
        if (has_ref_bindings && body && body->kind == EX_DO) {
            /* We need to add defer expressions to the do body */
            /* First, collect all ref binding names that need drops (excluding moved ones) */
            uint32_t n_refs = 0;
            for (uint32_t k = 0; k < n_binds; k++) {
                /* Skip refs that come from ref/from-rc - they don't own the data */
                /* Skip refs that were moved during init or body elaboration - avoid use-after-move defer */
                /* ST2: Skip linear refs — LT1 scope-exit check enforces they are consumed explicitly */
                if (binds[k].binding->type.kind == TY_REF &&
                    binds[k].init->kind != EX_REF_FROM_RC &&
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved &&
                    !binds[k].binding->is_linear &&
                    !is_binding_consumed(body, binds[k].binding)) {
                    n_refs++;
                }
            }
            
            if (n_refs > 0) {
                /* Create new items array with space for defers */
                uint32_t new_n = body->as.do_.n + n_refs;
                Expr **new_items = (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));
                
                /* Copy existing items */
                memcpy(new_items, body->as.do_.items, body->as.do_.n * sizeof(Expr *));
                
                /* Add defer expressions for each ref binding at the end */
                /* Note: defers execute in LIFO order, so we add them in order and they'll
                 * fire in reverse order. But since we're adding them at the end of the
                 * items array, they'll be after the actual body expressions, which is
                 * what we want for scope-exit behavior. */
                uint32_t defer_idx = body->as.do_.n;
                for (uint32_t k = 0; k < n_binds; k++) {
                    /* Skip refs that come from ref/from-rc - they don't own the data */
                    /* Skip refs moved during init or body elaboration - avoid use-after-move defer */
                    /* ST2: Skip linear refs — they must be consumed explicitly; LT1 enforces this */
                    if (binds[k].binding->type.kind == TY_REF &&
                        binds[k].init->kind != EX_REF_FROM_RC &&
                        !binding_moved_during_init[k] &&
                        !binds[k].binding->is_moved &&
                        !binds[k].binding->is_linear &&
                        !is_binding_consumed(body, binds[k].binding)) {
                        /* Create (defer (drop! binding_name)) expression */
                        /* Create a variable reference to the binding */
                        Expr *var_expr = expr_new(e->arena, EX_VAR, binds[k].binding->type, call->span);
                        var_expr->as.var.binding = binds[k].binding;
                        
                        /* Look up the drop! builtin spec and create a BUILTIN expression */
                        const BuiltinSpec *spec = builtin_lookup(e->sym_drop, binds[k].binding->type, 1);
                        if (!spec) {
                            diag_emit(DIAG_ERROR, call->span,
                                      "internal error: drop! builtin not found for ref<T>");
                            rc = -1;
                            break;
                        }
                        
                        /* Create the drop! builtin call */
                        Expr *drop_call = expr_new(e->arena, EX_BUILTIN, TYPE_NIL, call->span);
                        drop_call->as.builtin.spec = spec;
                        drop_call->as.builtin.n = 1;
                        drop_call->as.builtin.args = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                        drop_call->as.builtin.args[0] = var_expr;
                        
                        /* Create the defer expression */
                        Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                        defer_expr->as.defer_.body = drop_call;
                        /* Capture analysis for defer body */
                        /* Collect free variables in the defer body (the drop! call references the binding) */
                        uint32_t n_free = 0;
                        Binding **free_vars = collect_free_vars(drop_call, NULL, 0, &n_free);
                        
                        Binding **captures = NULL;
                        uint8_t n_captures = 0;
                        if (n_free > 0) {
                            captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
                            memcpy(captures, free_vars, n_free * sizeof(Binding *));
                            n_captures = (uint8_t)n_free;
                        }
                        free(free_vars);
                        
                        defer_expr->as.defer_.captures = captures;
                        defer_expr->as.defer_.n_captures = n_captures;
                        
                        new_items[defer_idx++] = defer_expr;
                    }
                }
                
                /* Update the body with new items */
                body->as.do_.items = new_items;
                body->as.do_.n = new_n;
            }
        }
    }

    /* Phase 5b / EXG1-5: RC auto-drop injection with consumption detection.
     * Inject (defer (rc/drop x)) for let-bound rc/of values that are:
     * 1. Not consumed by ref/from-rc (which would transfer ownership and cause double-free)
     * 2. Not explicitly dropped via (rc/drop x)
     * 3. Not moved to another binding
     *
     * EXG1-5: constrained existentials (`(exists [a] [(C a) ...] T)`) are
     * allocated through rc_cb_alloc by emit_expr.c, so their bindings need
     * the same scope-exit decrement.  We piggy-back on the same EX_RC_DROP
     * mechanism: at codegen time the binding's value is a void* that
     * implicitly converts to RcControlBlock* for rc_strong_decrement.
     * Unconstrained existentials (no witnesses) are unchanged — they do
     * not allocate, so they need no drop. */
    bool has_rc_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        Type bt = binds[k].binding->type;
        bool is_rc_managed = bt.kind == TY_RC ||
            (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0);
        if (is_rc_managed) {
            has_rc_bindings = true;
            break;
        }
    }

    /* If we have rc bindings, wrap body in do if needed and inject defers */
    if (has_rc_bindings && body && body->kind != EX_DO) {
        Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        items[0] = body;
        body = expr_new(e->arena, EX_DO, body->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = 1;
    }

    if (has_rc_bindings && body && body->kind == EX_DO) {
        /* Count rc bindings that need auto-drop (excluding consumed/moved ones) */
        uint32_t n_rc_drops = 0;
        for (uint32_t k = 0; k < n_binds; k++) {
            Type bt = binds[k].binding->type;
            bool is_rc_managed = bt.kind == TY_RC ||
                (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0);
            if (is_rc_managed &&
                !binding_moved_during_init[k] &&
                !binds[k].binding->is_moved &&
                !is_binding_consumed(body, binds[k].binding)) {
                n_rc_drops++;
            }
        }

        if (n_rc_drops > 0) {
            /* Create new items array with space for rc drop defers */
            uint32_t new_n = body->as.do_.n + n_rc_drops;
            Expr **new_items = (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));

            /* Copy existing items */
            memcpy(new_items, body->as.do_.items, body->as.do_.n * sizeof(Expr *));

            /* Add defer expressions for each unconsumed rc binding */
            uint32_t defer_idx = body->as.do_.n;
            for (uint32_t k = 0; k < n_binds; k++) {
                /* Skip RC bindings that are moved or consumed */
                Type bt = binds[k].binding->type;
                bool is_rc_managed = bt.kind == TY_RC ||
                    (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0);
                if (is_rc_managed &&
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved &&
                    !is_binding_consumed(body, binds[k].binding)) {
                    
                    /* Create a variable reference to the rc binding */
                    Expr *var_expr = expr_new(e->arena, EX_VAR, binds[k].binding->type, call->span);
                    var_expr->as.var.binding = binds[k].binding;
                    
                    /* Create the rc/drop expression */
                    Expr *rc_drop_expr = expr_new(e->arena, EX_RC_DROP, TYPE_NIL, call->span);
                    rc_drop_expr->as.rc_drop_.expr = var_expr;
                    
                    /* Create the defer expression wrapping the rc/drop */
                    Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                    defer_expr->as.defer_.body = rc_drop_expr;
                    
                    /* Capture analysis */
                    uint32_t n_free = 0;
                    Binding **free_vars = collect_free_vars(rc_drop_expr, NULL, 0, &n_free);
                    
                    Binding **captures = NULL;
                    uint8_t n_captures = 0;
                    if (n_free > 0) {
                        captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
                        memcpy(captures, free_vars, n_free * sizeof(Binding *));
                        n_captures = (uint8_t)n_free;
                    }
                    free(free_vars);
                    
                    defer_expr->as.defer_.captures = captures;
                    defer_expr->as.defer_.n_captures = n_captures;
                    
                    new_items[defer_idx++] = defer_expr;
                }
            }
            
            /* Update the body with new items */
            body->as.do_.items = new_items;
            body->as.do_.n = new_n;
        }
    }

    /* LT1: At scope exit, verify all linear bindings were consumed */
    if (g_linear_enabled && rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            Binding *lb = binds[k].binding;
            if (lb->is_linear && !lb->is_linear_consumed && !lb->is_moved) {
                diag_emit_with_code(DIAG_ERROR, lb->span,
                                    TUR_E0100_LINEAR_DROPPED,
                                    "linear value '%s' dropped without being consumed",
                                    lb->name->name);
                rc = -1;
            }
        }
    }

    /* ST1: At scope exit, verify all relevant bindings were used at least once */
    if (g_substructural_enabled && rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            Binding *lb = binds[k].binding;
            if (lb->is_relevant && lb->usage_state == USAGE_UNUSED && !lb->is_moved) {
                diag_emit_with_code(DIAG_ERROR, lb->span,
                                    TUR_E0151_RELEVANT_DROPPED,
                                    "relevant value '%s' dropped without being used",
                                    lb->name->name);
                rc = -1;
            }
        }
    }

    /* Pop scope before returning. */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Clean up move-state tracking memory */
    if (binding_moved_during_init) {
        free(binding_moved_during_init);
    }

    if (rc != 0) { free(binds); return NULL; }

    Expr *out = expr_new(e->arena, EX_LET, body->type, call->span);
    LetBinding *bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
    memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    free(binds);
    out->as.let_.bindings = bcopy;
    out->as.let_.n = n_binds;
    out->as.let_.body = body;
    return out;
}

Expr *elab_do(Elab *e, const Form *call) {
    /* (do body...) — value of last expr; (do) is nil. */
    uint32_t n = call->as.list.len - 1;
    if (n == 0) return e_nil(e, call->span);

    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) return NULL;
    }
    
    /* Phase R6: Warn on discarded result values */
    if (g_warn_unused_result) {
        /* Check if this is an ignore! pattern: (do <expr> nil) */
        bool is_ignore_pattern = (n == 2 && items[1]->kind == EX_NIL_LIT);
        
        for (uint32_t i = 0; i < n - 1; i++) {
            /* All items except the last have their values discarded */
            if (items[i]->type.kind == TY_PTR_VOID) {
                /* This is a ptr<void> value being discarded - likely a Result */
                /* Skip warning if this is the ignore! pattern: (do expr nil) */
                if (!is_ignore_pattern || i != 0) {
                    diag_emit(DIAG_WARNING, items[i]->span,
                              "discarded result value of type ptr<void>; use ignore! to suppress this warning");
                }
            }
        }
    }
    
    Expr *out = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
    out->as.do_.items = items;
    out->as.do_.n = n;
    return out;
}

Expr *elab_if(Elab *e, const Form *call) {
    if (call->as.list.len != 3 && call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "if expects (if cond then) or (if cond then else); got %u argument(s)",
                  call->as.list.len - 1);
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "if condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }

    Binding **move_bindings = NULL;
    bool *before_states = NULL;
    uint32_t n_move_bindings = move_state_snapshot_bindings(e->scope, &move_bindings, &before_states);

    /* LT1: Snapshot linear consumption state before branches. */
    Binding **lin_bindings = NULL;
    bool *lin_before = NULL;
    uint32_t n_lin = 0;
    if (g_linear_enabled) {
        n_lin = linear_state_snapshot_bindings(e->scope, &lin_bindings, &lin_before);
    }

    Expr *then_ = elab_form(e, call->as.list.items[2]);
    if (!then_) {
        free(move_bindings);
        free(before_states);
        free(lin_bindings);
        free(lin_before);
        return NULL;
    }
    bool *then_states = move_state_capture_current(move_bindings, n_move_bindings);

    /* LT1: Capture linear consumption state after then-branch. */
    bool *lin_then = NULL;
    if (g_linear_enabled && n_lin > 0) {
        lin_then = linear_state_capture_current(lin_bindings, n_lin);
    }

    /* Rewind to pre-branch move-state before elaborating else branch. */
    move_state_restore(move_bindings, before_states, n_move_bindings);

    /* LT1: Rewind linear consumption state before elaborating else branch. */
    if (g_linear_enabled && n_lin > 0) {
        linear_state_restore(lin_bindings, lin_before, n_lin);
    }

    Expr *else_ = NULL;
    Type result_t = TYPE_NIL;
    if (call->as.list.len == 4) {
        else_ = elab_form(e, call->as.list.items[3]);
        if (!else_) {
            move_state_restore(move_bindings, before_states, n_move_bindings);
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            return NULL;
        }
        bool *else_states = move_state_capture_current(move_bindings, n_move_bindings);

        /* A move is guaranteed after if/else only if it was present before,
         * or both branches moved the binding. */
        for (uint32_t i = 0; i < n_move_bindings; i++) {
            move_bindings[i]->is_moved = before_states[i] || (then_states[i] && else_states[i]);
        }
        free(else_states);

        /* A branch that diverges — its top-level expression is a return,
         * throw, panic, panic-with, or has `!` (TYPE_NEVER) type — is
         * compatible with any other branch.  The if's result type is then
         * the non-diverging branch's type.  This lets `(? expr)` lower to
         * `(if cond (return ...) ok-val)` even when the two branch types
         * differ. */
        bool then_div = (then_->type.kind == TY_NEVER) ||
                        (then_->kind == EX_RETURN) ||
                        (then_->kind == EX_PANIC)  ||
                        (then_->kind == EX_PANIC_WITH);
        bool else_div = (else_->type.kind == TY_NEVER) ||
                        (else_->kind == EX_RETURN) ||
                        (else_->kind == EX_PANIC)  ||
                        (else_->kind == EX_PANIC_WITH);

        /* LT1: Capture linear state after else, check for branch mismatch, merge. */
        if (g_linear_enabled && n_lin > 0) {
            bool *lin_else = linear_state_capture_current(lin_bindings, n_lin);
            bool lin_ok = true;
            for (uint32_t i = 0; i < n_lin; i++) {
                if (lin_before[i]) continue; /* already consumed before if; fine */
                /* Skip mismatch check for diverging branches. */
                if (!then_div && !else_div && lin_then[i] != lin_else[i]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                                        TUR_E0104_LINEAR_BRANCH_MISMATCH,
                                        "linear value '%s' consumed in one branch but not the other"
                                        " -- consume it in both branches or neither",
                                        lin_bindings[i]->name->name);
                    lin_ok = false;
                }
                /* Merge: use surviving branch's consumed state. */
                bool merged;
                if (then_div && else_div) {
                    merged = false; /* both diverge; treat as unconsumed */
                } else if (then_div) {
                    merged = lin_else[i];
                } else if (else_div) {
                    merged = lin_then[i];
                } else {
                    merged = lin_then[i] && lin_else[i];
                }
                lin_bindings[i]->is_linear_consumed = merged;
            }
            free(lin_else);
            if (!lin_ok) {
                free(then_states);
                free(move_bindings);
                free(before_states);
                free(lin_then);
                free(lin_bindings);
                free(lin_before);
                return NULL;
            }
        }

        if (then_div && else_div) {
            result_t = then_->type;  /* both diverge; pick either */
        } else if (then_div) {
            result_t = else_->type;
        } else if (else_div) {
            result_t = then_->type;
        } else if (!type_eq(then_->type, else_->type)) {
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            diag_emit(DIAG_ERROR, call->span,
                      "if branches have mismatched types: then=%s else=%s",
                      type_name(then_->type), type_name(else_->type));
            return NULL;
        } else {
            result_t = then_->type;
        }
    } else {
        /* Without else, then-branch moves are not guaranteed after the if. */
        move_state_restore(move_bindings, before_states, n_move_bindings);
        /* LT1: Without else, then-branch linear consumption is not guaranteed;
         * restore to the pre-if state so scope-exit checking fires if needed. */
        if (g_linear_enabled && n_lin > 0) {
            linear_state_restore(lin_bindings, lin_before, n_lin);
        }
    }

    free(then_states);
    free(move_bindings);
    free(before_states);
    free(lin_then);
    free(lin_bindings);
    free(lin_before);

    /* If no else, the if is a statement-style branch with type nil
     * (matches Clojure's behavior of returning nil for a missing else). */
    Expr *out = expr_new(e->arena, EX_IF, result_t, call->span);
    out->as.if_.cond = cond;
    out->as.if_.then_ = then_;
    out->as.if_.else_or_null = else_;
    return out;
}

/* Phase 6: Threading macro ->  */
/* (-> x (f a) (g b)) expands to (g (f x a) b) */
Expr *elab_thread(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "-> requires at least one argument");
        return NULL;
    }
    
    /* Start with the initial value as a Form */
    Form *current = call->as.list.items[1];
    
    /* Process remaining forms */
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *form = call->as.list.items[i];
        if (form->tag == F_SYM) {
            /* (-> x f) -> (f x) */
            current = form_list(e->arena, call->span,
                (Form *[]){form, current}, 2);
        } else if (form->tag == F_LIST) {
            /* (-> x (f a b)) -> (f x a b) */
            /* Prepend current to the list arguments */
            uint32_t n = form->as.list.len;
            Form **new_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
            new_items[0] = form->as.list.items[0]; /* function name */
            new_items[1] = current; /* insert current as first arg */
            for (uint32_t j = 1; j < n; j++) {
                new_items[j + 1] = form->as.list.items[j];
            }
            current = form_list(e->arena, call->span, new_items, n + 1);
        } else {
            diag_emit(DIAG_ERROR, form->span,
                      "-> expected symbol or list, got %s",
                      form->tag == F_VEC ? "vector" : "other");
            return NULL;
        }
    }
    
    /* Elaborate the final form */
    return elab_form(e, current);
}

/* Phase 6: Threading macro ->>  */
/* (->> x (f a) (g b)) expands to (g b (f x a)) - value as last arg */
Expr *elab_thread_last(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "->> requires at least one argument");
        return NULL;
    }
    
    /* Start with the initial value as a Form */
    Form *current = call->as.list.items[1];
    
    /* Process remaining forms */
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *form = call->as.list.items[i];
        if (form->tag == F_SYM) {
            /* (->> x f) -> (f x) - same as -> for single arg */
            current = form_list(e->arena, call->span,
                (Form *[]){form, current}, 2);
        } else if (form->tag == F_LIST) {
            /* (->> x (f a b)) -> (f a b x) - append current as last arg */
            uint32_t n = form->as.list.len;
            Form **new_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
            for (uint32_t j = 0; j < n; j++) {
                new_items[j] = form->as.list.items[j];
            }
            new_items[n] = current; /* append current as last arg */
            current = form_list(e->arena, call->span, new_items, n + 1);
        } else {
            diag_emit(DIAG_ERROR, form->span,
                      "->> expected symbol or list, got %s",
                      form->tag == F_VEC ? "vector" : "other");
            return NULL;
        }
    }
    
    /* Elaborate the final form */
    return elab_form(e, current);
}

Expr *elab_set(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "set! takes (set! name value)");
        return NULL;
    }
    Form *target = call->as.list.items[1];

    /* Phase 12: Handle (set! (@ r) value) - mutation through mutable borrow */
    if (target->tag == F_LIST && target->as.list.len == 2
        && target->as.list.items[0]->tag == F_SYM
        && target->as.list.items[0]->as.sym == e->sym_deref) {
        return elab_set_deref(e, call, target);
    }

    if (target->tag != F_SYM) {
        diag_emit(DIAG_ERROR, target->span, "set! target must be a symbol or (@ borrow)");
        return NULL;
    }
    Binding *b = scope_lookup(e->scope, target->as.sym);
    if (!b) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is not bound", target->as.sym->name);
        return NULL;
    }

    /* DV1: If the target is a dynamic var, dispatch to dynvar set path */
    if (b->is_dynvar && b->dynvar_entry) {
        DynVarEntry *entry = b->dynvar_entry;

        /* TUR-E0605: set! on dynvar inside atomically is disallowed */
        if (elab_in_atomically) {
            diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0605_DYNVAR_SET_IN_ATOMIC,
                                "set!: cannot mutate dynamic var '%s' inside an "
                                "atomically block (mutation cannot be rolled back on retry)",
                                entry->name->name);
            return NULL;
        }

        /* TUR-E0601: no active binding frame for this var */
        bool frame_active = false;
        for (uint32_t i = 0; i < e->n_active_dynvar_bindings; i++) {
            if (e->active_dynvar_bindings[i] == entry->name) {
                frame_active = true;
                break;
            }
        }
        if (!frame_active) {
            diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0601_DYNVAR_SET_NO_BINDING,
                                "set!: '%s' has no active binding frame on this thread; "
                                "wrap in (binding [%s ...] ...) first",
                                entry->name->name, entry->name->name);
            return NULL;
        }

        Expr *value = elab_form(e, call->as.list.items[2]);
        if (!value) return NULL;

        /* TUR-E0602: type must match declared value type */
        if (value->type.kind != entry->value_type.kind) {
            diag_emit_with_code(DIAG_ERROR, value->span, TUR_E0602_DYNVAR_TYPE_MISMATCH,
                                "set!: '%s' declared as '%s' but value has type '%s'",
                                entry->name->name, type_name(entry->value_type),
                                type_name(value->type));
            return NULL;
        }

        Expr *out = expr_new(e->arena, EX_DYNVAR_SET, TYPE_NIL, call->span);
        out->as.dynvar_set_.entry = entry;
        out->as.dynvar_set_.value = value;
        return out;
    }

    /* Phase 11: Check if target binding has been moved */
    if (b->is_moved) {
        diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0005_USE_AFTER_MOVE,
                            "use-after-move: cannot set! '%s' because it was moved",
                            b->name->name);
        return NULL;
    }
    if (!b->is_mut) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is immutable; use ^mut at the binding site to allow it",
                  b->name->name);
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    if (!type_eq(value->type, b->type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set!: value type %s does not match binding type %s",
                  type_name(value->type), type_name(b->type));
        return NULL;
    }

    /* Phase 11: Move tracking - if value is a CK_MOVE binding reference, poison it */
    if (value->kind == EX_VAR && type_is_move(value->as.var.binding->type)) {
        binding_mark_moved(value->as.var.binding, value->span);
    }

    Expr *out = expr_new(e->arena, EX_SET, TYPE_NIL, call->span);
    out->as.set_.target = b;
    out->as.set_.value = value;
    return out;
}

Expr *elab_while(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "while requires a condition");
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "while condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }
    uint32_t n = call->as.list.len - 2;
    Expr *body;
    if (n == 0) body = e_nil(e, call->span);
    else if (n == 1) {
        body = elab_form(e, call->as.list.items[2]);
        if (!body) return NULL;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) {
            items[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!items[i]) return NULL;
        }
        body = expr_new(e->arena, EX_DO, TYPE_NIL, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }
    Expr *out = expr_new(e->arena, EX_WHILE, TYPE_NIL, call->span);
    out->as.while_.cond = cond;
    out->as.while_.body = body;
    return out;
}

/* case desugars to (let [g disc] (if (= g v1) e1 (if (= g v2) e2 ... eN))).
 * Syntax: (case val v1 e1 v2 e2 ... :else eN) */
Expr *elab_case(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "case requires (case val v1 e1 ...), got empty call");
        return NULL;
    }
    uint32_t n = call->as.list.len - 2; /* clause forms after discriminant */
    if ((n & 1) != 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "case expects pairs of (value expr) after discriminant; got %u clause forms", n);
        return NULL;
    }

    /* Generate a gensym for the discriminant to avoid multiple evaluation. */
    char disc_name[32];
    snprintf(disc_name, sizeof(disc_name), "case_disc_%u", e->next_gensym_id++);
    const Symbol *disc_sym = symtab_intern(e->st, strslice(disc_name, (uint32_t)strlen(disc_name)));
    Form *disc_sym_f = form_sym(e->arena, call->span, disc_sym);

    /* Intern = symbol for building (= g vi) tests */
    const Symbol *sym_eq = symtab_intern(e->st, strslice("=", 1));
    Form *sym_eq_f = form_sym(e->arena, call->span, sym_eq);

    /* Intern 'if' and ':else' for building the nested if tree */
    Form *sym_if_f  = form_sym(e->arena, call->span, e->sym_if);

    /* Build the nested if tree right-to-left */
    Form *acc = form_nil(e->arena, call->span);
    for (int i = (int)n - 2; i >= 0; i -= 2) {
        Form *val  = call->as.list.items[2 + i];
        Form *body = call->as.list.items[2 + i + 1];

        if (val->tag == F_KEYWORD && val->as.sym == e->kw_else) {
            /* :else — replace acc with the else body */
            acc = body;
            continue;
        }

        /* Build (= disc_sym val) */
        Form *eq_items[3] = { sym_eq_f, disc_sym_f, val };
        Form *test_f = form_list(e->arena, val->span, eq_items, 3);

        /* Build (if test body acc) */
        Form *if_items[4] = { sym_if_f, test_f, body, acc };
        acc = form_list(e->arena, val->span, if_items, 4);
    }

    /* Wrap in (let [disc_sym disc_expr] acc) */
    Form *disc_f = call->as.list.items[1];
    Form *bind_vec_items[2] = { disc_sym_f, disc_f };
    Form *bind_vec = form_vec(e->arena, call->span, bind_vec_items, 2);
    Form *sym_let_f = form_sym(e->arena, call->span, e->sym_let);
    Form *let_items[3] = { sym_let_f, bind_vec, acc };
    Form *let_form = form_list(e->arena, call->span, let_items, 3);

    return elab_form(e, let_form);
}

/* Phase 4: defer — (defer expr)
 * Records an expression to be evaluated at scope exit, in LIFO order.
 * For now, only valid inside let/do/defn/while bodies.
 * The body is elaborated but its value is discarded (defer always evaluates to nil).
 * v1 lowering (effects-plan.md §6.10): performs capture analysis for thunk lifting.
 * Nested defers are not yet supported.
 */
Expr *elab_defer(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "defer requires an expression");
        return NULL;
    }
    /* Phase M5: module-level defer (top-level scope) is allowed.
     * The body runs at process exit via atexit(). No captures — at global
     * scope all referenced names are already global bindings. */
    if (e->scope == &e->global) {
        Expr *body = elab_form(e, call->as.list.items[1]);
        if (!body) return NULL;
        Expr *out = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
        out->as.defer_.body       = body;
        out->as.defer_.captures   = NULL;
        out->as.defer_.n_captures = 0;
        return out;
    }
    /* Elaborate the body expression */
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;
    
    /* v1 lowering: Collect free variables (captures) for thunk lifting.
     * Per effects-plan.md §6.10.1, defers are entries in a list-on-frame.
     * For defers that reference local variables, we need to capture them
     * in an env struct (same pattern as closures in Phase 3).
     * 
     * For defer thunks (unlike closure thunks), there are no "params" — all
     * non-global bindings referenced in the defer body need to be captured
     * since the thunk is a separate function at file scope.
     * We pass empty params to collect_free_vars so all non-global bindings
     * are treated as free variables (captures). */
    Binding **captures = NULL;
    uint8_t n_captures = 0;
    
    /* Collect free variables in the defer body - pass empty params
     * so all non-global bindings are captured */
    uint32_t n_free = 0;
    Binding **free_vars = collect_free_vars(body, NULL, 0, &n_free);
    
    if (n_free > 0) {
        /* Store captures in the defer expression (arena-allocated, lives for compilation) */
        captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
        memcpy(captures, free_vars, n_free * sizeof(Binding *));
        n_captures = (uint8_t)n_free;
    }
    
    free(free_vars);
    
    /* Create EX_DEFER expression with capture info */
    Expr *out = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
    out->as.defer_.body = body;
    out->as.defer_.captures = captures;
    out->as.defer_.n_captures = n_captures;
    return out;
}

/* Phase 3/4: return — (return) or (return expr)
 * Early return from a function, firing all defers in the scope chain.
 * 
 * Grammar: (return) or (return expr)
 * The return value type must match the function's return type.
 * 
 * Per effects-plan.md §6.10: "return: walk every enclosing frame, fire defers 
 * per frame, then function-exit." The codegen emits tur_frame_fire_chain to 
 * walk the parent chain and fire all defers before returning.
 */
Expr *elab_return(Elab *e, const Form *call) {
    /* return is only valid inside function bodies */
    if (e->scope == &e->global) {
        diag_emit(DIAG_ERROR, call->span,
                  "return is not allowed at module top level");
        return NULL;
    }
    
    /* Check number of arguments */
    if (call->as.list.len > 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "return takes at most one argument: (return) or (return expr)");
        return NULL;
    }
    
    Expr *value = NULL;
    if (call->as.list.len == 2) {
        /* (return expr) */
        value = elab_form(e, call->as.list.items[1]);
        if (!value) return NULL;

        /* Phase 11: returning a move-only binding transfers ownership. */
        if (value->kind == EX_VAR && type_is_move(value->as.var.binding->type)) {
            binding_mark_moved(value->as.var.binding, value->span);
        }
    }
    
    /* Create EX_RETURN expression */
    /* The type will be determined by the function's return type during type checking.
     * For now, use the value's type or NIL if no value. */
    Type return_type = value ? value->type : TYPE_NIL;
    Expr *out = expr_new(e->arena, EX_RETURN, return_type, call->span);
    out->as.return_.value = value;
    return out;
}

/* Phase R1: ? operator — (? expr)
 *
 * Lowers to:
 *   (let [__q_N expr]
 *     (if (err? __q_N)
 *         (return (err (err-val __q_N)))
 *         (ok-val __q_N)))
 *
 * Requires `err?`, `err`, `err-val`, and `ok-val` to be in scope (the fixture
 * defines these alongside the result-shaped type).  Must appear inside a
 * function body.  The if branches have types `!` (from `return`) and the
 * unwrapped ok type; `elab_if` accepts NEVER as compatible with any type and
 * propagates the other branch's type as the result.
 */
Expr *elab_question(Elab *e, const Form *call) {
    if (e->fn_body_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "? operator is only allowed inside a function body");
        return NULL;
    }
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "? operator requires exactly one argument: (? expr)");
        return NULL;
    }

    Form *inner = call->as.list.items[1];
    Span span = call->span;

    /* Fresh symbol __q_N to avoid multiple evaluation of expr. */
    char q_name[32];
    snprintf(q_name, sizeof(q_name), "__q_%u", e->next_gensym_id++);
    const Symbol *q_sym = symtab_intern(e->st,
        strslice(q_name, (uint32_t)strlen(q_name)));
    Form *q_sym_f = form_sym(e->arena, span, q_sym);

    /* User-defined result helpers — looked up by name in scope. */
    const Symbol *sym_err_q   = symtab_intern(e->st, strslice("err?",    4));
    const Symbol *sym_err     = symtab_intern(e->st, strslice("err",     3));
    const Symbol *sym_err_val = symtab_intern(e->st, strslice("err-val", 7));
    const Symbol *sym_ok_val  = symtab_intern(e->st, strslice("ok-val",  6));

    Form *err_q_f   = form_sym(e->arena, span, sym_err_q);
    Form *err_f     = form_sym(e->arena, span, sym_err);
    Form *err_val_f = form_sym(e->arena, span, sym_err_val);
    Form *ok_val_f  = form_sym(e->arena, span, sym_ok_val);
    Form *if_f      = form_sym(e->arena, span, e->sym_if);
    Form *let_f     = form_sym(e->arena, span, e->sym_let);
    Form *return_f  = form_sym(e->arena, span, e->sym_return);

    /* (err? __q) */
    Form *test_items[2] = { err_q_f, q_sym_f };
    Form *test_form = form_list(e->arena, span, test_items, 2);

    /* (err-val __q) */
    Form *err_val_items[2] = { err_val_f, q_sym_f };
    Form *err_val_form = form_list(e->arena, span, err_val_items, 2);

    /* (err (err-val __q)) */
    Form *err_call_items[2] = { err_f, err_val_form };
    Form *err_call_form = form_list(e->arena, span, err_call_items, 2);

    /* (return (err (err-val __q))) */
    Form *return_items[2] = { return_f, err_call_form };
    Form *return_form = form_list(e->arena, span, return_items, 2);

    /* (ok-val __q) */
    Form *ok_val_items[2] = { ok_val_f, q_sym_f };
    Form *ok_val_form = form_list(e->arena, span, ok_val_items, 2);

    /* (if (err? __q) (return (err (err-val __q))) (ok-val __q)) */
    Form *if_items[4] = { if_f, test_form, return_form, ok_val_form };
    Form *if_form = form_list(e->arena, span, if_items, 4);

    /* (let [__q expr] if_form) */
    Form *bind_vec_items[2] = { q_sym_f, inner };
    Form *bind_vec = form_vec(e->arena, span, bind_vec_items, 2);
    Form *let_items[3] = { let_f, bind_vec, if_form };
    Form *let_form = form_list(e->arena, span, let_items, 3);

    return elab_form(e, let_form);
}

/* Phase 12: Borrow traits */

/* Elaborate (set! (@ r) value) - mutation through a mutable borrow.
 *
 * Called when elab_set detects a deref-assignment target: (set! (@ r) value).
 * Only &mut T is allowed as the borrow; &T produces an immutable-borrow diagnostic.
 */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form) {
    /* Elaborate the borrow expression r */
    Expr *ref = elab_form(e, deref_form->as.list.items[1]);
    if (!ref) return NULL;

    /* Must be &mut T */
    if (ref->type.kind == TY_REF_IMMUT) {
        diag_emit(DIAG_ERROR, deref_form->span,
                  "cannot assign through immutable borrow; use `&mut T` for mutation");
        return NULL;
    }
    if (ref->type.kind != TY_REF_MUT) {
        diag_emit(DIAG_ERROR, deref_form->span,
                  "set! via @ requires a &mut T borrow, got %s",
                  type_name(ref->type));
        return NULL;
    }

    /* Elaborate the value */
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;

    /* Type-check: value must match the inner type */
    Type inner_type = type_from_kind(ref->type.as.ref_borrow.target);
    if (!type_eq(value->type, inner_type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set! type mismatch: cannot assign %s through &mut %s borrow",
                  type_name(value->type), type_name(inner_type));
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_SET_DEREF, TYPE_NIL, call->span);
    out->as.set_deref_.ref = ref;
    out->as.set_deref_.value = value;
    return out;
}

/* GF1: Walk an elaborated expression tree and collect all EX_LET bindings.
 * Used to find yield-live variables for generator struct field promotion. */
static void collect_let_bindings_walk(const Expr *body,
                                      Binding ***out_arr, uint32_t *n_out,
                                      uint32_t *cap_out) {
    if (!body) return;
    const Expr **stk = (const Expr **)malloc(256 * sizeof(const Expr *));
    int sp = 0;
    stk[sp++] = body;
    while (sp > 0) {
        const Expr *cur = stk[--sp];
        if (!cur) continue;
        switch (cur->kind) {
            case EX_LET:
                for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                    Binding *b = cur->as.let_.bindings[i].binding;
                    if (b) {
                        if (*n_out >= *cap_out) {
                            *cap_out = *cap_out ? *cap_out * 2 : 8;
                            *out_arr = (Binding **)realloc(*out_arr, *cap_out * sizeof(Binding *));
                        }
                        (*out_arr)[(*n_out)++] = b;
                        if (cur->as.let_.bindings[i].init)
                            stk[sp++] = cur->as.let_.bindings[i].init;
                    }
                }
                if (cur->as.let_.body) stk[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.cond)         stk[sp++] = cur->as.if_.cond;
                if (cur->as.if_.then_)        stk[sp++] = cur->as.if_.then_;
                if (cur->as.if_.else_or_null) stk[sp++] = cur->as.if_.else_or_null;
                break;
            case EX_DO:
                for (uint32_t i = 0; i < cur->as.do_.n; i++)
                    stk[sp++] = cur->as.do_.items[i];
                break;
            case EX_WHILE:
                stk[sp++] = cur->as.while_.cond;
                stk[sp++] = cur->as.while_.body;
                break;
            case EX_YIELD:
                if (cur->as.yield_.value) stk[sp++] = cur->as.yield_.value;
                break;
            case EX_CALL:
                for (uint32_t i = 0; i < cur->as.call_.n_args; i++)
                    stk[sp++] = cur->as.call_.args[i];
                break;
            case EX_RETURN:
                if (cur->as.return_.value) stk[sp++] = cur->as.return_.value;
                break;
            default:
                break;
        }
    }
    free(stk);
}

/* GF1: (gen [] body...) -- create a generator expression.
 *
 * The form (gen [] body) compiles to a heap-allocated C state-machine struct
 * with a _next function.  All let bindings in the body are promoted to struct
 * fields (conservative yield-live analysis).  Captures (free variables) are
 * passed to the _create function and stored in the struct. */
Expr *elab_gen(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "gen requires a capture vector and at least one body form: (gen [] body)");
        return NULL;
    }
    /* v1: no nested generators */
    if (e->gen_ctx) {
        diag_emit(DIAG_ERROR, call->span,
                  "nested generators are not supported in v1");
        return NULL;
    }

    Form *cap_vec = call->as.list.items[1];
    if (cap_vec->tag != F_VEC) {
        diag_emit(DIAG_ERROR, cap_vec->span,
                  "gen second argument must be an empty capture vector []");
        return NULL;
    }
    if (cap_vec->as.list.len != 0) {
        diag_emit(DIAG_ERROR, cap_vec->span,
                  "gen capture vector must be empty in v1 (captures are computed automatically)");
        return NULL;
    }

    /* Generate unique struct/fn names */
    uint32_t gen_id = e->gen_counter++;
    const char *fn_name_raw = e->current_fn_name ? e->current_fn_name->name : "top";
    /* Mangle fn_name to a valid C identifier: replace hyphens/slashes/dots with '_'. */
    size_t fn_name_len = strlen(fn_name_raw);
    char *fn_name_buf = (char *)alloca(fn_name_len + 1);
    for (size_t _i = 0; _i < fn_name_len; _i++) {
        char _c = fn_name_raw[_i];
        fn_name_buf[_i] = ((_c >= 'a' && _c <= 'z') || (_c >= 'A' && _c <= 'Z') ||
                           (_c >= '0' && _c <= '9') || _c == '_') ? _c : '_';
    }
    fn_name_buf[fn_name_len] = '\0';
    const char *fn_name = fn_name_buf;

    GenDef *def = (GenDef *)arena_alloc(e->arena, sizeof(GenDef));
    memset(def, 0, sizeof(GenDef));
    snprintf(def->struct_name, sizeof(def->struct_name), "__gen_%s_%u_t", fn_name, gen_id);
    snprintf(def->next_fn,     sizeof(def->next_fn),    "__gen_%s_%u_next", fn_name, gen_id);
    snprintf(def->create_fn,   sizeof(def->create_fn),  "__gen_%s_%u_create", fn_name, gen_id);

    /* Push gen context */
    GenContext gctx;
    memset(&gctx, 0, sizeof(gctx));
    gctx.parent = e->gen_ctx;
    e->gen_ctx = &gctx;

    /* Push a new scope for the gen body */
    Scope body_scope;
    scope_init(&body_scope, e->scope);
    e->scope = &body_scope;

    /* Elaborate body forms */
    uint32_t n_body = call->as.list.len - 2;
    Expr *body_expr = NULL;
    if (n_body == 1) {
        body_expr = elab_form(e, call->as.list.items[2]);
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[i + 2]);
            if (!items[i]) {
                e->scope = body_scope.parent;
                e->gen_ctx = gctx.parent;
                return NULL;
            }
        }
        body_expr = expr_new(e->arena, EX_DO, items[n_body-1]->type, call->span);
        body_expr->as.do_.items = items;
        body_expr->as.do_.n     = n_body;
    }
    if (!body_expr) {
        e->scope = body_scope.parent;
        e->gen_ctx = gctx.parent;
        return NULL;
    }

    /* Pop scope and gen context */
    e->scope   = body_scope.parent;
    e->gen_ctx = gctx.parent;

    def->n_yield_points = gctx.n_yields;
    def->element_kind   = gctx.element_kind_set ? gctx.element_kind : TY_INT;
    def->body           = body_expr;

    /* Collect free variables (captures) from the elaborated body */
    uint32_t n_caps = 0;
    Binding **caps = collect_free_vars(body_expr, NULL, 0, &n_caps);
    def->captures   = caps ? (Binding **)arena_alloc(e->arena, n_caps * sizeof(Binding *)) : NULL;
    if (def->captures && caps)
        memcpy(def->captures, caps, n_caps * sizeof(Binding *));
    free(caps);
    def->n_captures = n_caps;

    /* Collect all let bindings for struct field promotion */
    Binding **let_binds  = NULL;
    uint32_t  n_let      = 0;
    uint32_t  cap_let    = 0;
    collect_let_bindings_walk(body_expr, &let_binds, &n_let, &cap_let);

    /* Build struct_bindings = captures ++ let_bindings (de-duplicated) */
    uint32_t total = n_caps + n_let;
    Binding **sb = NULL;
    uint32_t n_sb = 0;
    if (total > 0) {
        sb = (Binding **)arena_alloc(e->arena, total * sizeof(Binding *));
        /* Add captures first */
        for (uint32_t i = 0; i < n_caps; i++) sb[n_sb++] = def->captures[i];
        /* Add let bindings (skip if already in captures) */
        for (uint32_t i = 0; i < n_let; i++) {
            bool dup = false;
            for (uint32_t j = 0; j < n_caps; j++) {
                if (def->captures[j] == let_binds[i]) { dup = true; break; }
            }
            if (!dup) sb[n_sb++] = let_binds[i];
        }
    }
    free(let_binds);
    def->struct_bindings   = sb;
    def->n_struct_bindings = n_sb;

    /* Build result type: TY_GENERATOR */
    Type gen_type;
    memset(&gen_type, 0, sizeof(gen_type));
    gen_type.kind = TY_GENERATOR;
    gen_type.copy_kind = CK_COPY;
    gen_type.hkt_kind  = KIND_STAR;
    gen_type.as.generator_.element_kind = def->element_kind;

    Expr *out = expr_new(e->arena, EX_GEN, gen_type, call->span);
    out->as.gen_.def = def;
    return out;
}

/* GF1: (yield expr) -- yield a value inside a gen body. */
Expr *elab_yield(Elab *e, const Form *call) {
    if (!e->gen_ctx) {
        diag_emit(DIAG_ERROR, call->span,
                  "yield is only valid inside a gen body");
        return NULL;
    }
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "yield requires exactly one argument: (yield expr)");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;

    /* Record element kind on first yield */
    if (!e->gen_ctx->element_kind_set) {
        e->gen_ctx->element_kind     = value->type.kind;
        e->gen_ctx->element_kind_set = true;
    }

    uint32_t yid = ++e->gen_ctx->n_yields;

    Expr *out = expr_new(e->arena, EX_YIELD, TYPE_NIL, call->span);
    out->as.yield_.value    = value;
    out->as.yield_.yield_id = yid;
    return out;
}

/* GF1: (gen-next g) -- advance a generator; returns ptr<void> (some/none). */
Expr *elab_gen_next(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "gen-next requires exactly one argument: (gen-next g)");
        return NULL;
    }
    Expr *gen_expr = elab_form(e, call->as.list.items[1]);
    if (!gen_expr) return NULL;
    if (gen_expr->type.kind != TY_GENERATOR) {
        diag_emit(DIAG_ERROR, gen_expr->span,
                  "gen-next expects a generator value, got %s",
                  type_name(gen_expr->type));
        return NULL;
    }

    Type ptr_type;
    memset(&ptr_type, 0, sizeof(ptr_type));
    ptr_type.kind      = TY_PTR_VOID;
    ptr_type.copy_kind = CK_COPY;
    ptr_type.hkt_kind  = KIND_STAR;

    Expr *out = expr_new(e->arena, EX_GEN_NEXT, ptr_type, call->span);
    out->as.gen_next_.gen_expr = gen_expr;
    out->as.gen_next_.def      = gen_expr->as.gen_.def;
    return out;
}

/* GF1: (gen-done? g) -- true if the generator is exhausted. */
Expr *elab_gen_done(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "gen-done? requires exactly one argument: (gen-done? g)");
        return NULL;
    }
    Expr *gen_expr = elab_form(e, call->as.list.items[1]);
    if (!gen_expr) return NULL;
    if (gen_expr->type.kind != TY_GENERATOR) {
        diag_emit(DIAG_ERROR, gen_expr->span,
                  "gen-done? expects a generator value, got %s",
                  type_name(gen_expr->type));
        return NULL;
    }

    Type bool_type;
    memset(&bool_type, 0, sizeof(bool_type));
    bool_type.kind      = TY_BOOL;
    bool_type.copy_kind = CK_COPY;
    bool_type.hkt_kind  = KIND_STAR;

    Expr *out = expr_new(e->arena, EX_GEN_DONE, bool_type, call->span);
    out->as.gen_done_.gen_expr = gen_expr;
    return out;
}
