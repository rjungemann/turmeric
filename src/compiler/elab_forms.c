/* elab_forms.c -- control-flow and basic expression forms (let/if/do/while/case/...). */
#include "elab_internal.h"

/* ---- file-local helper forward declarations ---- */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form);

/* ---- internal define splicing ---- */

/* splice_internal_defines -- rewrite a body window that may contain
 * (define name init) forms into nested let expressions.
 *
 * Returns NULL when no define is present so callers can keep their existing
 * fast path unchanged (true no-op guarantee -- zero codegen drift).
 *
 * When defines are present, always returns a non-NULL Form* (a let or do)
 * that the caller should elaborate with elab_form.  On parse error, emits a
 * diagnostic and returns form_nil; the caller still calls elab_form on it,
 * which returns a typed nil (preventing double-error from fallthrough).
 *
 * Desugaring:
 *   (define x 1)       =>  (let [x 1] <rest>)
 *   (define ^mut x 1)  =>  (let [^mut x 1] <rest>)
 */
Form *splice_internal_defines(Elab *e, Form **items, uint32_t n, Span span) {
    /* Pre-scan: bail out fast when no defines are present. */
    bool has_define = false;
    for (uint32_t i = 0; i < n; i++) {
        Form *f = items[i];
        if (f->tag == F_LIST && f->as.list.len >= 1) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM && head->as.sym == e->sym_define) {
                has_define = true;
                break;
            }
        }
    }
    if (!has_define) return NULL;

    /* Find the first define and build (let [^ann... name init] <tail>). */
    for (uint32_t i = 0; i < n; i++) {
        Form *f = items[i];
        bool is_define = (f->tag == F_LIST && f->as.list.len >= 1 &&
                          f->as.list.items[0]->tag == F_SYM &&
                          f->as.list.items[0]->as.sym == e->sym_define);
        if (!is_define) continue;

        /* Parse: (define [^ann...] name init) */
        Form **dargs  = f->as.list.items + 1;  /* skip "define" head */
        uint32_t dlen = f->as.list.len - 1;
        if (dlen < 2) {
            diag_emit(DIAG_ERROR, f->span,
                      "define requires (define name init)");
            return form_nil(e->arena, f->span);
        }

        /* Consume annotation symbols before the binding name.
         * Mirrors the annotation loop in elab_let (elab_forms.c). */
        uint32_t ann_end = 0;
        {
            uint32_t j = 0;
            while (j < dlen) {
                Form *cur = dargs[j];
                if (cur->tag != F_SYM) break;
                const Symbol *s = cur->as.sym;
                if (s == e->sym_caret_mut       ||
                    s == e->sym_caret_persistent ||
                    s == e->sym_caret_linear     ||
                    s == e->sym_caret_unique     ||
                    s == e->sym_caret_affine     ||
                    s == e->sym_caret_relevant) {
                    j++;
                } else {
                    break;
                }
            }
            ann_end = j;
        }
        uint32_t name_idx = ann_end;
        if (name_idx >= dlen) {
            diag_emit(DIAG_ERROR, f->span,
                      "define requires (define name init)");
            return form_nil(e->arena, f->span);
        }
        Form *name_form = dargs[name_idx];
        if (name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_form->span,
                      "define: binding name must be a symbol");
            return form_nil(e->arena, f->span);
        }

        uint32_t init_idx = name_idx + 1;
        if (init_idx >= dlen) {
            diag_emit(DIAG_ERROR, f->span,
                      "define requires an initial value");
            return form_nil(e->arena, f->span);
        }
        if (init_idx != dlen - 1) {
            diag_emit(DIAG_ERROR, f->span,
                      "define: expected (define name init); got extra forms");
            return form_nil(e->arena, f->span);
        }

        /* Build the let binding vector: [^ann... name init] */
        uint32_t bvec_len = ann_end + 2; /* annotations + name + init */
        Form **bvec_items = (Form **)arena_alloc(e->arena, bvec_len * sizeof(Form *));
        uint32_t bvi = 0;
        for (uint32_t a = 0; a < ann_end; a++) bvec_items[bvi++] = dargs[a];
        bvec_items[bvi++] = name_form;
        bvec_items[bvi++] = dargs[init_idx];
        Form *bvec = form_vec(e->arena, f->span, bvec_items, bvec_len);

        /* Build body from remaining items, recursively splicing. */
        uint32_t tail_n = n - (i + 1);
        Form   **tail   = items + i + 1;
        Form    *body_form;
        if (tail_n == 0) {
            body_form = form_nil(e->arena, span);
        } else {
            Form *spliced_tail = splice_internal_defines(e, tail, tail_n, span);
            if (spliced_tail) {
                body_form = spliced_tail;
            } else if (tail_n == 1) {
                body_form = tail[0];
            } else {
                Form **do_items = (Form **)arena_alloc(e->arena, (tail_n + 1) * sizeof(Form *));
                do_items[0] = form_sym(e->arena, span, e->sym_do);
                for (uint32_t k = 0; k < tail_n; k++) do_items[k + 1] = tail[k];
                body_form = form_list(e->arena, span, do_items, tail_n + 1);
            }
        }

        /* Return (let [bvec] body_form). */
        Form *let_items[3];
        let_items[0] = form_sym(e->arena, f->span, e->sym_let);
        let_items[1] = bvec;
        let_items[2] = body_form;
        return form_list(e->arena, f->span, let_items, 3);
    }

    /* has_define was true but we processed no defines -- shouldn't happen.
     * Return NULL to take the safe caller path. */
    return NULL;
}

/* elab_define_error -- stub handler for 'define' reached through normal call
 * dispatch, i.e. outside a body sequence.  Always errors. */
Expr *elab_define_error(Elab *e, const Form *call) {
    diag_emit(DIAG_ERROR, call->span,
              "define is only valid as a body form (in defn, fn, let, or do); "
              "use def for top-level bindings");
    return NULL;
}

/* ---- special forms ---- */

Expr *elab_let(Elab *e, const Form *call) {
    /* (let [b1 i1 b2 i2 ...] body...)
     * Named-let: (let name [p1 v1 ...] body...) -- desugar to letrec */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let requires a binding vector");
        return NULL;
    }
    /* Dispatch to named-let when item[1] is a symbol and item[2] is a vector. */
    if (call->as.list.len >= 4 &&
        call->as.list.items[1]->tag == F_SYM &&
        call->as.list.items[2]->tag == F_VEC) {
        return elab_named_let(e, call);
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
        bool is_fat_ann = false;      /* vec-get-typed-fat-closure-readback */
        /* Phase P3 / LT0 / UT0 / UT2 / ST0: Consume binding annotations in any order.
         * Accepted: ^mut, ^persistent, ^linear, ^unique, ^affine, ^relevant, ^fat
         * -- may appear in any combination before the binding name,
         * e.g. [^affine v ...]. */
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
                } else if (cur->as.sym == e->sym_caret_fat) {
                    is_fat_ann = true;
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
        /* Optional type annotation between name and init:
         *   fused:  (let [x :int 5] ...)
         *   spaced: (let [x : int 5] ...)
         * Spaced (F_TYPE_ANN) is unambiguous and always consumed.  Fused
         * (F_KEYWORD) overlaps with keyword *values* (e.g. a macro template
         * `(let [__k ~k] ...)` where `k` is bound to `:foo`), so only consume
         * a bare keyword when it resolves to a known TypeKind.  Anything else
         * is treated as the initializer. */
        const Form *type_ann_form = NULL;
        if (i < bindings_form->as.list.len) {
            Form *maybe_ann = bindings_form->as.list.items[i];
            if (maybe_ann->tag == F_TYPE_ANN) {
                type_ann_form = maybe_ann;
                i++;
            } else if (maybe_ann->tag == F_KEYWORD &&
                       maybe_ann->as.sym != NULL &&
                       typekind_from_symbol(maybe_ann->as.sym->name) != TY_UNKNOWN) {
                type_ann_form = maybe_ann;
                i++;
            }
        }
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

        /* Phase P3: flag ^persistent RHS so map-new can lower to hamt/new */
        bool prev_in_persistent_let = e->in_persistent_let;
        if (is_persistent) e->in_persistent_let = true;
        /* generic-return-type-not-inferred-from-context: when the binding
         * carries a type annotation, push it onto the expected-type channel
         * so a generic `(call ...)` whose `[A]` lives only on the result
         * can bind A to the annotation.  Skip ^fat (it re-types the binding
         * after the fact and the annotation is not a return-position type). */
        Type *prev_expected = e->expected_type;
        Type *let_init_expected = NULL;
        if (type_ann_form && !is_fat_ann) {
            let_init_expected = fn_type_from_form(e, type_ann_form, NULL, NULL, 0);
            if (let_init_expected) e->expected_type = let_init_expected;
        }
        Expr *init = elab_form(e, init_form);
        e->expected_type = prev_expected;
        e->in_persistent_let = prev_in_persistent_let;

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

        /* Verify the optional type annotation against the elaborated init type.
         * For now we compare TypeKind for the common primitive cases (int,
         * float, bool, cstr, nil/void, ptr) -- enough to reject obvious
         * mistakes like `(let [x : int "hello"] ...)`.  Complex types
         * (structs, ADTs, arrows) parse but skip the equality check;
         * downstream typing rules still apply to the init expression. */
        if (type_ann_form) {
            Type *ann_ty = fn_type_from_form(e, type_ann_form, NULL, NULL, 0);
            if (ann_ty) {
                /* bare-fat-param-non-int-result (Phase A4): a declared-typed
                 * binding whose init's tail is a bare-^fat int64 call carries no
                 * recorded result type; infer it from the annotation and re-stamp
                 * the call before the kind-match check below.  See
                 * docs/upcoming/bare-fat-result-type-inference-plan.md. */
                if (kind_is_non_int_register_class(ann_ty->kind) &&
                    retype_bare_fat_tail_calls(init, ann_ty->kind) &&
                    init->type.kind == TY_INT) {
                    init->type = type_from_kind(ann_ty->kind);
                }
                TypeKind ak = ann_ty->kind, ik = init->type.kind;
                bool primitive = (ak == TY_INT || ak == TY_FLOAT ||
                                  ak == TY_BOOL || ak == TY_CSTR ||
                                  ak == TY_NIL || ak == TY_PTR_VOID);
                if (primitive && ak != ik) {
                    diag_emit(DIAG_ERROR, type_ann_form->span,
                        "let binding '%s': type annotation does not match "
                        "initializer (annotated %s, got %s)",
                        name->name,
                        typekind_to_string(ak), typekind_to_string(ik));
                    rc = -1; break;
                }
            }
        }

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
        /* SZ8 projection-size recovery: retain a type-annotation Form so a
         * downstream call passing `b` (or a field projection of it) can recover
         * its static size index.  Prefer an explicit binding annotation; else
         * inherit the initializer call's declared return-type Form (e.g.
         * `a (mk-2)` where `mk-2 : (SizedBuf (Static 2))`). */
        if (type_ann_form) {
            b->decl_type_form = type_ann_form;
        } else if (init && init->kind == EX_CALL && init->as.call_.fn_binding) {
            const Binding *callee = init->as.call_.fn_binding;
            const Type *cft = &callee->type;
            if (callee->closure_fn_binding) cft = &callee->closure_fn_binding->type;
            if (cft && cft->kind == TY_FN && cft->as.fn.result_type_form)
                b->decl_type_form = cft->as.fn.result_type_form;
        }
        /* TY4: borrow-escape at a let binding.  If the init is a borrow of a
         * referent that lives in a deeper (shorter-lived) scope than this
         * binding, the borrow would outlive the value it points to. */
        {
            const Binding *ref = borrow_referent_binding(init);
            if (ref && !ref->is_global && ref->scope_depth > b->scope_depth) {
                diag_emit_with_code(DIAG_ERROR, init->span,
                    TUR_E0105_BORROW_ESCAPES_SCOPE,
                    "`%s` borrows `%s`, which does not live long enough "
                    "(the borrow outlives the value it points to)",
                    name->name, ref->name->name);
                rc = -1;
                break;
            }
        }
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
        /* vec-get-typed-fat-closure-readback: ^fat re-types a :ptr<void> fat-box
         * init (the standard shape produced by a fat-closure-returning helper,
         * e.g. an SF-fold's (chain-loop ...) result) into a directly-callable
         * fat closure.  The fn-type annotation supplies the call signature;
         * is_fat routes (out args) through the fat-dispatch path -- exactly the
         * syntax the "declare it as a fat closure parameter (^fat out :(fn ...))"
         * help text recommends.  Without the annotation, a bare ^fat keeps the
         * init's :ptr<void> type but is still fat-callable. */
        if (is_fat_ann) {
            b->is_fat = true;
            if (type_ann_form) {
                Type *fat_ty = fn_type_from_form(e, type_ann_form, NULL, NULL, 0);
                if (fat_ty && fat_ty->kind == TY_FN) {
                    b->type = *fat_ty;
                }
            }
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
        
        /* Propagate closure metadata through lets so a binding produced by a
         * closure literal or a closure-returning call remains callable with the
         * underlying thunk signature. */
        if (init) {
            /* curried-fn-typed-param: distinguish "init *is* a closure value"
             * from "init names a *function* that returns a closure".  When the
             * init is an EX_VAR naming a plain function (TY_FN, no
             * closure_fn_binding of its own) that merely *returns* a closure,
             * the alias is still that function -- propagate
             * returns_closure_fn_binding, NOT closure_fn_binding.  Marking the
             * alias as a closure value would make elab_call_fn swap in the
             * inner thunk's type and subtract a hidden env param from its
             * arity, so (f 1) on a curried (fn [int] (fn [int] int)) would
             * wrongly resolve to the inner result kind instead of (fn [int]
             * int). */
            if (init->kind == EX_VAR && init->as.var.binding &&
                init->as.var.binding->type.kind == TY_FN &&
                !init->as.var.binding->closure_fn_binding &&
                init->as.var.binding->returns_closure_fn_binding) {
                b->returns_closure_fn_binding =
                    init->as.var.binding->returns_closure_fn_binding;
            } else {
                Binding *closure_b = expr_closure_fn_binding(init);
                if (closure_b) {
                    /* let-bound SF (let-bound-sf-loses-outer-arg-type): the same
                     * "is a closure value" vs "returns a closure" distinction the
                     * EX_VAR branch above makes also applies to a *call* init.
                     * When init is a call to a function whose return *value* is a
                     * thin (non-boxed) function pointer that itself returns a
                     * closure -- e.g. (make-sf) returning the outer
                     * (fn [sig] (fn [t] ...)) lambda -- closure_b describes what
                     * that thin fn *returns* when called, not what the call result
                     * *is*.  Recording it as closure_fn_binding would make
                     * elab_call_fn swap in the inner env+arg thunk type for `b`,
                     * dropping b's real first parameter (the outer `sig`) and
                     * reading the inner arg in its place.  Route it to
                     * returns_closure_fn_binding instead so a chained call
                     * (sf input) still sees its result as a closure while `b`
                     * keeps its declared outer signature.  A call whose callee
                     * returns a genuine fat closure *box* (e.g. (adder 10), where
                     * adder's body is a capturing lambda) still flows to
                     * closure_fn_binding -- the call result IS the closure value. */
                    if (init->kind == EX_CALL && init->as.call_.fn_binding &&
                        !init->as.call_.fn_binding->returns_boxed_closure) {
                        b->returns_closure_fn_binding = closure_b;
                    } else {
                        b->closure_fn_binding = closure_b;
                    }
                }
            }
        }
        /* Phase HRT4: propagate poly fn metadata through let-bindings.
         * (let [g f] ...) where f is is_poly_fn → g inherits is_poly_fn.
         * (let [g id] ...) where id is a global TY_FN → g.source_binding = id. */
        if (init && init->kind == EX_VAR) {
            Binding *init_b = init->as.var.binding;
            /* closure-representation-unification (Phase 0): propagate the ^fat
             * marker through a let alias so (let [gv g] ... (gv x)) on a
             * fn-typed ^fat parameter still fat-dispatches instead of taking the
             * thin ER2 path (which casts the fat box to a bare fn pointer and
             * crashes).  A bare ^fat alias is :ptr<void> and fat-dispatches
             * regardless, but carrying is_fat keeps the two forms consistent. */
            if (init_b->is_fat) {
                b->is_fat = true;
            }
            if (init_b->is_poly_fn) {
                b->is_poly_fn = true;
                b->poly_type  = init_b->poly_type;
            } else if (init_b->type.kind == TY_FN) {
                /* Follow any existing source chain to the root global fn. */
                Binding *root = init_b->source_binding ? init_b->source_binding : init_b;
                /* pr-386 regression fix (docs/reported/pr-386-source-binding-
                 * alias-breaks-closure-and-with-resource.md): never chain to a
                 * lifted-lambda __fn_N helper.  source_binding means "the user
                 * typed a global function name as the init"; a captureless
                 * closure-returning lambda is callable only through the
                 * closure-dispatch protocol on this let binding, and chaining
                 * to __fn_N makes (f x) emit a direct call whose result is the
                 * int64 carrier rather than a function pointer. */
                if (root->is_global && !root->is_lifted_lambda) b->source_binding = root;
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
        } else {
            /* Internal defines in let body: splice into nested let forms. */
            Form *spliced = splice_internal_defines(e,
                                call->as.list.items + 2, body_count, call->span);
            if (spliced) {
                body = elab_form(e, spliced);
                if (!body) rc = -1;
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
        }

        /* EXG4-1/EXG4-4: ownership transfer through let-tail position.
         *
         * If the body's tail expression is a direct EX_VAR of one of this
         * let's own bindings, the let's result IS that binding's value.
         * Whoever consumes the let's result (an outer let init, a function
         * return, a call arg, etc.) takes ownership.  Mark the binding as
         * moved so the auto-drop pass below skips it -- the consumer is
         * responsible for the eventual drop.
         *
         * Without this, the inner let auto-drops the rc-block before its
         * value escapes, producing use-after-free at the consumer site
         * (e.g. `(let [outer (let [e (pack ...)] e)] ...)`).  Only fires
         * for move-typed bindings; copy types are unaffected. */
        if (rc == 0 && body && n_binds > 0) {
            Expr *cur = body;
            while (cur) {
                if (cur->kind == EX_VAR) {
                    Binding *bv = cur->as.var.binding;
                    for (uint32_t k = 0; k < n_binds; k++) {
                        if (binds[k].binding == bv &&
                            type_is_move(bv->type) &&
                            !bv->is_moved) {
                            binding_mark_moved(bv, cur->span);
                            break;
                        }
                    }
                    break;
                } else if (cur->kind == EX_DO) {
                    if (cur->as.do_.n == 0) break;
                    cur = cur->as.do_.items[cur->as.do_.n - 1];
                } else if (cur->kind == EX_LET) {
                    cur = cur->as.let_.body;
                } else {
                    break;
                }
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
        /* EXG6: linear existentials use the plain malloc emit path and
         * are freed at the open site, so they do NOT participate in the
         * rc-based scope-exit auto-drop. */
        bool is_rc_managed = bt.kind == TY_RC ||
            (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
             && !bt.as.forall_.is_linear);
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
                (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
                 && !bt.as.forall_.is_linear);
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
                    (bt.kind == TY_EXISTS && bt.as.forall_.n_constraints > 0
                     && !bt.as.forall_.is_linear);
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
    LetBinding *bcopy = NULL;
    if (n_binds > 0) {
        bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
        memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    }
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

    /* Internal defines: splice (define name init) into nested let forms. */
    {
        Form *spliced = splice_internal_defines(e, call->as.list.items + 1, n, call->span);
        if (spliced) return elab_form(e, spliced);
    }

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

/* elab_letstar -- (let* [b1 i1 b2 i2 ...] body...)
 *
 * Sequential-binding let: each binding sees the bindings that precede it.
 * Desugars to a right-nested chain of single-binding `let` forms and delegates
 * to elab_let, so all of let's machinery (annotations, vector destructuring,
 * move/alias tracking, typed inits) carries over unchanged.
 *
 *   (let* [a 1 b (+ a 1)] body)
 *     => (let [a 1] (let [b (+ a 1)] body))
 *
 * A single binding "unit" is: zero or more `^`-prefixed annotation symbols,
 * followed by a name symbol OR a vector destructuring pattern, followed by one
 * initializer form.  This mirrors how elab_let parses each entry.
 */
Expr *elab_letstar(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let* requires a binding vector");
        return NULL;
    }
    Form *bindings = call->as.list.items[1];
    if (bindings->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bindings->span,
                  "let* bindings must be a vector [name init ...]");
        return NULL;
    }
    Span     sp     = call->span;
    uint32_t n_body = call->as.list.len - 2;
    Form   **body   = call->as.list.items + 2;
    uint32_t blen   = bindings->as.list.len;

    /* Split the binding vector into units, recording (start,len) ranges. */
    typedef struct { uint32_t start; uint32_t len; } LsUnit;
    LsUnit  *units   = (blen == 0) ? NULL
                       : (LsUnit *)arena_alloc(e->arena, blen * sizeof(LsUnit));
    uint32_t n_units = 0;
    uint32_t i = 0;
    while (i < blen) {
        uint32_t start = i;
        /* Consume leading `^`-prefixed annotation symbols (^mut, ^linear, ...). */
        while (i < blen &&
               bindings->as.list.items[i]->tag == F_SYM &&
               bindings->as.list.items[i]->as.sym->len > 0 &&
               bindings->as.list.items[i]->as.sym->name[0] == '^') {
            i++;
        }
        if (i >= blen) {
            diag_emit(DIAG_ERROR, bindings->as.list.items[start]->span,
                      "let* trailing annotation with no binding name");
            return NULL;
        }
        i++; /* the name symbol or destructuring pattern */
        /* Optional type annotation between name and init (mirrors elab_let).
         * F_TYPE_ANN is unambiguous; F_KEYWORD only consumed if it names a
         * known type (otherwise it's a keyword value, e.g. macro-expanded). */
        if (i < blen) {
            Form *maybe_ann = bindings->as.list.items[i];
            if (maybe_ann->tag == F_TYPE_ANN) {
                i++;
            } else if (maybe_ann->tag == F_KEYWORD &&
                       maybe_ann->as.sym != NULL &&
                       typekind_from_symbol(maybe_ann->as.sym->name) != TY_UNKNOWN) {
                i++;
            }
        }
        if (i >= blen) {
            diag_emit(DIAG_ERROR, bindings->as.list.items[i - 1]->span,
                      "let* binding is missing its initializer");
            return NULL;
        }
        i++; /* the initializer form */
        units[n_units].start = start;
        units[n_units].len   = i - start;
        n_units++;
    }

    Form *sym_let_f = form_sym(e->arena, sp, e->sym_let);

    /* No bindings: (let* [] body...) == (let [] body...). */
    if (n_units == 0) {
        Form  *empty_bv  = form_vec(e->arena, sp, NULL, 0);
        uint32_t len     = 2 + n_body;
        Form **items     = (Form **)arena_alloc(e->arena, len * sizeof(Form *));
        items[0] = sym_let_f;
        items[1] = empty_bv;
        for (uint32_t k = 0; k < n_body; k++) items[2 + k] = body[k];
        return elab_let(e, form_list(e->arena, sp, items, len));
    }

    /* Build the nested let chain from the innermost unit outward. */
    Form *inner = NULL;
    for (int u = (int)n_units - 1; u >= 0; u--) {
        uint32_t ustart = units[u].start;
        uint32_t ulen   = units[u].len;
        Form **bv_items = (Form **)arena_alloc(e->arena, ulen * sizeof(Form *));
        for (uint32_t k = 0; k < ulen; k++) {
            bv_items[k] = bindings->as.list.items[ustart + k];
        }
        Form *bv = form_vec(e->arena, sp, bv_items, ulen);

        uint32_t  let_len;
        Form    **let_items;
        if (u == (int)n_units - 1) {
            /* Innermost let carries the original body forms. */
            let_len   = 2 + n_body;
            let_items = (Form **)arena_alloc(e->arena, let_len * sizeof(Form *));
            for (uint32_t k = 0; k < n_body; k++) let_items[2 + k] = body[k];
        } else {
            /* Outer lets wrap the next-inner let as their single body form. */
            let_len      = 3;
            let_items    = (Form **)arena_alloc(e->arena, let_len * sizeof(Form *));
            let_items[2] = inner;
        }
        let_items[0] = sym_let_f;
        let_items[1] = bv;
        inner = form_list(e->arena, sp, let_items, let_len);
    }
    return elab_let(e, inner);
}

/* ---- letrec and named let ---- */

/* elab_letrec -- (letrec [f1 i1 f2 i2 ...] body...)
 *
 * Two-pass binding: all names are pre-registered in the inner scope before
 * any initializer is elaborated, enabling self-recursion and mutual recursion
 * between fn-valued bindings.  Non-fn inits that reference their own name will
 * see the TY_UNKNOWN placeholder and type-check against it, producing a
 * type-mismatch error rather than silent undefined behaviour.
 *
 * Restrictions in v1:
 *   - Binding annotations (^linear, ^unique, ^affine, ^relevant, ^mut) are
 *     rejected.  Substructural recursive locals can be top-level defns.
 *   - Vector destructuring is not supported.
 */
Expr *elab_letrec(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "letrec requires a binding vector and a body: (letrec [name init ...] body...)");
        return NULL;
    }
    Form *bvec = call->as.list.items[1];
    if (bvec->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bvec->span,
                  "letrec bindings must be a vector [name init ...]");
        return NULL;
    }
    uint32_t blen = bvec->as.list.len;
    if (blen % 2 != 0) {
        diag_emit(DIAG_ERROR, bvec->span,
                  "letrec binding vector must have an even number of forms (name init pairs)");
        return NULL;
    }
    uint32_t n_entries = blen / 2;

    /* Temporary per-binding metadata, freed before return. */
    typedef struct { const Symbol *name; Span span; Form *init_form; } LrEntry;
    LrEntry *entries = (LrEntry *)malloc(n_entries * sizeof(LrEntry));
    if (!entries) { fprintf(stderr, "tur: oom\n"); abort(); }
    Binding **pre_b = (Binding **)malloc(n_entries * sizeof(Binding *));
    if (!pre_b) { fprintf(stderr, "tur: oom\n"); abort(); }

    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    int rc = 0;

    /* Parse binding vector: reject annotations, collect name/init pairs. */
    for (uint32_t k = 0; k < n_entries && rc == 0; k++) {
        Form *name_f = bvec->as.list.items[k * 2];
        Form *init_f = bvec->as.list.items[k * 2 + 1];
        /* Reject substructural / mutability annotations that make no sense in
         * a pre-registered binding group. */
        if (name_f->tag == F_SYM && (
            name_f->as.sym == e->sym_caret_mut       ||
            name_f->as.sym == e->sym_caret_linear    ||
            name_f->as.sym == e->sym_caret_unique    ||
            name_f->as.sym == e->sym_caret_affine    ||
            name_f->as.sym == e->sym_caret_relevant  ||
            name_f->as.sym == e->sym_caret_persistent)) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "letrec does not support binding annotations ('^%s'); "
                      "use a top-level defn for annotated recursive bindings",
                      name_f->as.sym->name);
            rc = -1; break;
        }
        if (name_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "letrec binding name must be a symbol");
            rc = -1; break;
        }
        /* Duplicate name check. */
        for (uint32_t j = 0; j < k; j++) {
            if (entries[j].name == name_f->as.sym) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "letrec: '%s' is bound twice in the same binding group",
                          name_f->as.sym->name);
                rc = -1; break;
            }
        }
        if (rc != 0) break;
        entries[k].name      = name_f->as.sym;
        entries[k].span      = name_f->span;
        entries[k].init_form = init_f;
    }

    /* Pass A -- pre-register all names with placeholder types.
     * If the init is a (fn ...) literal, peek at arity and return type to build
     * a TY_FN stub so callers see the right arity during init elaboration.
     * Otherwise fall back to TY_UNKNOWN. */
    for (uint32_t k = 0; k < n_entries && rc == 0; k++) {
        Form *init_f = entries[k].init_form;
        Type placeholder = TYPE_UNKNOWN;
        if (init_f->tag == F_LIST && init_f->as.list.len >= 3) {
            Form *ih = init_f->as.list.items[0];
            if (ih->tag == F_SYM &&
                (ih->as.sym == e->sym_fn || ih->as.sym == e->sym_lambda)) {
                Form *params_f = init_f->as.list.items[1];
                if (params_f->tag == F_VEC) {
                    /* Same `^`-marker-aware scan as the defmodule/top-level
                     * forward-decl pre-pass: a `(fn [^fat g ...] ...)` literal
                     * must not count its ^fat marker as a parameter slot. */
                    TypeKind arg_kinds[MAX_FN_ARITY];
                    uint32_t arity = fwd_decl_scan_params(params_f, arg_kinds);
                    /* Peek at the return-type keyword at index 2 (fn [params] :ret body). */
                    TypeKind ret_kind = TY_INT;
                    if (init_f->as.list.len >= 4) {
                        Form *ret_f = init_f->as.list.items[2];
                        /* Accept spaced `: T` (F_TYPE_ANN{F_SYM/F_KEYWORD}) too. */
                        if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                            (ret_f->as.list.items[0]->tag == F_SYM ||
                             ret_f->as.list.items[0]->tag == F_KEYWORD)) {
                            ret_f = ret_f->as.list.items[0];
                        }
                        if (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM) {
                            const char *rn = ret_f->as.sym->name;
                            uint32_t   rl = ret_f->as.sym->len;
                            if      (rl == 3 && memcmp(rn, "int",  3) == 0) ret_kind = TY_INT;
                            else if (rl == 4 && memcmp(rn, "bool", 4) == 0) ret_kind = TY_BOOL;
                            else if (rl == 4 && memcmp(rn, "void", 4) == 0) ret_kind = TY_NIL;
                            else if (rl == 3 && memcmp(rn, "nil",  3) == 0) ret_kind = TY_NIL;
                            else if (rl == 4 && memcmp(rn, "cstr", 4) == 0) ret_kind = TY_CSTR;
                        }
                    }
                    placeholder = type_fn(arg_kinds, (uint8_t)arity, ret_kind);
                }
            }
        }
        Binding *b = binding_new(e, entries[k].name, placeholder,
                                 false, false, entries[k].span);
        scope_add(&inner, b);
        pre_b[k] = b;
    }

    /* Pass B -- elaborate each init inside the inner scope, then patch the
     * pre-registered binding's type with the actual elaborated type. */
    LetBinding *binds = NULL;
    uint32_t n_binds = 0;
    if (rc == 0) {
        binds = (LetBinding *)malloc(n_entries * sizeof(LetBinding));
        if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    for (uint32_t k = 0; k < n_entries && rc == 0; k++) {
        Expr *init = elab_form(e, entries[k].init_form);
        if (!init) { rc = -1; break; }
        /* Patch the pre-registered binding with the real type. */
        pre_b[k]->type = init->type;
        /* For fn-valued bindings, mark as global so the emitter uses direct
         * calls (C function name) rather than cast-and-call through a local
         * pointer -- enabling self-recursion and mutual recursion. */
        /* For no-capture fn bindings (EX_VAR pointing to a file-scope static fn),
         * mark as global so callers emit direct C function calls, enabling
         * self-recursion and mutual recursion without closure overhead. */
        if (init->kind == EX_VAR && init->type.kind == TY_FN) {
            Binding *fn_b = init->as.var.binding;
            char *fn_c_name = elab_mangle_binding_name(fn_b);
            pre_b[k]->is_global = true;
            pre_b[k]->c_export_name = arena_strdup(e->arena, fn_c_name, strlen(fn_c_name));
            free(fn_c_name);
        }
        /* Propagate closure metadata (mirrors elab_let). */
        Binding *cl = expr_closure_fn_binding(init);
        if (cl) pre_b[k]->closure_fn_binding = cl;
        /* Phase HRT4: poly fn metadata propagation. */
        if (init->kind == EX_VAR) {
            Binding *ib = init->as.var.binding;
            if (ib->is_poly_fn) {
                pre_b[k]->is_poly_fn = true;
                pre_b[k]->poly_type  = ib->poly_type;
            } else if (ib->type.kind == TY_FN) {
                Binding *root = ib->source_binding ? ib->source_binding : ib;
                if (root->is_global) pre_b[k]->source_binding = root;
            }
        }
        binds[n_binds].binding = pre_b[k];
        binds[n_binds].init    = init;
        n_binds++;
    }

    /* Pass C -- elaborate body. */
    Expr *body = NULL;
    if (rc == 0) {
        uint32_t body_count = call->as.list.len - 2;
        if (body_count == 0) {
            body = e_nil(e, call->span);
        } else {
            Form *spliced = splice_internal_defines(e,
                                call->as.list.items + 2, body_count, call->span);
            if (spliced) {
                body = elab_form(e, spliced);
                if (!body) rc = -1;
            } else if (body_count == 1) {
                body = elab_form(e, call->as.list.items[2]);
                if (!body) rc = -1;
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, body_count * sizeof(Expr *));
                for (uint32_t k = 0; k < body_count && rc == 0; k++) {
                    items[k] = elab_form(e, call->as.list.items[2 + k]);
                    if (!items[k]) rc = -1;
                }
                if (rc == 0) {
                    body = expr_new(e->arena, EX_DO, items[body_count-1]->type, call->span);
                    body->as.do_.items = items;
                    body->as.do_.n = body_count;
                }
            }
        }
    }

    e->scope = inner.parent;
    scope_free(&inner);
    free(entries);
    free(pre_b);

    if (rc != 0) {
        free(binds);
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_LETREC, body->type, call->span);
    LetBinding *bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
    memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    free(binds);
    out->as.let_.bindings = bcopy;
    out->as.let_.n        = n_binds;
    out->as.let_.body     = body;
    return out;
}

/* elab_named_let -- (let name [p1 v1 p2 v2 ...] body...)
 *
 * Desugars to:
 *   (letrec [name (fn [p1 p2 ...] body...)]
 *     (name v1 v2 ...))
 *
 * Type annotations carry through verbatim: [n :int 10 ...] becomes (fn [n :int] ...).
 */
Expr *elab_named_let(Elab *e, const Form *call) {
    /* call: (let <sym> [p1 v1 ...] body...) */
    Form *loop_name_f = call->as.list.items[1]; /* F_SYM checked by caller */
    const Symbol *loop_sym = loop_name_f->as.sym;
    Form *bvec = call->as.list.items[2]; /* F_VEC checked by caller */
    Span sp = call->span;
    Arena *a = e->arena;

    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, sp,
                  "named let requires a body: (let %s [...] body...)", loop_sym->name);
        return NULL;
    }

    /* Walk the binding vector, separating param specs from initial values.
     * Format: [name1 [:type1] init1  name2 [:type2] init2 ...]
     * fn_param_items: the items that go inside (fn [...] ...)
     * init_items:     the initial call arguments for (name v1 v2 ...) */
    uint32_t blen = bvec->as.list.len;
    Form **fn_param_items = (Form **)arena_alloc(a, blen * sizeof(Form *));
    Form **init_items     = (Form **)arena_alloc(a, blen * sizeof(Form *));
    uint32_t n_params = 0, n_inits = 0;

    uint32_t bi = 0;
    while (bi < blen) {
        Form *cur = bvec->as.list.items[bi];
        if (cur->tag != F_SYM) {
            diag_emit(DIAG_ERROR, cur->span,
                      "named let: binding name must be a symbol");
            return NULL;
        }
        fn_param_items[n_params++] = cur;  /* the param name */
        bi++;
        /* Optional :type annotation -- accept both fused `:type` (F_KEYWORD)
         * and spaced `: type` (F_TYPE_ANN); fn elaboration handles both. */
        if (bi < blen &&
            (bvec->as.list.items[bi]->tag == F_KEYWORD ||
             bvec->as.list.items[bi]->tag == F_TYPE_ANN)) {
            fn_param_items[n_params++] = bvec->as.list.items[bi++];
        }
        /* Mandatory initializer */
        if (bi >= blen) {
            diag_emit(DIAG_ERROR, cur->span,
                      "named let: missing initializer for parameter '%s'", cur->as.sym->name);
            return NULL;
        }
        init_items[n_inits++] = bvec->as.list.items[bi++];
    }

    /* Build (fn [fn_param_items...] body...) */
    uint32_t body_count = call->as.list.len - 3; /* items after the bvec */
    uint32_t fn_len = 2 + body_count;             /* fn + params_vec + body... */
    Form **fn_items_arr = (Form **)arena_alloc(a, fn_len * sizeof(Form *));
    fn_items_arr[0] = form_sym(a, sp, e->sym_fn);
    fn_items_arr[1] = form_vec(a, sp, fn_param_items, n_params);
    for (uint32_t i = 0; i < body_count; i++)
        fn_items_arr[2 + i] = call->as.list.items[3 + i];
    Form *fn_form = form_list(a, sp, fn_items_arr, fn_len);

    /* Build letrec binding vector [loop_sym fn_form] */
    Form *lr_bvec_arr[2];
    lr_bvec_arr[0] = form_sym(a, sp, loop_sym);
    lr_bvec_arr[1] = fn_form;
    Form *lr_bvec = form_vec(a, sp, lr_bvec_arr, 2);

    /* Build call form (loop_sym init_items...) */
    uint32_t call_len = 1 + n_inits;
    Form **call_items_arr = (Form **)arena_alloc(a, call_len * sizeof(Form *));
    call_items_arr[0] = form_sym(a, sp, loop_sym);
    for (uint32_t i = 0; i < n_inits; i++) call_items_arr[1 + i] = init_items[i];
    Form *call_form = form_list(a, sp, call_items_arr, call_len);

    /* Build (letrec lr_bvec call_form) */
    Form *lr_arr[3];
    lr_arr[0] = form_sym(a, sp, e->sym_letrec);
    lr_arr[1] = lr_bvec;
    lr_arr[2] = call_form;
    Form *letrec_form = form_list(a, sp, lr_arr, 3);

    return elab_form(e, letrec_form);
}

/* TY3: recognize a flow-narrowing guard in an `if` condition Form.
 *
 * Two supported shapes, both yielding (variable symbol, type-name symbol):
 *   (is? x T)              -- the dedicated type-test predicate
 *   (= (type-of x) "T")    -- type-of compared against a string literal
 *
 * On a match, *out_var receives x's Symbol and *out_type receives T's Symbol
 * (interned from the string literal in the type-of shape), and returns true.
 * Only direct, single-variable tests narrow; negation/conjunction do not (see
 * TY3.3).  Recognition is purely syntactic on the un-elaborated Form. */
static bool if_guard_narrowing(Elab *e, const Form *cond,
                               const Symbol **out_var, const Symbol **out_type) {
    if (!cond || cond->tag != F_LIST || cond->as.list.len < 1) return false;
    Form *head = cond->as.list.items[0];
    if (head->tag != F_SYM) return false;

    /* Shape 1: (is? x T) */
    if (head->as.sym == e->sym_is_q && cond->as.list.len == 3) {
        Form *xf = cond->as.list.items[1];
        Form *tf = cond->as.list.items[2];
        if (xf->tag == F_SYM && tf->tag == F_SYM) {
            *out_var  = xf->as.sym;
            *out_type = tf->as.sym;
            return true;
        }
        return false;
    }

    /* Shape 2: (= (type-of x) "T") */
    const Symbol *sym_eq = symtab_intern(e->st, strslice("=", 1));
    if (head->as.sym == sym_eq && cond->as.list.len == 3) {
        Form *lhs = cond->as.list.items[1];
        Form *rhs = cond->as.list.items[2];
        /* lhs must be (type-of x) with x a bare symbol */
        if (lhs->tag != F_LIST || lhs->as.list.len != 2) return false;
        Form *lhead = lhs->as.list.items[0];
        Form *xf    = lhs->as.list.items[1];
        if (lhead->tag != F_SYM || lhead->as.sym != e->sym_type_of) return false;
        if (xf->tag != F_SYM) return false;
        /* rhs must be a string literal naming the type */
        if (rhs->tag != F_STR) return false;
        *out_var  = xf->as.sym;
        *out_type = symtab_intern(e->st, rhs->as.s);
        return true;
    }

    return false;
}

/* TY3: wrap a branch Form so the narrowed variable is rebound to the unboxed
 * value: <branch>  =>  (let [x (cast x T)] <branch>).  Reuses the TY2 checked
 * cast, so a use of x at type T inside the branch type-checks and the runtime
 * tag is verified.  Returns the original branch if any piece cannot be built. */
static Form *if_narrow_branch(Elab *e, Form *branch,
                              const Symbol *var, const Symbol *type_sym, Span sp) {
    Arena *a = e->arena;
    /* (cast x T) */
    Form *cast_items[3];
    cast_items[0] = form_sym(a, sp, e->sym_cast);
    cast_items[1] = form_sym(a, sp, var);
    cast_items[2] = form_sym(a, sp, type_sym);
    Form *cast_f = form_list(a, sp, cast_items, 3);
    /* binding vector [x (cast x T)] */
    Form *bvec_items[2] = { form_sym(a, sp, var), cast_f };
    Form *bvec = form_vec(a, sp, bvec_items, 2);
    /* (let [x (cast x T)] branch) */
    Form *let_items[3] = { form_sym(a, sp, e->sym_let), bvec, branch };
    return form_list(a, sp, let_items, 3);
}

/* vec-get-typed-fat-closure-readback: is this expression a fat-closure value --
 * i.e. a directly-callable ^fat fn binding, or a boxed TY_FN closure value?
 * Such a value is representationally a void* fat box, so it unifies with a
 * :ptr<void> branch in an `if`.  A thin (unboxed) bare fn pointer is NOT a fat
 * box, so it is excluded to preserve the raw-pointer-vs-closure split. */
static bool expr_is_fat_closure_value(const Expr *x) {
    if (!x) return false;
    if (x->kind == EX_VAR && x->as.var.binding && x->as.var.binding->is_fat)
        return true;
    if (x->type.kind == TY_FN && x->type.as.fn.boxed)
        return true;
    return false;
}

/* M2b: if-branch tyvar tolerance.
 *
 * The strict `type_eq` used for if-branch parity treats `(Option A)` (A bare
 * tyvar) and `(Option int)` as distinct.  After M2b polymorphized `(none)` /
 * `(some x)` over A, the natural pattern
 *
 *   (if cond (none) (some x))
 *
 * fails: then-branch is `(Option A)`, else-branch is `(Option int)`, and they
 * don't unify even though A := int trivially makes them agree.
 *
 * Walk both types structurally; whenever one side carries a bare TY_TYVAR
 * where the other side has a concrete kind, accept it.  When both sides
 * agree everywhere else, pick the more-concrete side as the if result
 * (so downstream callers see `Option int`, not `Option A`).  Returns false
 * if the types disagree in any non-tyvar position.
 *
 * Scope: only used inside if-branch comparison; the rest of the elaborator
 * keeps strict `type_eq` semantics.  Mirrors the spec-resolver pattern used
 * in elab_make_struct for unbound result tyvars. */
static bool type_eq_tyvar_tolerant(Type a, Type b, Type *out_concrete) {
    if (a.kind == TY_TYVAR && b.kind != TY_TYVAR) {
        if (out_concrete) *out_concrete = b;
        return true;
    }
    if (b.kind == TY_TYVAR && a.kind != TY_TYVAR) {
        if (out_concrete) *out_concrete = a;
        return true;
    }
    if (a.kind != b.kind) return false;
    if (a.kind == TY_APP) {
        if (!a.as.app.fn || !b.as.app.fn || !a.as.app.arg || !b.as.app.arg)
            return false;
        Type fn_concrete = *a.as.app.fn;
        Type arg_concrete = *a.as.app.arg;
        if (!type_eq_tyvar_tolerant(*a.as.app.fn, *b.as.app.fn, &fn_concrete))
            return false;
        if (!type_eq_tyvar_tolerant(*a.as.app.arg, *b.as.app.arg, &arg_concrete))
            return false;
        if (out_concrete) *out_concrete = a;  /* shape preserved, args refined */
        /* Rebuild a fresh TY_APP with the concrete sub-types woven in. */
        if (out_concrete) {
            Type out = a;
            /* Best-effort: drop the recovered args back through static buffers
             * the same way type_app does at elab time would be cleaner, but
             * for the if-result we just preserve `a`'s shape — the use site
             * only consults the result type's outermost kind for ABI
             * decisions, and the inner tyvar gets resolved per call site
             * downstream through ordinary spec resolution. */
            out.as.app.fn = a.as.app.fn;
            out.as.app.arg = a.as.app.arg;
            (void)fn_concrete;
            (void)arg_concrete;
            *out_concrete = out;
        }
        return true;
    }
    if (a.kind == TY_TYVAR) {
        /* Both sides are tyvars; accept (existing type_eq already does). */
        if (out_concrete) *out_concrete = a;
        return true;
    }
    return type_eq(a, b);
}

static bool if_branches_unify_via_tyvar(Type then_ty, Type else_ty, Type *out) {
    Type result = then_ty;
    if (!type_eq_tyvar_tolerant(then_ty, else_ty, &result)) return false;
    /* Prefer the side with no bare tyvars at the outermost position. */
    if (then_ty.kind == TY_TYVAR && else_ty.kind != TY_TYVAR) result = else_ty;
    else if (else_ty.kind == TY_TYVAR && then_ty.kind != TY_TYVAR) result = then_ty;
    else result = then_ty;
    if (out) *out = result;
    return true;
}

Expr *elab_if(Elab *e, const Form *call) {
    if (call->as.list.len != 3 && call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "if expects (if cond then) or (if cond then else); got %u argument(s)",
                  call->as.list.len - 1);
        return NULL;
    }

    /* TY3: flow-sensitive narrowing.  Detect a type-test guard on the raw
     * condition Form *before* elaborating it.  Two effects:
     *   1. The `(= (type-of x) "T")` shape is rewritten to `(is? x T)` so the
     *      condition elaborates to a tag comparison (plain `=` has no cstr
     *      overload).  The `(is? x T)` shape is already in that form.
     *   2. When x is `any`-typed, the then-branch is wrapped in
     *      (let [x (cast x T)] ...) so a use of x at type T type-checks
     *      without an explicit cast; the TY2 checked cast verifies the runtime
     *      tag on entry to the branch.
     * (TY3.3: only the direct then-branch on an `any` variable narrows; the
     * else-complement is left to a future phase -- the `any` complement is not
     * a single type, and union variables already narrow via `match`.) */
    Form *cond_form = call->as.list.items[1];
    Form *then_form = call->as.list.items[2];
    Form *else_form = (call->as.list.len == 4) ? call->as.list.items[3] : NULL;
    {
        const Symbol *gv = NULL, *gt = NULL;
        if (if_guard_narrowing(e, cond_form, &gv, &gt)) {
            /* Rewrite the condition to the canonical (is? x T) test form. */
            Form *is_items[3] = { form_sym(e->arena, call->span, e->sym_is_q),
                                  form_sym(e->arena, call->span, gv),
                                  form_sym(e->arena, call->span, gt) };
            cond_form = form_list(e->arena, call->span, is_items, 3);
            Binding *vb = scope_lookup(e->scope, gv);
            if (vb && vb->type.kind == TY_ANY) {
                then_form = if_narrow_branch(e, then_form, gv, gt, call->span);
            }
        }
    }

    Expr *cond = elab_form(e, cond_form);
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

    Expr *then_ = elab_form(e, then_form);
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
        else_ = elab_form(e, else_form);
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
        } else if (then_->type.kind == TY_ANY || else_->type.kind == TY_ANY) {
            /* TY2.2: branch widening to `any`.  When one branch is `any`, box
             * the other (a narrower subtype) so both arms share the tagged
             * representation and the if yields `any`. */
            then_ = elab_coerce_to_any(e, then_);
            else_ = elab_coerce_to_any(e, else_);
            result_t = then_->type;
        } else if ((expr_is_fat_closure_value(then_) && else_->type.kind == TY_PTR_VOID) ||
                   (expr_is_fat_closure_value(else_) && then_->type.kind == TY_PTR_VOID)) {
            /* vec-get-typed-fat-closure-readback: a fat-closure value (a directly
             * callable ^fat fn binding, representationally a void* box) and a
             * :ptr<void> are the same representation.  This is the natural shape
             * of an SF-fold base case: `(if done sig (loop ... (apply ...)))`
             * where `sig` is a ^fat parameter (TY_FN) and the recursive arm
             * returns the threaded :ptr<void> fat box.  Widen both arms to
             * :ptr<void> so the if elaborates instead of reporting a spurious
             * "then=(fn ...) else=ptr<void>" mismatch; the caller re-types the
             * result with `^fat` (or `::`) to call it again.  Calling a raw
             * :ptr<void> directly stays an error (CRU B-4). */
            result_t = TYPE_PTR_VOID;
        } else if (!type_eq(then_->type, else_->type)
                   && !if_branches_unify_via_tyvar(then_->type, else_->type, &result_t)) {
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            /* type_name() heap-allocates (strdup) for composite kinds (fn,
             * handler, union, ...) and no one frees it -- a LeakSanitizer-visible
             * leak on every if-branch mismatch involving a function type. Print
             * into owned buffers we free here, matching the leak-clean pattern in
             * elab_call.c's arg-mismatch diagnostic. */
            Buf then_buf; buf_init(&then_buf);
            type_print(&then_buf, then_->type);
            buf_putc(&then_buf, '\0');
            Buf else_buf; buf_init(&else_buf);
            type_print(&else_buf, else_->type);
            buf_putc(&else_buf, '\0');
            diag_emit(DIAG_ERROR, call->span,
                      "if branches have mismatched types: then=%s else=%s",
                      then_buf.data, else_buf.data);
            buf_free(&then_buf);
            buf_free(&else_buf);
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

/* Phase DS3: (set! (.field s) v) - struct field write.
 *
 * Accepts two receiver shapes:
 *   - TY_STRUCT (direct struct value): the receiver binding must be `^mut`.
 *   - TY_RC where struct_def is set (rc<Struct>): no `^mut` required
 *     (interior mutability through the rc wrapper).
 *
 * For rc-typed fields, the prior value's strong count is decremented before
 * the new pointer is stored (the new value already carries its own +1).
 * For move-only non-rc values the source binding is marked moved. */
static Expr *elab_set_field(Elab *e, const Form *call, Form *target) {
    /* target shape: (.field receiver-form) */
    Form *head_form = target->as.list.items[0];
    const char *fname = head_form->as.sym->name + 1; /* skip leading '.' */

    Expr *receiver = elab_form(e, target->as.list.items[1]);
    if (!receiver) return NULL;

    /* Resolve the struct def -- either directly or through the rc wrapper. */
    StructDef *def = NULL;
    bool receiver_is_rc = false;
    /* end-to-end-monomorphization: a :heap receiver is a typed pointer to a
     * shared heap header -- mutation through it is interior (like rc), so no
     * `^mut` on the local binding is required, and the field write derefs. */
    bool receiver_is_heap = type_is_heap_struct(receiver->type);
    Type rt = receiver->type;
    const Type *receiver_struct_type = &receiver->type;
    if (rt.kind == TY_STRUCT) {
        def = rt.as.struct_.def;
    } else if (rt.kind == TY_APP) {
        Type app_args[8];
        for (uint32_t si = 0; si < e->n_struct_defs; si++) {
            StructDef *candidate = e->struct_defs[si];
            if (candidate->n_type_params == 0 || candidate->n_type_params > 8) continue;
            if (elab_struct_type_extract_args(&rt, candidate, app_args)) {
                def = candidate;
                break;
            }
        }
    } else if (rt.kind == TY_RC && rt.as.rc.struct_def) {
        def = rt.as.rc.struct_def;
        receiver_is_rc = true;
        receiver_struct_type = &rt;
    } else if (rt.kind == TY_REF_MUT) {
        /* &mut Struct -- field write through a mutable borrow.  The
         * borrow already permits interior mutation; no ^mut on the
         * source binding required. */
        Type pointee = type_from_kind(rt.as.ref_borrow.target);
        if (pointee.kind == TY_STRUCT && receiver->type.as.ref_borrow.target == TY_STRUCT) {
            /* type_from_kind loses the struct def; in practice &mut Struct
             * is rare for field writes today.  Fall through to error. */
        }
        diag_emit(DIAG_ERROR, target->span,
                  "set! through &mut Struct is not yet supported -- "
                  "use a ^mut struct binding or rc<Struct> instead");
        return NULL;
    } else {
        diag_emit(DIAG_ERROR, target->span,
                  "set! (.field s): receiver must be a struct or rc<Struct>, got %s",
                  type_name(rt));
        return NULL;
    }

    /* Find the field by name. */
    uint32_t fi = 0;
    for (; fi < def->n_fields; fi++) {
        if (strcmp(def->fields[fi].name, fname) == 0) break;
    }
    if (fi >= def->n_fields) {
        diag_emit(DIAG_ERROR, head_form->span,
                  "set! (.%s s): struct '%s' has no field '%s'",
                  fname, def->name, fname);
        return NULL;
    }

    /* Direct-struct receivers require ^mut on the binding.  Through-rc and
     * through a :heap typed pointer are interior-mutable so any binding is fine. */
    if (!receiver_is_rc && !receiver_is_heap && receiver->kind == EX_VAR) {
        Binding *rb = receiver->as.var.binding;
        if (rb->is_moved) {
            diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0005_USE_AFTER_MOVE,
                                "use-after-move: cannot set! field of '%s' because it was moved",
                                rb->name->name);
            return NULL;
        }
        if (!rb->is_mut) {
            diag_emit(DIAG_ERROR, target->span,
                      "set! (.%s ...): '%s' is immutable; use ^mut at the binding site",
                      fname, rb->name->name);
            return NULL;
        }
    }

    /* Elaborate the value and type-check against the field. */
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;

    Type expected_field;
    expected_field = elab_struct_field_use_type(e, receiver_struct_type, def, &def->fields[fi]);
    if (value->type.kind != TY_PTR_VOID && !type_eq(value->type, expected_field)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set! (.%s ...): value type %s does not match field type %s",
                  fname, type_name(value->type), type_name(expected_field));
        return NULL;
    }

    /* Move-at-set for rc-managed payloads, mirroring the DS2 scan
     * in elab_make_struct. */
    if (value->kind == EX_VAR && value->as.var.binding) {
        TypeKind vk = value->type.kind;
        if (vk == TY_RC || vk == TY_WEAK || vk == TY_EXISTS) {
            (void)binding_mark_moved(value->as.var.binding, value->span);
        }
    }

    Expr *out = expr_new(e->arena, EX_SET_FIELD, TYPE_NIL, call->span);
    out->as.set_field_.receiver = receiver;
    out->as.set_field_.value = value;
    out->as.set_field_.field_idx = fi;
    out->as.set_field_.def = def;
    out->as.set_field_.receiver_is_rc = receiver_is_rc;
    return out;
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

    /* Phase DS3: (set! (.field s) v) -- struct field write. */
    if (target->tag == F_LIST && target->as.list.len == 2
        && target->as.list.items[0]->tag == F_SYM
        && target->as.list.items[0]->as.sym->len > 1
        && target->as.list.items[0]->as.sym->name[0] == '.') {
        return elab_set_field(e, call, target);
    }

    if (target->tag != F_SYM) {
        diag_emit(DIAG_ERROR, target->span,
                  "set! target must be a symbol, (@ borrow), or (.field struct)");
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

    /* R1: reject a `?` applied to a literal scalar up front.  A literal can
     * never be a Result value, and -- being a literal -- it can never be a
     * moved-from binding, so inspecting it here is side-effect free (no risk
     * of double-evaluating an owned operand).  Computed non-Result operands
     * are still caught downstream by the TUR-E0001 arg-type check on
     * __tur-q-is-err?; this branch just gives the canonical mistake a clearer,
     * `?`-specific message. */
    switch (inner->tag) {
        case F_INT: case F_FLOAT: case F_BOOL:
        case F_NIL: case F_STR: case F_KEYWORD: {
            const char *what =
                inner->tag == F_INT     ? "int"     :
                inner->tag == F_FLOAT   ? "float"   :
                inner->tag == F_BOOL    ? "bool"    :
                inner->tag == F_STR     ? "cstr"    :
                inner->tag == F_KEYWORD ? "keyword" : "nil";
            diag_emit_with_code(DIAG_ERROR, inner->span, TUR_E0001_TYPE_MISMATCH,
                                "? operator requires a Result value, got %s", what);
            return NULL;
        }
        default:
            break;
    }

    /* Fresh symbol __q_N to avoid multiple evaluation of expr. */
    char q_name[32];
    snprintf(q_name, sizeof(q_name), "__q_%u", e->next_gensym_id++);
    const Symbol *q_sym = symtab_intern(e->st,
        strslice(q_name, (uint32_t)strlen(q_name)));
    Form *q_sym_f = form_sym(e->arena, span, q_sym);

    /* The ? lowering routes through two stdlib helpers --
     * __tur-q-is-err? and __tur-q-ok-val (defined in stdlib/result.tur) --
     * that wrap the unsafe err? / ok-val calls inside (unsafe ...). This
     * keeps the unsafe audit surface for ? to a single pair of named
     * functions and means user call sites of ? do not need their own
     * (unsafe ...) wrapper. Update both lowering and the stdlib helpers
     * together if the internal result representation changes. */
    const Symbol *sym_is_err = symtab_intern(e->st,
        strslice("__tur-q-is-err?", 15));
    const Symbol *sym_ok_val = symtab_intern(e->st,
        strslice("__tur-q-ok-val", 14));

    Form *is_err_f  = form_sym(e->arena, span, sym_is_err);
    Form *ok_val_f  = form_sym(e->arena, span, sym_ok_val);
    Form *if_f      = form_sym(e->arena, span, e->sym_if);
    Form *let_f     = form_sym(e->arena, span, e->sym_let);
    Form *return_f  = form_sym(e->arena, span, e->sym_return);

    /* (__tur-q-is-err? __q) */
    Form *test_items[2] = { is_err_f, q_sym_f };
    Form *test_form = form_list(e->arena, span, test_items, 2);

    /* (return __q) -- __q is already the err Result; propagate it as-is. */
    Form *return_items[2] = { return_f, q_sym_f };
    Form *return_form = form_list(e->arena, span, return_items, 2);

    /* (__tur-q-ok-val __q) */
    Form *ok_val_items[2] = { ok_val_f, q_sym_f };
    Form *ok_val_form = form_list(e->arena, span, ok_val_items, 2);

    /* (if (__tur-q-is-err? __q) (return __q) (__tur-q-ok-val __q)) */
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

/* CF5: recursively scan a Form tree for any reference to a given symbol. */
static bool form_contains_sym(const Form *f, const Symbol *sym) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == sym;
    if (f->tag == F_LIST || f->tag == F_VEC || f->tag == F_MAP || f->tag == F_SET) {
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_contains_sym(f->as.list.items[i], sym)) return true;
    }
    return false;
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

    /* CF5: detect recursive generator -- enclosing function calls itself in body. */
    bool gen_is_recursive = false;
    if (e->current_fn_name) {
        for (uint32_t i = 2; i < call->as.list.len; i++) {
            if (form_contains_sym(call->as.list.items[i], e->current_fn_name)) {
                gen_is_recursive = true;
                break;
            }
        }
    }

    /* Push gen context */
    GenContext gctx;
    memset(&gctx, 0, sizeof(gctx));
    gctx.parent = e->gen_ctx;
    gctx.is_recursive = gen_is_recursive;
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
    /* CF5 (TUR-E0702): yield inside a match arm is unsupported in v1. */
    if (e->in_match_arm) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0702_YIELD_IN_MATCH_ARM,
            "'yield' is not supported inside a 'match' arm (1.0 limitation); "
            "this requires the post-1.0 CPS pass.");
        return NULL;
    }
    /* CF5 (TUR-E0703): yield inside a recursive generator is unsupported in v1. */
    if (e->gen_ctx->is_recursive) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0703_YIELD_IN_RECURSIVE_GEN,
            "'yield' is not supported inside a recursive generator (1.0 limitation); "
            "this requires the post-1.0 CPS pass.");
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
