/* elab_fns.c -- function definition forms: defn, fn, extern-c, def. */
#include "elab_internal.h"

/* Phase 2: defn — (defn name [param1 param2 ...] : return-type body...)
 * For now, we only support : int return type annotation. Param types are
 * inferred from usage. */
static bool fn_type_param_index(const Symbol **type_params, uint8_t n_type_params,
                                const Symbol *sym, uint8_t *out_idx) {
    if (!sym) return false;
    for (uint8_t i = 0; i < n_type_params; i++) {
        if (type_params[i] == sym) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static Type *fn_type_from_form(Elab *e, const Form *form,
                               const Symbol **type_params,
                               Kind *type_param_kinds,
                               uint8_t n_type_params) {
    if (!form) return NULL;
    if (form->tag == F_TYPE_ANN && form->as.list.len > 0) {
        return fn_type_from_form(e, form->as.list.items[0],
                                 type_params, type_param_kinds, n_type_params);
    }
    if (form->tag == F_SYM || form->tag == F_KEYWORD) {
        const Symbol *sym = form->as.sym;
        uint8_t idx = 0;
        if (fn_type_param_index(type_params, n_type_params, sym, &idx)) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_tyvar_named(sym->name);
            t->hkt_kind = type_param_kinds ? type_param_kinds[idx] : KIND_STAR;
            return t;
        }
        Type *t = type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        if (t && t->kind == TY_STRUCT && t->as.struct_.def == NULL) {
            Type *tv = (Type *)arena_alloc(e->arena, sizeof(Type));
            *tv = type_tyvar_named(sym->name);
            return tv;
        }
        return t;
    }
    if (form->tag == F_LIST && form->as.list.len >= 1 &&
        form->as.list.items[0]->tag == F_SYM) {
        const Symbol *head = form->as.list.items[0]->as.sym;
        /* KB-008/KB-020/KB-018/KB-019: (-> T1 T2), (fn [...] :ret),
         * (lref T), (handler E V R), and the session/role type
         * constructors (Session, Send, Recv, Choose, Branch, Rec,
         * Timeout, Role) are all type-constructor forms with special
         * handling in type_expr_from_form, not generic type applications.
         * Route them through the same path so the spaced annotation form
         * (`x : (-> a b)`, `p : (lref int)`, `ch :(Session (Send ...))`)
         * resolves identically to the keyword form. */
        if (head == e->sym_forall || head == e->sym_exists ||
            head == e->sym_forall_u || head == e->sym_exists_u ||
            head == e->sym_arrow || head == e->sym_fn ||
            /* LS1: borrow *type* heads -- &T / &mut T (and lifetime-annotated
             * &'a T / &mut 'a T) are type-constructor forms, not generic
             * applications.  `&`-headed lists are also caught by has_amp below,
             * but &mut needs its own routing. */
            head == e->sym_ampersand || head == e->sym_borrow_mut ||
            head == e->sym_lref || head == e->sym_handler_type ||
            head == e->sym_session_type ||
            head == e->sym_session_Send || head == e->sym_session_Recv ||
            head == e->sym_session_Choose || head == e->sym_session_Branch ||
            head == e->sym_session_Rec || head == e->sym_session_Timeout ||
            head == e->sym_role_type || head == e->sym_project_type) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        bool has_pipe = false, has_amp = false;
        for (uint32_t i = 0; i < form->as.list.len; i++) {
            Form *item = form->as.list.items[i];
            if (item->tag != F_SYM) continue;
            if (item->as.sym == e->sym_pipe) has_pipe = true;
            if (item->as.sym == e->sym_ampersand) has_amp = true;
        }
        if (has_pipe || has_amp) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
        Type *cur = fn_type_from_form(e, form->as.list.items[0],
                                      type_params, type_param_kinds, n_type_params);
        if (!cur) return NULL;
        for (uint32_t i = 1; i < form->as.list.len; i++) {
            Type *arg = fn_type_from_form(e, form->as.list.items[i],
                                          type_params, type_param_kinds, n_type_params);
            if (!arg) return NULL;
            Type *next = (Type *)arena_alloc(e->arena, sizeof(Type));
            *next = type_app(e->arena, *cur, *arg, form->span);
            cur = next;
        }
        return cur;
    }
    return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
}

/* Typed-variadic rest: resolve a `& rest :T` annotation form into a rest
 * element kind plus an optional full Type.  The full Type is non-NULL only for
 * user-defined int64_t-carried types (opaque / struct / ADT / type
 * application) so that call sites can compare type identity (e.g. distinguish
 * Route from Middleware); primitives keep rest_full_type == NULL and use the
 * fast TypeKind comparison.  Declared type parameters yield a polymorphic rest
 * (TY_TYVAR, accepts any arg).  An unknown type name is a hard error -- never
 * silently demoted to :int.  `ctx` is the form name for diagnostics
 * ("defn" / "fn"). */
static bool resolve_variadic_rest_type(Elab *e, const Form *type_p,
                                       const Symbol **fn_type_params,
                                       Kind *fn_type_param_kinds,
                                       uint8_t n_fn_type_params,
                                       const char *ctx,
                                       TypeKind *out_kind,
                                       Type **out_full) {
    *out_kind = TY_INT;
    *out_full = NULL;
    const Symbol *sym = type_p->as.sym;
    /* A declared type parameter (e.g. `& rest :A` in a `[A]` defn) is a
     * polymorphic rest that accepts any argument via the TY_TYVAR fast path. */
    uint8_t tpi = 0;
    if (fn_type_param_index(fn_type_params, n_fn_type_params, sym, &tpi)) {
        *out_kind = TY_TYVAR;
        return true;
    }
    Type *rt = fn_type_from_form(e, type_p, fn_type_params,
                                 fn_type_param_kinds, n_fn_type_params);
    /* fn_type_from_form maps any unrecognised name to a named type variable.
     * Since we already handled declared type params above, a TY_TYVAR result
     * here means the name is undefined. */
    if (!rt || rt->kind == TY_TYVAR) {
        diag_emit(DIAG_ERROR, type_p->span,
                  "%s: unknown rest type '%s'", ctx, sym ? sym->name : "?");
        return false;
    }
    *out_kind = rt->kind;
    if (rt->kind == TY_STRUCT || rt->kind == TY_ADT || rt->kind == TY_APP) {
        *out_full = rt;
    }
    return true;
}

static bool fn_type_has_named_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name != NULL;
        case TY_APP:
            return fn_type_has_named_tyvar(t->as.app.fn) ||
                   fn_type_has_named_tyvar(t->as.app.arg);
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (fn_type_has_named_tyvar(t->as.union_.members[i])) return true;
            }
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (fn_type_has_named_tyvar(t->as.intersection_.members[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

/* KB-025: append every named type variable appearing in `t` to the elaborator's
 * signature-tyvar set (deduped, capped).  Used so a GADT match arm can tell a
 * legitimately-polymorphic result (`a` is quantified by the fn) apart from a
 * skolem that escapes into a concrete return position. */
static void fn_collect_sig_tyvars(Elab *e, const Type *t) {
    if (!t) return;
    switch (t->kind) {
        case TY_TYVAR:
            if (t->as.tyvar_.name) {
                for (uint8_t i = 0; i < e->n_sig_tyvars; i++) {
                    if (e->sig_tyvars[i] &&
                        strcmp(e->sig_tyvars[i], t->as.tyvar_.name) == 0) return;
                }
                if (e->n_sig_tyvars < 32) {
                    e->sig_tyvars[e->n_sig_tyvars++] = t->as.tyvar_.name;
                }
            }
            return;
        case TY_APP:
            fn_collect_sig_tyvars(e, t->as.app.fn);
            fn_collect_sig_tyvars(e, t->as.app.arg);
            return;
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++)
                fn_collect_sig_tyvars(e, t->as.union_.members[i]);
            return;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++)
                fn_collect_sig_tyvars(e, t->as.intersection_.members[i]);
            return;
        case TY_FN:
            if (t->as.fn.arg_full_types) {
                for (uint8_t i = 0; i < t->as.fn.arity; i++)
                    fn_collect_sig_tyvars(e, t->as.fn.arg_full_types[i]);
            }
            fn_collect_sig_tyvars(e, t->as.fn.result_full_type);
            return;
        default:
            return;
    }
}

/* KB-026: does `t` mention the named type variable `name` anywhere? */
static bool fn_type_mentions_named(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return fn_type_mentions_named(t->as.app.fn, name) ||
                   fn_type_mentions_named(t->as.app.arg, name);
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++)
                if (fn_type_mentions_named(t->as.union_.members[i], name)) return true;
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++)
                if (fn_type_mentions_named(t->as.intersection_.members[i], name)) return true;
            return false;
        case TY_FN:
            if (t->as.fn.arg_full_types)
                for (uint8_t i = 0; i < t->as.fn.arity; i++)
                    if (fn_type_mentions_named(t->as.fn.arg_full_types[i], name)) return true;
            return fn_type_mentions_named(t->as.fn.result_full_type, name);
        default:
            return false;
    }
}

/* KB-026: is `name` the declared type parameter of some in-scope ADT or
 * struct?  Such a name (e.g. `a` from `(defgadt Witness [a] ...)`) is a genuine
 * type variable even when it occurs only once in a signature, because it gets
 * refined per match arm. */
static bool fn_name_is_adt_tyvar(const Elab *e, const char *name) {
    if (!name) return false;
    for (uint32_t i = 0; i < e->n_adt_defs; i++) {
        const AdtDef *d = e->adt_defs[i];
        for (uint8_t k = 0; k < d->n_type_params; k++)
            if (d->type_params[k] && strcmp(d->type_params[k], name) == 0) return true;
    }
    for (uint32_t i = 0; i < e->n_struct_defs; i++) {
        const StructDef *d = e->struct_defs[i];
        for (uint8_t k = 0; k < d->n_type_params; k++)
            if (d->type_params[k] && strcmp(d->type_params[k], name) == 0) return true;
    }
    return false;
}

static bool form_mentions_type_param(const Form *form, const Symbol *sym) {
    if (!form || !sym) return false;
    switch (form->tag) {
        case F_SYM:
        case F_KEYWORD:
            return form->as.sym == sym;
        case F_TYPE_ANN:
        case F_LIST:
        case F_VEC:
        case F_MAP:
        case F_CONTRACT_TYPE:
            for (uint32_t i = 0; i < form->as.list.len; i++) {
                if (form_mentions_type_param(form->as.list.items[i], sym)) return true;
            }
            return false;
        default:
            return false;
    }
}

static const Form *fn_return_annotation_form(const Form *call, uint32_t after_params_idx) {
    if (!call || call->tag != F_LIST || call->as.list.len <= after_params_idx) return NULL;
    uint32_t idx = after_params_idx;
    if (idx < call->as.list.len && call->as.list.items[idx]->tag == F_MAP) idx++;
    if (idx < call->as.list.len) {
        Form *ret_f = call->as.list.items[idx];
        if (ret_f->tag == F_KEYWORD || ret_f->tag == F_TYPE_ANN) return ret_f;
    }
    return NULL;
}

static uint8_t collect_implicit_fn_type_params(const Form *params_f, const Form *ret_f,
                                               const Symbol **out_params,
                                               Kind *out_kinds) {
    if (!params_f || params_f->tag != F_VEC) return 0;
    uint8_t n = 0;
    for (uint32_t i = 0; i < params_f->as.list.len && n < 8; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) break;
        bool mentioned = false;
        for (uint32_t j = i + 1; j < params_f->as.list.len; j++) {
            if (form_mentions_type_param(params_f->as.list.items[j], p->as.sym)) {
                mentioned = true;
                break;
            }
        }
        if (!mentioned && ret_f) mentioned = form_mentions_type_param(ret_f, p->as.sym);
        if (!mentioned) break;
        out_params[n] = p->as.sym;
        out_kinds[n] = KIND_STAR;
        n++;
    }
    return n;
}

/* TY4: reject a function whose result is a borrow of one of its own locals
 * (params or let-locals).  Such a borrow dangles once the frame is gone --
 * today it only surfaces as a C -Wdangling-pointer warning.  Walk the body's
 * result position through do/let/if tails; fn_local_depth is the depth of the
 * function's parameter scope, so any referent at that depth or deeper is a
 * function-local.  Emits TUR-E0105 and returns false on the first violation. */
static bool check_no_borrow_escape(const Expr *tail, uint32_t fn_local_depth,
                                   const Symbol *fn_name) {
    if (!tail) return true;
    switch (tail->kind) {
        case EX_DO:
            return tail->as.do_.n == 0 ? true
                : check_no_borrow_escape(tail->as.do_.items[tail->as.do_.n - 1],
                                         fn_local_depth, fn_name);
        case EX_LET:
        case EX_LETREC:
            return check_no_borrow_escape(tail->as.let_.body, fn_local_depth, fn_name);
        case EX_IF:
            return check_no_borrow_escape(tail->as.if_.then_, fn_local_depth, fn_name)
                && check_no_borrow_escape(tail->as.if_.else_or_null, fn_local_depth, fn_name);
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT: {
            const Binding *ref = borrow_referent_binding(tail);
            if (ref && !ref->is_global && ref->scope_depth >= fn_local_depth) {
                diag_emit_with_code(DIAG_ERROR, tail->span,
                    TUR_E0105_BORROW_ESCAPES_SCOPE,
                    "function '%s' returns a borrow of local `%s`, "
                    "which does not outlive the function",
                    fn_name ? fn_name->name : "?", ref->name->name);
                return false;
            }
            return true;
        }
        case EX_CALL: {
            /* LS4: inter-procedural borrow escape.  A call that returns a
             * lifetime-tied borrow (&'a T) yields a borrow of whatever the
             * tied argument borrows.  Follow the escape check into that
             * argument: if it ultimately borrows a caller-local, the returned
             * borrow dangles just as a direct (& local) would.  The callee's
             * result_borrow_arg names the parameter the return is tied to. */
            const Binding *cb = tail->as.call_.fn_binding;
            if (cb && cb->type.kind == TY_FN) {
                int8_t bi = cb->type.as.fn.result_borrow_arg;
                if (bi >= 0 && (uint32_t)bi < tail->as.call_.n_args
                        && tail->as.call_.args) {
                    return check_no_borrow_escape(tail->as.call_.args[bi],
                                                  fn_local_depth, fn_name);
                }
            }
            return true;
        }
        default:
            return true;
    }
}

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

    /* F4 (cross-plan-followups): ^deprecated attribute before name.
     * Syntax: (defn ^deprecated "message" name [...] :ret body...)
     *         (defn ^deprecated name [...] :ret body...)            ; no message
     * Each use site of the defined binding emits a DIAG_WARNING with
     * the message (or a generic note if the message is omitted). */
    bool is_deprecated_attr = false;
    const char *deprecation_msg = NULL;
    if (name_f->tag == F_SYM && name_f->as.sym == e->sym_caret_deprecated) {
        is_deprecated_attr = true;
        name_idx++;
        if (name_idx >= call->as.list.len) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "^deprecated must be followed by an optional message string "
                      "and the function name");
            return NULL;
        }
        Form *next = call->as.list.items[name_idx];
        if (next->tag == F_STR) {
            char *msg_buf = (char *)arena_alloc(e->arena, next->as.s.len + 1);
            memcpy(msg_buf, next->as.s.p, next->as.s.len);
            msg_buf[next->as.s.len] = '\0';
            deprecation_msg = msg_buf;
            name_idx++;
            if (name_idx >= call->as.list.len) {
                diag_emit(DIAG_ERROR, next->span,
                          "^deprecated message must be followed by the function name");
                return NULL;
            }
        }
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
        /* MF3: hard-error on collision with an auto-loaded stdlib name. The
         * elaborator otherwise treats stdlib bindings as forward declarations
         * (because they're TY_FN + is_global, same shape as a pass-1 user
         * forward-decl) and lets the user defn shadow them; the C compile
         * then fails with "conflicting types for 'ok'" or similar. Producing
         * a clear diagnostic here avoids the broken-C symptom.  Suppress
         * during stdlib auto-load itself so stdlib pass-1 forward decls can
         * be matched by their pass-2 real definitions. */
        if (existing->is_from_stdlib && !e->in_stdlib_load) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is already defined by an auto-loaded stdlib "
                      "module; rename the local definition",
                      name_f->as.sym->name);
            return NULL;
        }
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

    const Symbol *fn_type_params[8];
    Kind fn_type_param_kinds[8];
    uint8_t n_fn_type_params = 0;
    uint8_t n_implicit_fn_type_params = 0;
    memset(fn_type_params, 0, sizeof(fn_type_params));
    for (uint8_t i = 0; i < 8; i++) fn_type_param_kinds[i] = KIND_STAR;

    uint32_t params_idx = name_idx + 1;
    if (call->as.list.len > name_idx + 2 &&
        call->as.list.items[name_idx + 1]->tag == F_VEC &&
        call->as.list.items[name_idx + 2]->tag == F_VEC) {
        Form *type_params_f = call->as.list.items[name_idx + 1];
        if (type_params_f->as.list.len > 8) {
            diag_emit(DIAG_ERROR, type_params_f->span,
                      "defn: too many type parameters (max 8)");
            return NULL;
        }
        n_fn_type_params = (uint8_t)type_params_f->as.list.len;
        for (uint8_t i = 0; i < n_fn_type_params; i++) {
            Form *tp = type_params_f->as.list.items[i];
            if (tp->tag != F_SYM) {
                diag_emit(DIAG_ERROR, tp->span,
                          "defn: type parameter must be a symbol");
                return NULL;
            }
            fn_type_params[i] = tp->as.sym;
            fn_type_param_kinds[i] = KIND_STAR;
        }
        params_idx = name_idx + 2;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[params_idx];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* LS1: open a lifetime context for this signature so borrow-type
     * annotations (&'a int, &mut 'a int) in the params and return type intern
     * their lifetimes into shared, stable per-function LifetimeIds.  Restored to
     * the saved value before the body is elaborated (a nested defn gets its own).
     */
    LifetimeContext sig_ltctx;
    lifetime_context_init(&sig_ltctx);
    LifetimeContext *saved_ltctx = e->cur_lifetime_ctx;
    e->cur_lifetime_ctx = &sig_ltctx;

    const Form *implicit_ret_f = NULL;
    if (n_fn_type_params == 0) {
        implicit_ret_f = fn_return_annotation_form(call, params_idx + 1);
        n_implicit_fn_type_params = collect_implicit_fn_type_params(params_f, implicit_ret_f,
                                                                    fn_type_params, fn_type_param_kinds);
        n_fn_type_params = n_implicit_fn_type_params;
    }

    /* Parse params - Phase 15 supports typeclass constraints.
     * Parameter type annotations accept both fused `[x :T]` and spaced
     * `[x : T]` forms.
     *
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
    /* A#1: ^fat annotation marks the next parameter as a fat-closure consumer */
    bool next_param_fat       = false;
    /* AR5: variadic rest parameter state */
    bool is_variadic = false;
    TypeKind rest_kind = TY_INT;  /* default rest element type */
    Type *rest_full_type = NULL;  /* typed-variadic: full Type for user-defined rest */

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];

        /* AR5: & rest-name :type -- variadic rest parameter */
        if (p->tag == F_SYM && p->as.sym == e->sym_borrow) {
            if (is_variadic) {
                diag_emit(DIAG_ERROR, p->span, "defn: multiple '&' in parameter list");
                return NULL;
            }
            if (i + 1 >= params_f->as.list.len) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: '&' must be followed by a rest parameter name");
                return NULL;
            }
            Form *rest_p = params_f->as.list.items[i + 1];
            if (rest_p->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_p->span,
                          "defn: rest parameter name must be a symbol");
                return NULL;
            }
            /* Parse optional type annotation: & rest :type */
            rest_kind = TY_INT;
            rest_full_type = NULL;
            if (i + 2 < params_f->as.list.len) {
                Form *type_p = params_f->as.list.items[i + 2];
                if (type_p->tag == F_KEYWORD) {
                    if (!resolve_variadic_rest_type(e, type_p,
                                                    fn_type_params, fn_type_param_kinds,
                                                    n_fn_type_params, "defn",
                                                    &rest_kind, &rest_full_type)) {
                        return NULL;
                    }
                    if (i + 3 < params_f->as.list.len) {
                        diag_emit(DIAG_ERROR, params_f->as.list.items[i + 3]->span,
                                  "defn: no parameters allowed after '& rest :type'");
                        return NULL;
                    }
                } else {
                    diag_emit(DIAG_ERROR, type_p->span,
                              "defn: '& rest' must be followed by a type annotation (e.g. :int)");
                    return NULL;
                }
            }
            is_variadic = true;
            /* Add rest param as a regular int binding (cons-list pointer at runtime) */
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
            }
            param_kinds[n_params] = TY_INT;
            Binding *rest_b = binding_new(e, rest_p->as.sym, TYPE_INT, false, false, rest_p->span);
            rest_b->is_param = true;
            params[n_params++] = rest_b;
            break; /* & must be the last; done parsing params */
        }

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

        /* A#1: ^fat annotation marks the next parameter as a fat-closure consumer.
         * A bare non-capturing fn passed to this parameter is auto-shimmed into a
         * fat closure at the call site (EX_FN_TO_FAT). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_fat) {
            next_param_fat = true;
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
        if (i < n_implicit_fn_type_params && p->tag == F_SYM &&
            fn_type_params[i] == p->as.sym) {
            continue;
        }

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
            Type *ann = fn_type_from_form(e, type_form,
                                          fn_type_params, fn_type_param_kinds, n_fn_type_params);
            if (!ann) return NULL;
            /* CT0: For contract types, use base type for C-level representation */
            if (ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
                TypeKind base_kind = ann->as.contract_.base_type->kind;
                param_kinds[n_params - 1] = base_kind;
                params[n_params - 1]->type = *ann->as.contract_.base_type;
                continue;
            }
            if (ann->kind == TY_FORALL) {
                /* Rank-2 polymorphic parameter: represented as tur_poly_fn_t at C level */
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = ann;
                param_poly_types[n_params - 1] = ann;
            } else if (ann->kind == TY_EXISTS) {
                /* F1-1: TY_EXISTS is a value type (produced by `pack`), not a
                 * rank-2 function.  Keep the full TY_EXISTS payload on the
                 * binding so `open` inside the body finds a valid
                 * `as.forall_.body`, and stash the full type in
                 * param_poly_types so call sites can subtype-check the
                 * argument against the declared existential.  Earlier code
                 * misrouted this into the rank-2 branch, which then
                 * rejected `(pack ...)` arguments with "rank-2 argument
                 * must be a named function". */
                param_kinds[n_params - 1] = TY_EXISTS;
                params[n_params - 1]->type = *ann;
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
                /* PH1.1: For handler-typed parameters, store the full type so
                 * the declared handled-effect row + value/result kinds reach
                 * arg_full_types at the call site, enabling row-precise
                 * argument checking (PH1.2) and a precise mismatch diagnostic
                 * (PH1.3) instead of a kind-only handler<?, ?, ?> comparison. */
                if (g_effect_types_enabled && ann->kind == TY_HANDLER) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* GS2: preserve full applied struct parameter types so call
                 * sites can distinguish (Box int) from (Box float) instead of
                 * comparing only the TY_APP kind shell. */
                if (ann->kind == TY_APP) {
                    param_poly_types[n_params - 1] = ann;
                }
                if (fn_type_has_named_tyvar(ann)) {
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

        /* Accept both fused :type (F_KEYWORD) and spaced `: type` (F_TYPE_ANN{inner: F_SYM}) */
        const Form *p_eff = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
        if (p_eff->tag != F_SYM && p_eff->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: parameter must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }

        /* Handle type annotations: if this is a keyword like :int, it's a type for the previous param */
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            /* This is a type annotation for the previous parameter */
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* Update the type of the last parameter */
            const Symbol *kw = p_eff->as.sym;
            uint8_t type_param_idx = 0;
            if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
                param_kinds[n_params - 1] = TY_TYVAR;
                params[n_params - 1]->type = type_tyvar_named(kw->name);
                params[n_params - 1]->type.hkt_kind = fn_type_param_kinds[type_param_idx];
                param_poly_types[n_params - 1] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *param_poly_types[n_params - 1] = params[n_params - 1]->type;
                continue;
            }
            /* Phase N: use typekind_from_symbol to resolve all known type names
             * (including fixed-width numeric types) before falling through to the
             * type-variable path.  The fast-path checks below are kept for the
             * most common cases; everything else goes through typekind_from_symbol. */
            TypeKind _kw_kind = typekind_from_symbol(kw->name);
            if (_kw_kind != TY_UNKNOWN) {
                param_kinds[n_params - 1] = _kw_kind;
                params[n_params - 1]->type = type_from_kind(_kw_kind);
                params[n_params - 1]->type.copy_kind = typekind_default_copy_kind(_kw_kind);
                /* :ref and :lref params need substructural handling (see below) */
                if (_kw_kind == TY_REF) {
                    params[n_params - 1]->type = type_ref(TY_INT);
                    if (g_substructural_enabled && !params[n_params - 1]->is_linear
                            && !params[n_params - 1]->is_affine
                            && !params[n_params - 1]->is_relevant) {
                        params[n_params - 1]->is_linear = true;
                        params[n_params - 1]->type.substruct = SK_LINEAR;
                    }
                } else if (_kw_kind == TY_LREF) {
                    params[n_params - 1]->type = type_lref(TY_INT);
                    if (g_linear_enabled) params[n_params - 1]->is_linear = true;
                }
            } else if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
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
                    /* Phase TA1: check defalias table */
                    const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                    TypeKind ak = TY_UNKNOWN;
                    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                        if (e->type_alias_names[ai] == ksym) { ak = e->type_alias_kinds[ai]; break; }
                    }
                    if (ak != TY_UNKNOWN) {
                        param_kinds[n_params - 1] = ak;
                        params[n_params - 1]->type = type_from_kind(ak);
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
                        /* Phase D: try to look up as a struct name (mirrors return-type path). */
                        StructDef *param_struct = NULL;
                        for (uint32_t si = 0; si < e->n_struct_defs; si++) {
                            if (strcmp(e->struct_defs[si]->name, kw->name) == 0) {
                                param_struct = e->struct_defs[si];
                                break;
                            }
                        }
                        if (param_struct) {
                            param_kinds[n_params - 1] = TY_STRUCT;
                            params[n_params - 1]->type = type_struct(param_struct);
                        } else {
                        /* Phase HRT/G2: Unknown keyword -- treat as an implicit type variable.
                         * A parameter annotation like :a where 'a' is not a known type or ADT
                         * is an implicit type variable. Mark the binding TY_TYVAR so that inside
                         * a GADT match arm, the per-arm skolem env can resolve it to a concrete type. */
                        param_kinds[n_params - 1] = TY_TYVAR;
                        params[n_params - 1]->type = type_tyvar_named(kw->name);
                        param_poly_types[n_params - 1] = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *param_poly_types[n_params - 1] = params[n_params - 1]->type;
                        } /* end struct else */
                    }
                    } /* end TA1 else */
                }
            }
            continue;
        }

        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: too many fixed parameters (max %d); use '& rest :type' for a rest list",
                      MAX_FN_ARITY);
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* For phase 2, default to int */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        b->is_param = true;
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
        /* A#1: If the previous ^fat annotation applied to this parameter, mark it
         * as a fat-closure consumer so call sites auto-shim bare fn arguments. */
        if (next_param_fat) {
            b->is_fat = true;
            next_param_fat = false;
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
    Type *return_session_type = NULL; /* SS3a/SS7: full session/role return type */
    Type *return_app_type = NULL; /* PTC4: full TY_APP return type for concrete type threading */
    Type *return_exists_type = NULL; /* F1-1: full TY_EXISTS/TY_FORALL return type so callers see the forall_ payload (without it elab_open SEGVs reading body) */
    Type *return_fn_type = NULL; /* Issue 1b: full TY_FN return type so callers see the complete function signature (arity, result_kind) rather than a zeroed TY_FN shell */
    Type *return_tyvar_type = NULL; /* GS4: full TY_TYVAR return type for call-site substitution */
    Type *return_borrow_type = NULL; /* LS2: full borrow return type (&'a T) so lifetime IDs survive */
    uint32_t body_start = params_idx + 1;  /* params_idx = params vector */

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
    /* CF7.3: record the scope just before this function's inner scope is pushed,
     * so check_cloneable_capture can stop at the function boundary. */
    struct Scope *saved_fn_entry_outer_scope = e->fn_entry_outer_scope;
    e->fn_entry_outer_scope = e->scope;
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    /* TY4: depth of the function's parameter/local scope, for the
     * borrow-escape check (see check_no_borrow_escape, called after the body
     * is elaborated).  Bindings at this depth or deeper are function-locals. */
    uint32_t fn_local_depth = 0;
    for (const Scope *s = e->scope; s; s = s->parent) fn_local_depth++;
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
                /* Phase N6: sized primitive types as return type keywords.
                 * elab_types.c handles these via typekind_from_symbol; mirror
                 * that lookup here so `:int32`, `:uint8`, etc. work in defn. */
                {
                    TypeKind sized_k = typekind_from_symbol(kw->name);
                    if (sized_k != TY_UNKNOWN && sized_k != TY_INT &&
                            sized_k != TY_FLOAT && sized_k != TY_BOOL &&
                            sized_k != TY_CSTR && sized_k != TY_NIL) {
                        return_kind = sized_k;
                        body_start++;
                        goto done_return_annotation;
                    }
                }
                /* Phase G3: Try constraint env (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                uint8_t type_param_idx = 0;
                if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
                    return_kind = TY_TYVAR;
                    return_tyvar_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *return_tyvar_type = type_tyvar_named(kw->name);
                    return_tyvar_type->hkt_kind = fn_type_param_kinds[type_param_idx];
                } else if (ck != TY_UNKNOWN) {
                    return_kind = ck;
                } else {
                    /* Phase TA1: check defalias table */
                    bool alias_found = false;
                    {
                        const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                        for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                            if (e->type_alias_names[ai] == ksym) {
                                return_kind = e->type_alias_kinds[ai];
                                alias_found = true;
                                break;
                            }
                        }
                    }
                    if (!alias_found) {
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
                        /* GS4 compatibility: unknown return type keywords remain
                         * named type variables so :a and : a both work for generic
                         * binder forms. */
                        return_kind = TY_TYVAR;
                        return_tyvar_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *return_tyvar_type = type_tyvar_named(kw->name);
                    }
                    } /* end !alias_found */
                }
            }
            body_start++;
            done_return_annotation:;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = fn_type_from_form(e, ret_f->as.list.items[0],
                                              fn_type_params, fn_type_param_kinds, n_fn_type_params);
                if (ann) {
                    return_kind = ann->kind;
                    if (ann->kind == TY_TYVAR) {
                        return_tyvar_type = ann;
                    }
                    /* SS3a: Capture full session return type so callers see the complete
                     * protocol type (e.g. Session[Rec[self, ...]]) rather than a bare
                     * TY_SESSION shell with a NULL protocol pointer. */
                    if (g_sessions_enabled && (ann->kind == TY_SESSION || ann->kind == TY_ROLE)) {
                        return_session_type = ann;
                    }
                    /* PTC4: capture full TY_APP return type so dispatch can extract elem types. */
                    if (ann->kind == TY_APP) {
                        return_app_type = ann;
                    }
                    /* F1-1: capture full TY_EXISTS / TY_FORALL return type so
                     * call sites can patch the resulting expression's type
                     * with the complete forall_ payload (var_names, var_kinds,
                     * body, constraints).  Without this, type_fn() leaves the
                     * union uninitialised and elab_open later dereferences a
                     * garbage `body` pointer. */
                    if (ann->kind == TY_EXISTS || ann->kind == TY_FORALL) {
                        return_exists_type = ann;
                    }
                    /* Issue 1b: capture full TY_FN return type so callers see
                     * the complete function signature (arity, result_kind). */
                    if (ann->kind == TY_FN) {
                        return_fn_type = ann;
                    }
                    /* LS2: capture full borrow return type (&'a T / &mut 'a T)
                     * so its lifetime IDs reach the lifetime pass via
                     * FnDef.return_type. */
                    if (ann->kind == TY_REF_IMMUT || ann->kind == TY_REF_MUT) {
                        return_borrow_type = ann;
                    }
                    /* F2-1: a `:linear` existential cannot escape past the
                     * scope that packs it -- the linear discipline relies on
                     * a single `open` in the same scope freeing the bare
                     * malloc'd record (no rc, no defer chain).  Returning
                     * one from a defn breaks that guarantee silently.
                     * Reject at the annotation parsing point so the
                     * diagnostic fires regardless of how the body returns
                     * the value (direct pack, let-tail, conditional). */
                    if (ann->kind == TY_EXISTS && ann->as.forall_.is_linear) {
                        diag_emit(DIAG_ERROR, ret_f->span,
                                  "defn: a :linear existential cannot escape "
                                  "its packing scope -- it must be opened in "
                                  "the same scope that packed it (no return, "
                                  "no struct/collection storage)");
                        e->scope = inner.parent;
                        scope_free(&inner);
                        return NULL;
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

    /* LS1: signature parsing is done; the body must not intern signature
     * lifetimes.  Restore the enclosing context (NULL at top level). */
    e->cur_lifetime_ctx = saved_ltctx;

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
        /* AR6: propagate variadic flag early so call sites in the same body
         * see is_variadic=true even if body elaboration fails before b->type=fn_type. */
        existing->type.as.fn.is_variadic = is_variadic;
        existing->type.as.fn.rest_kind   = rest_kind;
        existing->type.as.fn.rest_full_type = rest_full_type;
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

    /* KB-026: gate GS4 implicit type variables.  A *bare* unknown lowercase
     * keyword used as a param or return type (e.g. `:a`) is treated as a named
     * type variable for generic binder forms -- but only when the name is
     * genuinely quantified by the signature: it is declared in an explicit
     * type-param list, is a kind variable, or relates two type positions
     * (appears in >=2 of {param types, return type}).  A name occurring in just
     * its own single annotation is a typo, and recovering it as a type variable
     * silently swallowed two diagnostics:
     *   - a bare `:a` return with no param mentioning `a` should be an
     *     "unsupported return type keyword" error; and
     *   - a lone `[n : nope]` should stay an unresolved type so the
     *     "parameter looks like it was followed by a type annotation" hint can
     *     fire on misuse.
     * This pass undoes those two over-eager recoveries. */
    {
        /* Demote lone bare-keyword params to an unresolved type. */
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->type.kind != TY_TYVAR ||
                !params[i]->type.as.tyvar_.name) continue;
            const char *nm = params[i]->type.as.tyvar_.name;
            bool declared = false;
            for (uint8_t k = 0; k < n_fn_type_params; k++)
                if (fn_type_params[k] && strcmp(fn_type_params[k]->name, nm) == 0) {
                    declared = true; break;
                }
            for (uint8_t k = 0; !declared && k < n_kind_vars; k++)
                if (kind_var_names[k] && strcmp(kind_var_names[k]->name, nm) == 0) {
                    declared = true; break;
                }
            if (declared) continue;
            /* A name that is some ADT/struct's declared type parameter (e.g. `a`
             * from a defgadt) is a genuine type variable -- it is refined per
             * match arm -- even when it appears only once in this signature. */
            if (fn_name_is_adt_tyvar(e, nm)) continue;
            uint8_t occ = 0;
            for (uint8_t j = 0; j < n_params; j++) {
                const Type *jt = param_poly_types[j]
                    ? param_poly_types[j] : &params[j]->type;
                if (fn_type_mentions_named(jt, nm)) occ++;
            }
            if (fn_type_mentions_named(return_tyvar_type, nm) ||
                fn_type_mentions_named(return_app_type, nm) ||
                fn_type_mentions_named(return_fn_type, nm) ||
                fn_type_mentions_named(return_exists_type, nm)) occ++;
            if (occ >= 2) continue;  /* genuine: relates >=2 type positions */
            Type unresolved; memset(&unresolved, 0, sizeof(unresolved));
            unresolved.kind = TY_STRUCT;
            unresolved.copy_kind = CK_MOVE;
            unresolved.hkt_kind = KIND_STAR;
            unresolved.as.struct_.def = NULL;
            params[i]->type = unresolved;
            param_kinds[i] = TY_STRUCT;
            param_poly_types[i] = NULL;
        }

        /* Reject a bare return type variable with no quantifying binder. */
        if (return_kind == TY_TYVAR && return_tyvar_type &&
            return_tyvar_type->as.tyvar_.name) {
            const char *rn = return_tyvar_type->as.tyvar_.name;
            bool declared = false;
            for (uint8_t k = 0; k < n_fn_type_params; k++)
                if (fn_type_params[k] && strcmp(fn_type_params[k]->name, rn) == 0) {
                    declared = true; break;
                }
            for (uint8_t k = 0; !declared && k < n_kind_vars; k++)
                if (kind_var_names[k] && strcmp(kind_var_names[k]->name, rn) == 0) {
                    declared = true; break;
                }
            uint8_t occ = 1;  /* the return position itself */
            for (uint8_t j = 0; j < n_params; j++) {
                const Type *jt = param_poly_types[j]
                    ? param_poly_types[j] : &params[j]->type;
                if (fn_type_mentions_named(jt, rn)) occ++;
            }
            if (!declared && occ < 2) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "unsupported return type keyword '%s': it is not a "
                          "built-in type and is not bound by any parameter; "
                          "declare it (e.g. `[%s]` type params) or annotate a "
                          "parameter with it to use it as a type variable",
                          rn, rn);
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
    }

    /* KB-025: record this function's signature type variables (params + return)
     * so a GADT match arm can distinguish a quantified-`a` result from a skolem
     * that escapes.  Accumulated on top of any enclosing function's set. */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    for (uint8_t i = 0; i < n_params; i++) {
        if (param_poly_types[i]) fn_collect_sig_tyvars(e, param_poly_types[i]);
        else                     fn_collect_sig_tyvars(e, &params[i]->type);
    }
    fn_collect_sig_tyvars(e, return_tyvar_type);
    fn_collect_sig_tyvars(e, return_app_type);
    fn_collect_sig_tyvars(e, return_fn_type);
    fn_collect_sig_tyvars(e, return_exists_type);

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    e->fn_body_depth++;
    /* Phase R6: Track current function name for linting */
    e->current_fn_name = name_f->as.sym;
    if (fn_declared_unsafe) e->unsafe_depth++;
    {
        /* Internal defines: splice (define name init) into nested let forms. */
        Form *spliced = splice_internal_defines(e,
                            &call->as.list.items[body_start], n_body, call->span);
        if (spliced) {
            body = elab_form(e, spliced);
            if (!body) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->current_fn_name = NULL;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        } else if (n_body == 1) {
            body = elab_form(e, call->as.list.items[body_start]);
            if (!body) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                /* Phase R6: Reset current function name */
                e->current_fn_name = NULL;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
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
                    e->n_sig_tyvars = saved_n_sig_tyvars;
                    /* Phase R6: Reset current function name */
                    e->current_fn_name = NULL;
                    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
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
    }
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    e->n_sig_tyvars = saved_n_sig_tyvars;
    /* Phase R6: Reset current function name */
    e->current_fn_name = NULL;
    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;

    /* TY2.2: return-position widening to `any`.  A function declared `: any`
     * whose body yields a narrower type must box the result, otherwise the
     * raw value leaks into a tur_tagged_t slot and breaks C codegen.  Mirror
     * the call-argument widening via the shared coercion helper. */
    if (return_kind == TY_ANY && body && body->type.kind != TY_ANY &&
        body->type.kind != TY_NEVER) {
        body = elab_coerce_to_any(e, body);
    }

    /* TY4: reject returning a borrow of a function-local (would dangle).  The
     * inner scope is still current here; binding depths were stamped at
     * creation, so the check only reads the elaborated body. */
    if (!check_no_borrow_escape(body, fn_local_depth, name_f->as.sym)) {
        e->scope = inner.parent;
        scope_free(&inner);
        return NULL;
    }

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
        /* SS7: propagate full TY_ROLE type from body so callers see the correct
         * current_step (the step after the body's last session operation). */
        if (g_sessions_enabled && body->type.kind == TY_ROLE && !return_session_type) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_session_type = rft;
        }
        /* Issue 1b: propagate full TY_FN type from body so callers see the
         * complete function signature (arity, result_kind) rather than a zeroed
         * TY_FN shell from type_from_kind(TY_FN). */
        if (body->type.kind == TY_FN && !return_fn_type) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_fn_type = rft;
        }
    }

    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);
    /* AR6: mark variadic functions in their type */
    fn_type.as.fn.is_variadic = is_variadic;
    fn_type.as.fn.rest_kind   = rest_kind;
    fn_type.as.fn.rest_full_type = rest_full_type;

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
    if (return_tyvar_type) {
        fn_type.as.fn.result_full_type = return_tyvar_type;
    }
    /* Issue 1b: attach full TY_FN return type so callers see the complete
     * function signature (arity, result_kind) rather than a zeroed TY_FN
     * shell.  Only set if not already filled by a more specific path (e.g.
     * ADT, struct, session, TY_APP, TY_EXISTS). */
    if (return_fn_type && !fn_type.as.fn.result_full_type) {
        fn_type.as.fn.result_full_type = return_fn_type;
    }
    /* PTC4: attach full TY_APP return type so call sites can extract concrete elem types. */
    if (return_app_type) {
        fn_type.as.fn.result_full_type = return_app_type;
    }
    /* F1-1: attach full TY_EXISTS / TY_FORALL return type.  Mirrors the
     * ADT/struct/session/TY_APP paths above; consumed by elab_call.c to
     * patch the call expression's type. */
    if (return_exists_type) {
        fn_type.as.fn.result_full_type = return_exists_type;
    }
    /* LS2: attach the full borrow return type so a call site's result carries
     * the borrow target (and lifetime), letting @ deref recover the pointee
     * type instead of falling back to an unknown/void C type. */
    if (return_borrow_type && !fn_type.as.fn.result_full_type) {
        fn_type.as.fn.result_full_type = return_borrow_type;
    }
    /* LS4: precompute which parameter the borrow return is tied to so call
     * sites can check inter-procedural borrow escape.  A returned &'a T aliases
     * the argument bound to the param sharing lifetime 'a; an elided borrow
     * return follows the elision rules (the receiver-style first borrow param,
     * which also covers the single-borrow-param case). */
    if (return_borrow_type) {
        int8_t tied = -1;
        LifetimeId rlid = (return_borrow_type->n_lifetimes > 0)
                        ? return_borrow_type->lifetimes[0] : LIFETIME_NONE;
        if (rlid != LIFETIME_NONE) {
            for (uint8_t i = 0; i < n_params; i++) {
                if ((params[i]->type.kind == TY_REF_IMMUT
                     || params[i]->type.kind == TY_REF_MUT)
                        && params[i]->type.n_lifetimes > 0
                        && params[i]->type.lifetimes[0] == rlid) {
                    tied = (int8_t)i;
                    break;
                }
            }
        }
        if (tied < 0) {
            for (uint8_t i = 0; i < n_params; i++) {
                if (params[i]->type.kind == TY_REF_IMMUT
                        || params[i]->type.kind == TY_REF_MUT) {
                    tied = (int8_t)i;
                    break;
                }
            }
        }
        fn_type.as.fn.result_borrow_arg = tied;
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
    /* A#1: Store arg_fat flags from param bindings into fn_type so call sites
     * can auto-shim bare fn arguments into fat closures. */
    {
        bool any_fat = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_fat) { any_fat = true; break; }
        }
        if (any_fat) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_fat[i] = params[i]->is_fat;
            }
        }
    }

    /* Create/update binding for the function.
     * Reuse pass-1 forward bindings in place so subsequent lookups observe
     * updated arity/types from the real definition. */
    Binding *b = NULL;
    /* CC2: Only reuse an existing global binding as a forward
     * declaration/redefinition when it belongs to the *same* module being
     * defined into (or to no module yet -- e.g. a pre-module stdlib forward
     * decl).  In whole-program mode every module elaborates into the single
     * shared global scope, so a same-named *private* defn from a *different*
     * module would otherwise be reused here, collapsing two distinct functions
     * onto one mangled C symbol (and stamping it with whichever module came
     * last).  Creating a fresh binding instead gives each module's private its
     * own distinctly-mangled C name. */
    if (existing && existing->type.kind == TY_FN && existing->is_global &&
        (existing->defining_module_name == NULL ||
         existing->defining_module_name == e->current_module_name)) {
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
    b->returns_closure_fn_binding = expr_closure_fn_binding(body);
    /* Phase M6: Store ^:export-as C name on the binding */
    b->c_export_name = c_export_name;
    /* F4: Store ^deprecated attribute on the binding */
    b->is_deprecated = is_deprecated_attr;
    b->deprecation_message = deprecation_msg;

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = is_variadic;  /* AR5: propagate variadic flag */
    if (is_variadic) {
        extern bool g_has_variadics;
        g_has_variadics = true;  /* AR8: tell emit_module to include __tur_cons_of */
    }
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
        } else if ((param_kinds[i] == TY_REF_IMMUT || param_kinds[i] == TY_REF_MUT)
                   && (params[i]->type.kind == TY_REF_IMMUT
                       || params[i]->type.kind == TY_REF_MUT)) {
            /* LS1: preserve borrow target + lifetime IDs so the lifetime pass and
             * borrow checker see the programmer's &'a T annotation, not a
             * lifetime-stripped type_from_kind() shell. */
            fd->param_types[i] = params[i]->type;
        } else {
            fd->param_types[i] = type_from_kind(param_kinds[i]);
        }
    }
    /* LS1: carry the interned signature lifetimes into the FnDef so the always-on
     * lifetime pass (borrow_check.c) can solve over the programmer's lifetimes. */
    fd->lifetime_ctx = sig_ltctx;
    /* LS2: record the full declared return Type so borrow lifetimes survive.
     * For a borrow return we have the parsed Type (with lifetime IDs); otherwise
     * the bare kind is sufficient for the lifetime pass. */
    fd->return_type = return_borrow_type ? *return_borrow_type
                                         : type_from_kind(return_kind);
    /* Phase 15: Store collected constraints */
    fd->constraints.constraints = constraint_list;
    fd->constraints.n_constraints = n_constraints;
    fd->constraints.cap_constraints = n_constraints;

    /* AR9: variadic defn may not have an inline-C body (fixed C signatures only) */
    if (is_variadic && body && body->kind == EX_INLINE_C) {
        diag_emit(DIAG_ERROR, body->span,
                  "defn '%s': variadic body contains inline-C; "
                  "inline-C blocks need a fixed arity signature",
                  name_f->as.sym->name);
        return NULL;
    }

    /* Phase C: warn (or error under --Werror=inline-c-narrow-params) when a
     * narrow-width parameter reaches an inline-C body.  The C code sees the
     * parameter at its narrow C type (e.g. int16_t), not int64_t, which
     * surprises callers that wrote the body expecting the carrier width. */
    if (body && body->kind == EX_INLINE_C) {
        for (uint8_t _ci = 0; _ci < n_params; _ci++) {
            TypeKind k = param_kinds[_ci];
            bool is_narrow = (k == TY_INT8  || k == TY_INT16  || k == TY_INT32 ||
                              k == TY_UINT8 || k == TY_UINT16 || k == TY_UINT32 ||
                              k == TY_FLOAT32);
            if (!is_narrow) continue;
            DiagLevel sev = g_werror_inline_c_narrow_params ? DIAG_ERROR : DIAG_WARNING;
            diag_emit_with_code(sev, body->span, TUR_W0037_INLINE_C_NARROW_PARAM,
                "defn '%s': parameter '%s' has narrow type %s -- "
                "inline-C sees %s, not int64_t; add explicit casts if needed",
                name_f->as.sym->name,
                params[_ci]->name->name,
                type_name(type_from_kind(k)),
                type_c_name(type_from_kind(k)));
            if (g_werror_inline_c_narrow_params) return NULL;
        }
    }

    Expr *out = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    out->as.fn_def_.fn = fd;

    /* Nested defn: if not at file scope, register for file-scope emission
     * and return nil — the function is lifted to file scope, callable by
     * its name from this point on via the global binding. */
    if (e->scope != &e->global) {
        elab_register_file_def(e, out);
        return e_nil(e, call->span);
    }
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

    const Symbol *fn_type_params[8];
    Kind fn_type_param_kinds[8];
    uint8_t n_fn_type_params = 0;
    uint8_t n_implicit_fn_type_params = 0;
    memset(fn_type_params, 0, sizeof(fn_type_params));
    for (uint8_t i = 0; i < 8; i++) fn_type_param_kinds[i] = KIND_STAR;

    uint32_t params_idx = 1;
    if (call->as.list.len > 3 &&
        call->as.list.items[1]->tag == F_VEC &&
        call->as.list.items[2]->tag == F_VEC) {
        Form *type_params_f = call->as.list.items[1];
        if (type_params_f->as.list.len > 8) {
            diag_emit(DIAG_ERROR, type_params_f->span,
                      "fn: too many type parameters (max 8)");
            return NULL;
        }
        n_fn_type_params = (uint8_t)type_params_f->as.list.len;
        for (uint8_t i = 0; i < n_fn_type_params; i++) {
            Form *tp = type_params_f->as.list.items[i];
            if (tp->tag != F_SYM) {
                diag_emit(DIAG_ERROR, tp->span,
                          "fn: type parameter must be a symbol");
                return NULL;
            }
            fn_type_params[i] = tp->as.sym;
            fn_type_param_kinds[i] = KIND_STAR;
        }
        params_idx = 2;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[params_idx];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "fn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    if (n_fn_type_params == 0) {
        const Form *implicit_ret_f = fn_return_annotation_form(call, params_idx + 1);
        n_implicit_fn_type_params = collect_implicit_fn_type_params(params_f, implicit_ret_f,
                                                                    fn_type_params, fn_type_param_kinds);
        n_fn_type_params = n_implicit_fn_type_params;
    }

    /* Parse params */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];
    Type *param_full_types[MAX_FN_ARITY];
    for (uint8_t _i = 0; _i < MAX_FN_ARITY; _i++) param_full_types[_i] = NULL;
    /* AR5: variadic rest parameter state for fn */
    bool fn_is_variadic = false;
    TypeKind fn_rest_kind = TY_INT;
    Type *fn_rest_full_type = NULL;  /* typed-variadic: full Type for user-defined rest */

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (i < n_implicit_fn_type_params && p->tag == F_SYM &&
            fn_type_params[i] == p->as.sym) {
            continue;
        }
        /* AR5: & rest-name :type -- variadic rest parameter for fn */
        if (p->tag == F_SYM && p->as.sym == e->sym_borrow) {
            if (fn_is_variadic) {
                diag_emit(DIAG_ERROR, p->span, "fn: multiple '&' in parameter list");
                return NULL;
            }
            if (i + 1 >= params_f->as.list.len) {
                diag_emit(DIAG_ERROR, p->span,
                          "fn: '&' must be followed by a rest parameter name");
                return NULL;
            }
            Form *rest_p = params_f->as.list.items[i + 1];
            if (rest_p->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_p->span,
                          "fn: rest parameter name must be a symbol");
                return NULL;
            }
            fn_rest_kind = TY_INT;
            fn_rest_full_type = NULL;
            if (i + 2 < params_f->as.list.len) {
                Form *type_p = params_f->as.list.items[i + 2];
                if (type_p->tag == F_KEYWORD) {
                    if (!resolve_variadic_rest_type(e, type_p,
                                                    fn_type_params, fn_type_param_kinds,
                                                    n_fn_type_params, "fn",
                                                    &fn_rest_kind, &fn_rest_full_type)) {
                        return NULL;
                    }
                    if (i + 3 < params_f->as.list.len) {
                        diag_emit(DIAG_ERROR, params_f->as.list.items[i + 3]->span,
                                  "fn: no parameters allowed after '& rest :type'");
                        return NULL;
                    }
                } else {
                    diag_emit(DIAG_ERROR, type_p->span,
                              "fn: '& rest' must be followed by a type annotation (e.g. :int)");
                    return NULL;
                }
            }
            fn_is_variadic = true;
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
            }
            param_kinds[n_params] = TY_INT;
            Binding *rest_b = binding_new(e, rest_p->as.sym, TYPE_INT, false, false, rest_p->span);
            rest_b->is_param = true;
            params[n_params++] = rest_b;
            break;
        }
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN || p->tag == F_LIST || p->tag == F_VEC) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "fn: type annotation without preceding parameter");
                return NULL;
            }
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            Type *ann = fn_type_from_form(e, type_form,
                                          fn_type_params, fn_type_param_kinds, n_fn_type_params);
            if (!ann) return NULL;
            param_kinds[n_params - 1] = ann->kind;
            params[n_params - 1]->type = *ann;
            if (ann->kind == TY_APP || ann->kind == TY_TYVAR || ann->kind == TY_FN ||
                fn_type_has_named_tyvar(ann)) {
                param_full_types[n_params - 1] = ann;
            }
            continue;
        }
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: parameter name must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: too many parameters (max %d)", MAX_FN_ARITY);
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* Untyped fn params preserve the existing int default. */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        b->is_param = true;
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    Type *return_full_type = NULL;
    Type *return_fn_type = NULL; /* Preserve full TY_FN returns for higher-order calls. */
    uint32_t body_start = params_idx + 1;

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type. */
    EffectRow *declared_effect_row_fn = NULL;
    if (call->as.list.len >= params_idx + 2) {
        Form *maybe_row = call->as.list.items[params_idx + 1];
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
            body_start = params_idx + 2;
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
            uint8_t type_param_idx = 0;
            if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
                return_kind = TY_TYVAR;
                return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                *return_full_type = type_tyvar_named(kw->name);
                return_full_type->hkt_kind = fn_type_param_kinds[type_param_idx];
            } else if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
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
                return_kind = TY_TYVAR;
                return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                *return_full_type = type_tyvar_named(kw->name);
            }
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = fn_type_from_form(e, ret_f->as.list.items[0],
                                              fn_type_params, fn_type_param_kinds, n_fn_type_params);
                if (ann) {
                    return_kind = ann->kind;
                    if (ann->kind == TY_TYVAR || fn_type_has_named_tyvar(ann)) {
                        return_full_type = ann;
                    }
                    if (ann->kind == TY_FN) {
                        return_fn_type = ann;
                    }
                }
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
    /* CF7.3: record the scope just before this lambda's inner scope is pushed. */
    struct Scope *saved_fn_entry_outer_scope = e->fn_entry_outer_scope;
    e->fn_entry_outer_scope = e->scope;
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    /* KB-025: accumulate this closure's signature type variables on top of the
     * enclosing function's set (see elab_defn for the rationale). */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    for (uint8_t i = 0; i < n_params; i++) {
        fn_collect_sig_tyvars(e, &params[i]->type);
    }
    fn_collect_sig_tyvars(e, return_full_type);
    fn_collect_sig_tyvars(e, return_fn_type);

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;
    e->fn_body_depth++;
    if (fn_declared_unsafe) e->unsafe_depth++;
    {
        /* Internal defines: splice (define name init) into nested let forms. */
        Form *spliced = splice_internal_defines(e,
                            &call->as.list.items[body_start], n_body, call->span);
        if (spliced) {
            body = elab_form(e, spliced);
            if (!body) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        } else if (n_body == 1) {
            body = elab_form(e, call->as.list.items[body_start]);
            if (!body) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
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
                    e->n_sig_tyvars = saved_n_sig_tyvars;
                    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                    e->scope = inner.parent;
                    scope_free(&inner);
                    return NULL;
                }
            }
            body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = n_body;
        }
    }
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    e->n_sig_tyvars = saved_n_sig_tyvars;
    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;

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
        if (body->type.kind == TY_FN) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_fn_type = rft;
        }
    }
    
    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);
    /* AR6: mark variadic functions in their type */
    fn_type.as.fn.is_variadic = fn_is_variadic;
    fn_type.as.fn.rest_kind   = fn_rest_kind;
    fn_type.as.fn.rest_full_type = fn_rest_full_type;
    {
        bool any_full = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (param_full_types[i]) { any_full = true; break; }
        }
        if (any_full) {
            Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint8_t i = 0; i < n_params; i++) aFT[i] = param_full_types[i];
            fn_type.as.fn.arg_full_types = aFT;
        }
    }
    if (return_full_type) {
        fn_type.as.fn.result_full_type = return_full_type;
    }
    if (return_fn_type) {
        fn_type.as.fn.result_full_type = return_fn_type;
    }

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
    b->returns_closure_fn_binding = expr_closure_fn_binding(body);

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = fn_is_variadic;  /* AR5: propagate variadic flag */
    if (fn_is_variadic) {
        extern bool g_has_variadics;
        g_has_variadics = true;  /* AR8: tell emit_module to include __tur_cons_of */
    }
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_fn) {
        b->type.as.fn.effect_row = declared_effect_row_fn;
    }
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        fd->param_types[i] = params[i]->type;
    }
    /* Phase 15: Initialize constraints */
    constraint_set_init(&fd->constraints);
    /* LS2: lambdas carry no surface borrow-return lifetimes; record the bare
     * return kind and an empty lifetime context so the lifetime pass reads a
     * defined (not garbage) return Type. */
    lifetime_context_init(&fd->lifetime_ctx);
    fd->return_type = type_from_kind(return_kind);

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
            new_param_types[i + 1] = params[i]->type;
        }
        
        /* Update FnDef with new params */
        fd->params = new_params;
        fd->n_params = new_n_params;
        fd->param_types = new_param_types;
        
        /* Update function type to include env parameter */
        TypeKind new_arg_kinds[MAX_FN_ARITY];
        new_arg_kinds[0] = TY_PTR_VOID;  /* env parameter */
        for (uint8_t i = 0; i < n_params; i++) {
            new_arg_kinds[i + 1] = param_kinds[i];
        }
        Type new_fn_type = type_fn(new_arg_kinds, new_n_params, return_kind);
        if (b->type.as.fn.arg_full_types) {
            Type **shifted = (Type **)arena_alloc(e->arena, new_n_params * sizeof(Type *));
            shifted[0] = NULL;
            for (uint8_t i = 0; i < n_params; i++) shifted[i + 1] = b->type.as.fn.arg_full_types[i];
            new_fn_type.as.fn.arg_full_types = shifted;
        }
        if (b->type.as.fn.result_full_type) {
            new_fn_type.as.fn.result_full_type = b->type.as.fn.result_full_type;
        }
        if (return_fn_type) {
            new_fn_type.as.fn.result_full_type = return_fn_type;
        }
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
        /* Copy captures into arena memory so it shares the closure's lifetime. */
        Binding **arena_captures = (Binding **)arena_alloc(e->arena, n_captures * sizeof(Binding *));
        memcpy(arena_captures, captures, n_captures * sizeof(Binding *));
        closure->captures = arena_captures;
        closure->n_captures = n_captures;
        closure->env_name = env_name_sym;
        
        /* Store closure reference in FnDef for codegen */
        fd->closure = closure;
        
        /* Create EX_CLOSURE expression */
        /* The closure's type is void* (pointer to closure struct) */
        Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, TYPE_PTR_VOID, call->span);
        closure_expr->as.closure_.closure = closure;

        free(captures);
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
    /* A#1: ^fat marks the next extern-c parameter as a fat-closure consumer. */
    bool next_param_fat = false;

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        /* A#1: ^fat annotation applies to the next parameter.  Intercept before
         * the generic ^ctype handling below (which would misparse "fat"). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_fat) {
            next_param_fat = true;
            continue;
        }
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
                b->is_param = true;
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
            b->is_param = true;
            b->is_fat = next_param_fat; next_param_fat = false;
            params[n_params++] = b;
            continue;
        }

        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        b->is_param = true;
        b->is_fat = next_param_fat; next_param_fat = false;
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
    /* A#1: propagate ^fat parameter flags into the fn type for call-site shimming. */
    for (uint8_t i = 0; i < n_params; i++) {
        if (params[i]->is_fat) fn_type.as.fn.arg_fat[i] = true;
    }

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
    bool is_deprecated_attr = false;
    const char *deprecation_msg = NULL;

    if (call->as.list.len > 3) {
        Form *first = call->as.list.items[name_idx];
        if (first->tag == F_SYM && first->as.sym == e->sym_caret_persistent) {
            is_persistent = true;
            name_idx++;
        }
    }

    /* F4: ^deprecated ["message"] before the name (after ^persistent) */
    if (call->as.list.len > name_idx + 2 &&
        call->as.list.items[name_idx]->tag == F_SYM &&
        call->as.list.items[name_idx]->as.sym == e->sym_caret_deprecated) {
        is_deprecated_attr = true;
        name_idx++;
        if (call->as.list.items[name_idx]->tag == F_STR) {
            Form *msg_f = call->as.list.items[name_idx];
            char *msg_buf = (char *)arena_alloc(e->arena, msg_f->as.s.len + 1);
            memcpy(msg_buf, msg_f->as.s.p, msg_f->as.s.len);
            msg_buf[msg_f->as.s.len] = '\0';
            deprecation_msg = msg_buf;
            name_idx++;
        }
    }

    if (name_idx + 2 != call->as.list.len) {
        diag_emit(DIAG_ERROR, call->span,
                  "def takes (def [^persistent] [^deprecated [\"msg\"]] name init)");
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
    /* F4: ^deprecated on def */
    b->is_deprecated = is_deprecated_attr;
    b->deprecation_message = deprecation_msg;
    scope_add(&e->global, b);

    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = init;
    out->as.def_.struct_def = NULL;
    return out;
}
