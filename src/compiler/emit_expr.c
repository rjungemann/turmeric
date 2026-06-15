/* emit_expr.c -- expression-position C emission (emit_value and friends). */
#include "emit_internal.h"
#include "emit_cps.h"   /* cps-transform-plan: EX_CALLCC lowering */

/* ACB: true when kind represents a concrete aggregate type (struct, ADT, or
 * type-application) that the carrier ABI stores as a heap pointer.  Used by
 * KB-004 and KB-010 bridge insertion sites. */
static bool type_kind_is_aggregate(TypeKind k) {
    return k == TY_STRUCT || k == TY_ADT || k == TY_APP;
}

/* KB-021/KB-031: a type's dictionary-dispatch instance body uses the carrier
 * ABI (int64_t parameter) exactly when its value representation is the carrier
 * (see type_uses_carrier_abi in emit_core.c).  Both the value-declaration path
 * and these dispatch callsites consult the single shared predicate so they can
 * never disagree about whether an argument is already a carrier. */
static bool type_uses_carrier_in_dispatch(Type t) {
    return type_uses_carrier_abi(t);
}

/* KB-004/KB-021: find the ABI specialization (concrete-by-value clone) that an
 * EX_CALL resolves to, or NULL.  Factored out of the EX_CALL emit path so the
 * let-binding type can discover that a call returns a concrete struct by value
 * (e.g. a `tuple2`/`ok`/`thead` clone) rather than the int64_t carrier. */
static const EmitAbiSpecialization *find_matched_abi_spec(
        EmitCtx *ctx, const Expr *e, const Binding *fn_binding) {
    if (!ctx || !e || e->kind != EX_CALL) return NULL;
    /* M5 emit-side: when emit_module.c interned a spec for this exact call
     * Expr* (recorded in specialized_call_exprs / specialized_call_names),
     * emit_call_name returns that spec's clone_name.  Look up the spec by
     * clone_name so this function agrees with emit_call_name about which
     * ABI is in effect.  Without this, the type-match loop below tries
     * to compare the spec's *resolved* arg types (e.g. `Vec__int`) against
     * the IR call's *abstract* arg types (e.g. `(Vec A)` where A is the
     * outer spec's tyvar), type_eq fails, matched_spec is NULL, and the
     * dict_arg arg-bridge at emit_expr.c:2587 fires -- bridging args to
     * int64 even though the resolved callee takes them by value (cc error). */
    /* M5 Finding 7: the same source-body call Expr* is recorded once per outer
     * spec (one entry per element type for a shared instance-method body).
     * Prefer the entry whose recorded outer spec matches the CURRENT active
     * spec, so e.g. the `Eq Vec` spec for `bool` resolves the inner
     * `vec-len-byval` call to its `Vec__bool` clone instead of the
     * first-recorded `Vec__int` one.  Fall back to the first entry for this
     * call when no outer-matched entry exists (top-level / single-spec case,
     * preserving prior behaviour). */
    const char *active_outer = ctx->current_abi_specialization
        ? ctx->current_abi_specialization->clone_name : NULL;
    const char *fallback_clone = NULL;
    bool saw_call = false;
    for (uint32_t i = 0; i < ctx->n_specialized_calls; i++) {
        if (ctx->specialized_call_exprs[i] != e) continue;
        if (!saw_call) { fallback_clone = ctx->specialized_call_names[i]; saw_call = true; }
        if (ctx->specialized_call_outer[i] == active_outer) {
            fallback_clone = ctx->specialized_call_names[i];
            break;
        }
    }
    if (saw_call) {
        for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
            const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
            if (spec->clone_name && fallback_clone &&
                strcmp(spec->clone_name, fallback_clone) == 0) {
                return spec;
            }
        }
    }
    for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
        if (spec->binding != fn_binding || spec->n_args != e->as.call_.n_args) continue;
        bool args_match = true;
        for (uint32_t ai = 0; ai < e->as.call_.n_args; ai++) {
            const Expr *cur = e->as.call_.args[ai];
            /* ACB (KB-004): when the arg is EX_ASCRIBE, match against the
             * ascribed (concrete) type rather than the carrier inner. */
            Type actual;
            if (cur && cur->kind == EX_ASCRIBE) {
                actual = cur->type;
            } else {
                while (cur && cur->kind == EX_ASCRIBE) cur = cur->as.ascribe_.inner;
                actual = (cur && cur->kind == EX_REINTERPRET && cur->as.reinterpret_.expr)
                    ? cur->as.reinterpret_.expr->type
                    : (cur ? cur->type : emit_type_from_kind(TY_UNKNOWN));
            }
            if (!type_eq(spec->arg_types[ai], actual)) { args_match = false; break; }
        }
        if (args_match) return spec;
    }
    return NULL;
}

/* fat-closure-dispatch-aggregate-return: true when a direct call returns a
 * carrier-ABI aggregate *by value* rather than as the int64_t carrier.  A
 * non-generic defn whose declared result is a concrete carrier-ABI aggregate
 * (e.g. (Pair A B), (Vec int)) is emitted by emit_fns.c with a by-value C
 * return type (it threads result_full_type through and the body is not
 * inline-C).  Such a result must be bound and consumed by value -- declaring
 * the binding as int64_t would fail to type-check against the struct the call
 * returns.  Results carried as a tyvar (generic) or returned from an inline-C
 * body still come back as the int64_t carrier and are excluded here. */
static bool call_returns_byvalue_aggregate(EmitCtx *ctx, const Expr *call) {
    if (!call || call->kind != EX_CALL) return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN || fb->body_is_inline_c) return false;
    const Type *rft = fb->type.as.fn.result_full_type;
    if (!rft) return false;
    Type r = emit_resolve_type(ctx, *rft);
    if (!type_uses_carrier_abi(r)) return false;
    return strcmp(emit_type_c_name(ctx, r), "int64_t") != 0;
}

/* KB-021: the C type of the value that `emit_value(e)` produces, used to declare
 * a let binding so its declared type matches its initialiser's representation.
 *
 * Carrier-ABI types have two coexisting C representations in this codebase
 * (the stdlib author chooses per-operation): the int64_t carrier (heap-pointer
 * handle, e.g. (vec-new), (some x)) and a by-value concrete struct (a struct
 * constructor literal, or an ABI-specialized clone that returns the concrete
 * type, e.g. (tuple2 a b)).  A binding must be declared with whichever its
 * initialiser yields, or the C initialiser fails to type-check.  Non-carrier
 * types have a single representation, so they fall through to emit_type_c_name. */
static const char *emit_binding_repr_c_name(EmitCtx *ctx, Type binding_ty,
                                            const Expr *init) {
    if (!type_uses_carrier_abi(emit_resolve_type(ctx, binding_ty)))
        return emit_type_c_name(ctx, binding_ty);

    const Expr *p = init;
    /* After KB-021 an ascription emits its inner value unchanged for carrier-ABI
     * targets, so the representation is that of the inner expression. */
    while (p && p->kind == EX_ASCRIBE) p = p->as.ascribe_.inner;
    if (!p) return emit_type_c_name(ctx, binding_ty);

    /* M4 follow-up: when the init is an EX_ASCRIBE that casts a PLAIN
     * TY_INT (not a carrier-returning call) to a TY_APP whose spine
     * resolves to a concrete struct (e.g. `(:: t (Cons int))` where t is
     * a raw int param), the ascription bridge at `emit_expr.c:4240`
     * dereferences the int handle to a by-value struct.  Declare the
     * binding with the target's by-value C type so the bridge's output
     * type-checks against the binding's declaration.
     *
     * Narrow gate: only fire when p (the producer past EX_ASCRIBE
     * unwrapping) is itself a bare TY_INT value (literal or non-carrier
     * EX_VAR).  The carrier-relabel case (`(:: (vec-of) (Vec int))`)
     * has p as an EX_CALL whose spec returns a carrier; that path falls
     * through to the existing carrier handling below. */
    if (init && init->kind == EX_ASCRIBE
        && init->as.ascribe_.inner->type.kind == TY_INT
        && binding_ty.kind == TY_APP
        && (p->kind == EX_INT_LIT
            || (p->kind == EX_VAR && p->as.var.binding
                && !p->as.var.binding->emit_byvalue_carrier_abi
                && p->type.kind == TY_INT))) {
        Type resolved = emit_resolve_type(ctx, binding_ty);
        StructDef *rd = NULL;
        Type rargs[16]; uint8_t rn = 0;
        if (type_extract_struct_app(&resolved, &rd, rargs, &rn)
            && rd && !rd->is_opaque) {
            return emit_type_c_name(ctx, resolved);
        }
    }

    /* The carrier (int64_t) cases -- declare int64_t so a carrier initialiser
     * type-checks and downstream carrier-ABI uses (vec-push!, dispatch) agree:
     *   - a carrier-returning call (not resolved to a concrete-by-value spec);
     *   - a reference to a var that is itself a carrier. */
    if (p->kind == EX_CALL) {
        const EmitAbiSpecialization *spec =
            find_matched_abi_spec(ctx, p, p->as.call_.fn_binding);
        bool returns_concrete =
            spec && type_uses_carrier_abi(emit_resolve_type(ctx, spec->result_type));
        /* A concrete-aggregate-returning defn (no ABI spec) yields the struct
         * by value -- fall through to the by-value representation below. */
        if (!returns_concrete && !call_returns_byvalue_aggregate(ctx, p))
            return "int64_t";
    } else if (p->kind == EX_VAR) {
        if (!(p->as.var.binding && p->as.var.binding->emit_byvalue_carrier_abi))
            return "int64_t";
    }

    /* Everything else (struct constructor literal, concrete-spec call result,
     * by-value var, or a control-form result temp declared via emit_type_c_name)
     * keeps the by-value concrete representation. */
    return emit_type_c_name(ctx, binding_ty);
}

/* KB-021: true when emitting `e` yields a *by-value* concrete carrier-ABI
 * aggregate rather than the int64_t carrier.  Such a value must be bridged to
 * the carrier before a dictionary-dispatch / carrier-ABI call; an already-
 * carrier value must not.  By-value producers are: a struct constructor literal
 * (EX_MAKE_STRUCT), a var/param declared by-value (tracked on the binding), and
 * a call resolving to a concrete-by-value ABI specialization. */
static bool expr_emits_byvalue_carrier_abi(EmitCtx *ctx, const Expr *e) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    if (!e) return false;
    if (!type_uses_carrier_abi(e->type)) return false;
    if (e->kind == EX_MAKE_STRUCT) return true;
    if (e->kind == EX_VAR)
        return e->as.var.binding && e->as.var.binding->emit_byvalue_carrier_abi;
    if (e->kind == EX_CALL) {
        const EmitAbiSpecialization *spec =
            find_matched_abi_spec(ctx, e, e->as.call_.fn_binding);
        if (spec && type_uses_carrier_abi(emit_resolve_type(ctx, spec->result_type)))
            return true;
        return call_returns_byvalue_aggregate(ctx, e);
    }
    return false;
}

static bool reinterpret_kind_is_scalar(TypeKind kind) {
    switch (kind) {
        case TY_BOOL:
        case TY_INT:
        case TY_FLOAT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_SYM:        /* SYM3: interned symbol is a pointer-sized scalar */
        case TY_INT8:
        case TY_INT16:
        case TY_INT32:
        case TY_INT64:
        case TY_UINT8:
        case TY_UINT16:
        case TY_UINT32:
        case TY_UINT64:
        case TY_FLOAT32:
        case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

static int reinterpret_kind_size_bytes(TypeKind kind) {
    switch (kind) {
        case TY_BOOL:
        case TY_INT8:
        case TY_UINT8:
            return 1;
        case TY_INT16:
        case TY_UINT16:
            return 2;
        case TY_INT32:
        case TY_UINT32:
        case TY_FLOAT32:
            return 4;
        case TY_INT:
        case TY_FLOAT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_SYM:        /* SYM3: interned symbol pointer */
        case TY_INT64:
        case TY_UINT64:
        case TY_FLOAT64:
            return 8;
        default:
            return -1;
    }
}

/* Phase F: Returns true for sub-64-bit integer kinds eligible for concrete
 * typed poly dispatch on x86-64 SysV ABI.  These types are passed in the
 * low bits of a 64-bit integer register (zero-/sign-extended by the caller)
 * and returned in rax, so a call through a narrower function pointer type
 * still delivers the correct bit pattern after the callee's truncation --
 * for both native closure thunks and the generic int64_t-carrier wrapper.
 *
 * Signed and unsigned narrow ints share this property: the meaningful bits
 * occupy the low byte/half/word of the register regardless of the extension
 * discipline, and the concrete cast at the call site truncates back to the
 * declared width.  Full-width (u)int64 already coincides with the carrier,
 * so there is nothing to specialize.  Floats are deliberately excluded: the
 * generic carrier wrapper returns int64_t in rax, but a float-returning
 * signature reads xmm0, so the value would not survive the wrapper round
 * trip (a native float thunk would be fine, but the call site cannot always
 * tell the two apart). */
static bool type_kind_is_poly_concrete(TypeKind k) {
    return k == TY_BOOL ||
           k == TY_INT8  || k == TY_INT16  || k == TY_INT32 ||
           k == TY_UINT8 || k == TY_UINT16 || k == TY_UINT32;
}

static Binding *emit_expr_closure_fn_binding(const Expr *expr) {
    if (!expr) return NULL;

    switch (expr->kind) {
        case EX_ASCRIBE:
            return emit_expr_closure_fn_binding(expr->as.ascribe_.inner);
        case EX_CLOSURE:
            if (expr->as.closure_.closure && expr->as.closure_.closure->fn) {
                return expr->as.closure_.closure->fn->binding;
            }
            return NULL;
        case EX_VAR:
            if (!expr->as.var.binding) return NULL;
            if (expr->as.var.binding->closure_fn_binding) {
                return expr->as.var.binding->closure_fn_binding;
            }
            return expr->as.var.binding->returns_closure_fn_binding;
        case EX_CALL:
            if (!expr->as.call_.fn_binding) return NULL;
            return expr->as.call_.fn_binding->returns_closure_fn_binding;
        case EX_LET:
        case EX_LETREC:
            return emit_expr_closure_fn_binding(expr->as.let_.body);
        case EX_DO:
            for (int i = (int)expr->as.do_.n - 1; i >= 0; i--) {
                const Expr *item = expr->as.do_.items[i];
                if (item->kind == EX_DEFER) continue;
                return emit_expr_closure_fn_binding(item);
            }
            return NULL;
        case EX_IF: {
            Binding *then_binding = emit_expr_closure_fn_binding(expr->as.if_.then_);
            Binding *else_binding = expr->as.if_.else_or_null
                ? emit_expr_closure_fn_binding(expr->as.if_.else_or_null)
                : NULL;
            return (then_binding && then_binding == else_binding) ? then_binding : NULL;
        }
        default:
            return NULL;
    }
}

static Type emit_fn_result_type_from_type(Type fn_type) {
    if (fn_type.kind == TY_FN && fn_type.as.fn.result_full_type) {
        return *fn_type.as.fn.result_full_type;
    }
    return emit_type_from_kind(fn_type.kind == TY_FN ? fn_type.as.fn.result_kind : TY_NIL);
}

static Type emit_fn_arg_type_from_type(Type fn_type, uint8_t idx) {
    if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types && fn_type.as.fn.arg_full_types[idx]) {
        return *fn_type.as.fn.arg_full_types[idx];
    }
    return emit_type_from_kind(fn_type.kind == TY_FN ? fn_type.as.fn.arg_kinds[idx] : TY_UNKNOWN);
}

/* ------------ block-shaped emitters ------------ */

static void emit_temp_decl(EmitCtx *ctx, Buf *body, Type type, const char *name, const char *init_or_null) {
    type = emit_resolve_type(ctx, type);
    indent_buf(body, ctx->indent);
    /* CRU B-1: a boxed TY_FN (first-class closure value) is carried as a void *
     * holding the { thunk, env... } box -- declare it via the generic path
     * (emit_type_c_name -> "void *"), not as a thin function pointer, which
     * would mistype the box.  Only a *bare* TY_FN temp is a function pointer. */
    if (type.kind == TY_FN && !type.as.fn.boxed) {
        buf_printf(body, "%s (*%s)(",
                   type_c_name(emit_type_from_kind(type.as.fn.result_kind)), name);
        if (type.as.fn.arity == 0) {
            buf_puts(body, "void");
        } else {
            for (uint8_t i = 0; i < type.as.fn.arity; i++) {
                if (i > 0) buf_puts(body, ", ");
                buf_printf(body, "%s",
                           type_c_name(emit_type_from_kind(type.as.fn.arg_kinds[i])));
            }
        }
        buf_puts(body, ")");
        if (init_or_null) buf_printf(body, " = %s", init_or_null);
        buf_puts(body, ";\n");
        return;
    }

    buf_printf(body, "%s %s", emit_type_c_name(ctx, type), name);
    if (init_or_null) buf_printf(body, " = %s", init_or_null);
    buf_puts(body, ";\n");
}

/* Fat-closure-env scoped free (docs/reported/fat-closure-env-leak.md): decide
 * whether let-binding `idx` of `e` holds a freshly-constructed fat closure whose
 * heap env can be `free`d when the let scope exits.  Sound iff:
 *   - the initializer is an EX_CLOSURE (so the value is a `malloc`'d env), and
 *   - the closure returns a scalar value, so its result can never alias the env
 *     (no interior reference can outlive the free), and
 *   - the bound name does not escape -- it is used only as a direct-call callee
 *     within the let body and never escapes through a sibling binding's
 *     initializer (which could capture it into a longer-lived closure).
 * The conservative `closure_binding_escapes` check only ever greenlights a free,
 * so a false negative merely preserves the status-quo leak; it never frees a
 * still-live env. */
static bool let_binding_env_freeable(const Expr *e, uint32_t idx) {
    const Expr *init = e->as.let_.bindings[idx].init;
    const Binding *b = e->as.let_.bindings[idx].binding;
    if (!init || !b) return false;
    if (init->kind != EX_CLOSURE) return false;
    /* Scalar-result gate: a closure returning a reference/struct/pointer could
     * hand back a value derived from its env; restrict to scalar returns whose
     * result is copied out by value. */
    if (b->type.kind != TY_FN) return false;
    switch (b->type.as.fn.result_kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_NIL: break;
        default: return false;
    }
    if (closure_binding_escapes(e->as.let_.body, b)) return false;
    for (uint32_t j = 0; j < e->as.let_.n; j++) {
        if (j == idx) continue;
        if (closure_binding_escapes(e->as.let_.bindings[j].init, b)) return false;
    }
    return true;
}

static char *emit_let_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Phase 3/4: Check if body contains return or throw first */
    bool body_has_return_or_throw = expr_contains_return_or_throw(e->as.let_.body);
    
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    /* Phase R1: Also create temp if body has return/throw but let has non-nil type */
    if (!nil_result && !body_has_return_or_throw) {
        tmp = fresh_tmp(ctx);
        emit_temp_decl(ctx, body, e->type, tmp, NULL);
    } else if (!nil_result && body_has_return_or_throw) {
        /* Special case for ? operator: body may contain return but still produce a value */
        tmp = fresh_tmp(ctx);
        emit_temp_decl(ctx, body, e->type, tmp, NULL);
    }
    
    indent_buf(body, ctx->indent);
    buf_puts(body, "{\n");
    ctx->indent += 4;

    /* Fat-closure-env scoped free: collect the C names of let-bound,
     * non-escaping fat closures so their heap env can be released at scope
     * exit.  Only on the clean (non return/throw) paths -- an early return
     * would skip the trailing free, which is a leak, not a UAF. */
    char **env_free_names = NULL;
    uint32_t n_env_free = 0;
    if (!body_has_return_or_throw) {
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            if (let_binding_env_freeable(e, i)) {
                env_free_names = (char **)realloc(env_free_names,
                                                  (n_env_free + 1) * sizeof(char *));
                env_free_names[n_env_free++] =
                    name_for_binding(ctx, e->as.let_.bindings[i].binding);
            }
        }
    }

    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *b = e->as.let_.bindings[i].binding;
        char *bn = name_for_binding(ctx, b);
        char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
        indent_buf(body, ctx->indent);
        /* GF1: gen struct fields are already declared in the struct -- just assign */
        bool is_gen_field = false;
        if (ctx->gen_var_name && ctx->gen_struct_bindings) {
            for (uint32_t gi = 0; gi < ctx->n_gen_struct_bindings; gi++) {
                if (ctx->gen_struct_bindings[gi] == b) { is_gen_field = true; break; }
            }
        }
        if (is_gen_field) {
            buf_printf(body, "%s = %s;\n", bn, iv);
        } else if (b->type.kind == TY_FN && (b->is_fat || b->type.as.fn.boxed)) {
            /* closure-representation-unification (Phase 0): a fn-typed ^fat alias
             * holds a fat-closure box, not a bare function pointer.  CRU B-1: a
             * boxed TY_FN (a first-class closure value) is likewise a box, not a
             * thin fn pointer.  Declare either as the int64_t carrier so the
             * fat-dispatch call site (the ER2 is_fat/boxed path) casts it back to
             * void * and reads slot 0 -- declaring it as a thin fn pointer (the
             * TY_FN branch below) both mistypes the box and trips
             * -Wint-conversion.  A bare ^fat alias is :ptr<void> and is handled
             * cleanly by the fallback below. */
            buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n", bn, iv);
        } else if (b->type.kind == TY_FN
                   && (b->type.as.fn.result_kind == TY_FN
                       || b->type.as.fn.result_kind == TY_UNKNOWN)) {
            /* Closure-returning-instance-method codegen: a let-bound *curried*
             * closure -- the result of calling a method whose return type is a
             * function-returning-function (e.g. (.adder w) : (fn [:int] (fn
             * [:int] :int))) -- is a single fat-closure handle, not a thin
             * function pointer.  The thin-fn-pointer declaration below would
             * unwrap the result kind to an unknown-void return type and mistype
             * the handle; carry it as the int64_t handle instead, mirroring the
             * is_fat/boxed branch above. */
            buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n", bn, iv);
        } else if (b->type.kind == TY_FN) {
            /* For function pointer types, emit: <result> (*<name>)(<args...>) = <init>; */
            buf_printf(body, "%s (*%s)(",
                       type_c_name(emit_type_from_kind(b->type.as.fn.result_kind)), bn);
            for (uint8_t j = 0; j < b->type.as.fn.arity; j++) {
                if (j > 0) buf_puts(body, ", ");
                buf_printf(body, "%s",
                           type_c_name(emit_type_from_kind(b->type.as.fn.arg_kinds[j])));
            }
            buf_printf(body, ") = %s;\n", iv);
        } else if (b->is_poly_fn) {
            /* Phase HRT4: let-bound poly fn alias — declare as tur_poly_fn_t. */
            buf_printf(body, "tur_poly_fn_t %s = %s;\n", bn, iv);
        } else {
            /* KB-021: declare the binding with the C representation its
             * initialiser actually yields.  Carrier-ABI types have two C
             * representations (int64_t carrier vs by-value concrete struct);
             * picking the wrong one makes the C initialiser fail to type-check
             * (e.g. `int64_t v = (Vec__int){...}` or `Vec__int v = vec_new()`). */
            const char *bind_c = emit_binding_repr_c_name(ctx, b->type,
                                     e->as.let_.bindings[i].init);
            /* KB-021: record whether this binding ended up by-value so that a
             * later dictionary-dispatch use of the var bridges it to the carrier. */
            if (e->as.let_.bindings[i].binding)
                e->as.let_.bindings[i].binding->emit_byvalue_carrier_abi =
                    type_uses_carrier_abi(emit_resolve_type(ctx, b->type)) &&
                    strcmp(bind_c, "int64_t") != 0;
            /* CC1 (curried-call-cast-rough-edges-plan): when the binding is
             * declared as an int64_t carrier but the init expression yields a
             * function pointer or void * (e.g. a PAP wrapper capturing a bare
             * top-level defn into its env), wrap the init with the standard
             * (int64_t)(intptr_t) coercion -- otherwise clang rejects the
             * implicit pointer-to-int conversion under -Wint-conversion. */
            TypeKind init_kind = e->as.let_.bindings[i].init->type.kind;
            if (strcmp(bind_c, "int64_t") == 0 &&
                (init_kind == TY_FN || init_kind == TY_PTR_VOID)) {
                buf_printf(body, "%s %s = (int64_t)(intptr_t)(%s);\n", bind_c, bn, iv);
            } else {
                buf_printf(body, "%s %s = %s;\n", bind_c, bn, iv);
            }
        }
        /* Suppress unused-variable warnings even if the body never refs it. */
        indent_buf(body, ctx->indent);
        buf_printf(body, "(void)%s;\n", bn);
        free(bn);
        free(iv);
    }

    if (body_has_return_or_throw) {
        /* Body contains return/throw but may still produce a value on the
         * non-diverging path (e.g. the `?` operator: an if whose then-branch
         * returns and whose else-branch yields the unwrapped ok value).
         *
         * When the let has a non-nil result type AND the body itself can
         * yield a value (i.e. is not fully divergent), evaluate it as a
         * value expression and assign the inner result to `tmp`.  Fully
         * divergent bodies (e.g. `(do (defer ...) (return 42))`) produce
         * `(void)0` from emit_value; in that case we emit as a statement
         * and leave `tmp` uninitialised — the divergent path never reads
         * it. */
        if (!nil_result && !expr_is_divergent(e->as.let_.body)) {
            char *bv = emit_value(ctx, body, e->as.let_.body);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, bv);
            free(bv);
        } else {
            emit_stmt(ctx, body, e->as.let_.body);
        }
        /* Close scope */
        ctx->indent -= 4;
        indent_buf(body, ctx->indent);
        buf_puts(body, "}\n");
        return tmp;
    }
    
    if (nil_result) {
        emit_stmt(ctx, body, e->as.let_.body);
    } else {
        char *bv = emit_value(ctx, body, e->as.let_.body);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", tmp, bv);
        free(bv);
    }

    /* Fat-closure-env scoped free: release non-escaping closure envs now that
     * the let body (their last use) has been emitted. */
    for (uint32_t i = 0; i < n_env_free; i++) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "free((void *)(intptr_t)%s);\n", env_free_names[i]);
        free(env_free_names[i]);
    }
    free(env_free_names);

    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");

    return nil_result ? atom_nil() : tmp;
}

/* emit_letrec_value -- EX_LETREC codegen.
 *
 * For fn-valued bindings marked is_global (no-capture static fns), the static
 * function body is already emitted at file scope (via elab_register_file_def)
 * and Pass 1 in emit_module.c emitted forward declarations for all of them.
 * We must NOT declare a local variable with the same name as the static fn --
 * that would create an uninitialized self-referencing fn-pointer (crash).
 *
 * For non-fn or non-global bindings, we fall through to emit_let_value. */
static char *emit_letrec_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Check whether any binding needs the special letrec treatment. */
    bool has_global_fn = false;
    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *b = e->as.let_.bindings[i].binding;
        if (b->is_global && b->c_export_name && b->type.kind == TY_FN) {
            has_global_fn = true;
            break;
        }
    }
    if (!has_global_fn) return emit_let_value(ctx, body, e);

    /* At least one binding is a global fn -- handle the entire binding block
     * ourselves so we can skip local-var declarations for those bindings. */
    bool body_has_return_or_throw = expr_contains_return_or_throw(e->as.let_.body);
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    if (!nil_result) {
        tmp = fresh_tmp(ctx);
        emit_temp_decl(ctx, body, e->type, tmp, NULL);
    }

    indent_buf(body, ctx->indent);
    buf_puts(body, "{\n");
    ctx->indent += 4;

    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *b = e->as.let_.bindings[i].binding;
        if (b->is_global && b->c_export_name && b->type.kind == TY_FN) {
            /* This binding refers to a file-scope static function.
             * Call emit_value anyway in case it has side-effects (unlikely for
             * EX_VAR but keeps the logic consistent), then discard the result.
             * Do NOT declare a local variable -- that would shadow the static fn
             * with an uninitialized pointer and crash. */
            char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
            free(iv);
            continue;
        }
        /* Normal binding -- mirror emit_let_value logic. */
        char *bn = name_for_binding(ctx, b);
        char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
        indent_buf(body, ctx->indent);
        if (b->type.kind == TY_FN) {
            buf_printf(body, "%s (*%s)(",
                       type_c_name(emit_type_from_kind(b->type.as.fn.result_kind)), bn);
            for (uint8_t j = 0; j < b->type.as.fn.arity; j++) {
                if (j > 0) buf_puts(body, ", ");
                buf_printf(body, "%s",
                           type_c_name(emit_type_from_kind(b->type.as.fn.arg_kinds[j])));
            }
            buf_printf(body, ") = %s;\n", iv);
        } else if (b->is_poly_fn) {
            buf_printf(body, "tur_poly_fn_t %s = %s;\n", bn, iv);
        } else {
            const char *bind_c = emit_binding_repr_c_name(ctx, b->type,
                                     e->as.let_.bindings[i].init);
            if (e->as.let_.bindings[i].binding)
                e->as.let_.bindings[i].binding->emit_byvalue_carrier_abi =
                    type_uses_carrier_abi(emit_resolve_type(ctx, b->type)) &&
                    strcmp(bind_c, "int64_t") != 0;
            /* CC1 (curried-call-cast-rough-edges-plan): when the binding is
             * declared as an int64_t carrier but the init expression yields a
             * function pointer or void * (e.g. a PAP wrapper capturing a bare
             * top-level defn into its env), wrap the init with the standard
             * (int64_t)(intptr_t) coercion -- otherwise clang rejects the
             * implicit pointer-to-int conversion under -Wint-conversion. */
            TypeKind init_kind = e->as.let_.bindings[i].init->type.kind;
            if (strcmp(bind_c, "int64_t") == 0 &&
                (init_kind == TY_FN || init_kind == TY_PTR_VOID)) {
                buf_printf(body, "%s %s = (int64_t)(intptr_t)(%s);\n", bind_c, bn, iv);
            } else {
                buf_printf(body, "%s %s = %s;\n", bind_c, bn, iv);
            }
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "(void)%s;\n", bn);
        free(bn);
        free(iv);
    }

    if (body_has_return_or_throw) {
        if (!nil_result && !expr_is_divergent(e->as.let_.body)) {
            char *bv = emit_value(ctx, body, e->as.let_.body);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, bv);
            free(bv);
        } else {
            emit_stmt(ctx, body, e->as.let_.body);
        }
        ctx->indent -= 4;
        indent_buf(body, ctx->indent);
        buf_puts(body, "}\n");
        return tmp ? tmp : atom_nil();
    }

    if (nil_result) {
        emit_stmt(ctx, body, e->as.let_.body);
    } else {
        char *bv = emit_value(ctx, body, e->as.let_.body);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", tmp, bv);
        free(bv);
    }

    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");

    return nil_result ? atom_nil() : tmp;
}

static char *emit_if_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Phase 3/4: Check if branches contain return or throw */
    bool then_has_return_or_throw = expr_contains_return_or_throw(e->as.if_.then_);
    bool else_has_return_or_throw = e->as.if_.else_or_null ? 
        expr_contains_return_or_throw(e->as.if_.else_or_null) : false;
    bool any_has_return_or_throw = then_has_return_or_throw || else_has_return_or_throw;
    
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    /* Phase R1: Also create temp if only then-branch has return/throw and else doesn't */
    bool else_no_return = e->as.if_.else_or_null ? !else_has_return_or_throw : false;
    if (!nil_result && ( !any_has_return_or_throw || (then_has_return_or_throw && else_no_return))) {
        tmp = fresh_tmp(ctx);
        emit_temp_decl(ctx, body, e->type, tmp, NULL);
    }
    char *cond = emit_value(ctx, body, e->as.if_.cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) {\n", cond);
    free(cond);
    ctx->indent += 4;
    if (nil_result || any_has_return_or_throw) {
        emit_stmt(ctx, body, e->as.if_.then_);
    } else {
        char *t = emit_value(ctx, body, e->as.if_.then_);
        indent_buf(body, ctx->indent);
        /* SF-application carrier bridge (if-branch assign):
         * an `^fat`-bound variable is held in C as int64_t even though its
         * elab type (TY_FN boxed or TY_PTR_VOID) lowers `e->type` to a
         * pointer type at the temp's declaration.  Bridge a bare ^fat var
         * source with (void *)(intptr_t) so clang accepts the assign. */
        const Expr *th = e->as.if_.then_;
        while (th && th->kind == EX_ASCRIBE) th = th->as.ascribe_.inner;
        bool then_is_fat_var = th && th->kind == EX_VAR && th->as.var.binding
                               && th->as.var.binding->is_fat;
        const char *temp_c = type_c_name(e->type);
        size_t tlen = strlen(temp_c);
        bool temp_is_ptr = tlen > 0 && temp_c[tlen - 1] == '*';
        if (then_is_fat_var && temp_is_ptr) {
            buf_printf(body, "%s = (void *)(intptr_t)(%s);\n", tmp, t);
        } else {
            buf_printf(body, "%s = %s;\n", tmp, t);
        }
        free(t);
    }
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "} else {\n");
    ctx->indent += 4;
    if (e->as.if_.else_or_null) {
        /* Phase R1: Special handling for ? operator lowering.
         * If only the then-branch has return/throw, emit the else-branch as a value.
         * This allows the ? operator to work: (if (err? x) (return ...) (ok-val x))
         */
        if (nil_result || (then_has_return_or_throw && else_has_return_or_throw)) {
            emit_stmt(ctx, body, e->as.if_.else_or_null);
        } else {
            char *el = emit_value(ctx, body, e->as.if_.else_or_null);
            indent_buf(body, ctx->indent);
            /* See the then-branch comment above. */
            const Expr *eb = e->as.if_.else_or_null;
            while (eb && eb->kind == EX_ASCRIBE) eb = eb->as.ascribe_.inner;
            bool else_is_fat_var = eb && eb->kind == EX_VAR && eb->as.var.binding
                                    && eb->as.var.binding->is_fat;
            const char *temp_c2 = type_c_name(e->type);
            size_t tlen2 = strlen(temp_c2);
            bool temp_is_ptr2 = tlen2 > 0 && temp_c2[tlen2 - 1] == '*';
            if (else_is_fat_var && temp_is_ptr2) {
                buf_printf(body, "%s = (void *)(intptr_t)(%s);\n", tmp, el);
            } else {
                buf_printf(body, "%s = %s;\n", tmp, el);
            }
            free(el);
        }
    } else {
        /* missing else: nothing to do (result type is nil) */
    }
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    return nil_result ? atom_nil() : tmp;
}

/* Phase 4 v1: defer-aware do-block emission.
 *
 * v1 strategy (effects-plan.md §6.10): Each scope gets a tur_frame variable.
 * Defers are collected and fired via tur_frame_fire_lifo at scope exit.
 * This is the unified runtime-list-on-frame model.
 *
 * For v1, we still emit defer bodies inline (not as thunks yet),
 * but we use the frame infrastructure for future-proofing.
 *
 * Known limitations:
 *   - Defer bodies are still emitted inline (v0 style), not as thunks
 *   - Captures are not yet properly handled via env structs
 *   - Defers inside `while` bodies fire on every loop iteration
 *   - Early `return` is not yet a language feature
 *
 * Future: Proper thunk generation with captures (effects-plan.md §6.10). */
static char *emit_do_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    uint32_t n = e->as.do_.n;
    if (n == 0) return atom_nil();

    /* Phase 3/4: Check if there's a return/throw or defer in this do block */
    bool has_return_or_throw = false;
    bool has_defers = false;
    for (uint32_t i = 0; i < n; i++) {
        const Expr *it = e->as.do_.items[i];
        if (it->kind == EX_DEFER) {
            has_defers = true;
        } else if (expr_contains_return_or_throw(it)) {
            has_return_or_throw = true;
        }
    }
    
    /* If there's a return/throw, we need frame support for defer firing */
    if (has_return_or_throw) {
        has_defers = true;
    }

    if (!has_defers) {
        /* No defers - use old path (no frame needed) */
        int last_value_idx = -1;
        for (int i = (int)n - 1; i >= 0; i--) {
            if (e->as.do_.items[i]->kind != EX_DEFER) { last_value_idx = i; break; }
        }

        for (uint32_t i = 0; i < n; i++) {
            const Expr *it = e->as.do_.items[i];
            if (it->kind == EX_DEFER) continue;
            if ((int)i == last_value_idx) continue;
            emit_stmt(ctx, body, it);
        }

        char *result = NULL;
        if (last_value_idx >= 0) {
            const Expr *last = e->as.do_.items[last_value_idx];
            /* Phase 3/4: If last item is return or throw, emit as statement only */
            if (last->kind == EX_RETURN || last->kind == EX_PANIC ||
                last->kind == EX_PANIC_WITH || last->kind == EX_THROW) {
                emit_stmt(ctx, body, last);
                return atom_nil();
            } else if (last->type.kind == TY_NIL &&
                       /* Phase 19D: fiber-based resume always produces int64_t result */
                       !(last->kind == EX_RESUME && last->as.resume_.resume &&
                         last->as.resume_.resume->k && last->as.resume_.resume->k->type.kind == TY_INT)) {
                emit_stmt(ctx, body, last);
            } else {
                result = fresh_tmp(ctx);
                /* Phase 19D: if nil-typed but is fiber resume, use int64_t to capture result */
                bool is_fiber_resume = (last->kind == EX_RESUME && last->as.resume_.resume &&
                                        last->as.resume_.resume->k &&
                                        last->as.resume_.resume->k->type.kind == TY_INT);
                if (last->type.kind == TY_NIL && is_fiber_resume) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "int64_t %s;\n", result);
                } else {
                    emit_temp_decl(ctx, body, last->type, result, NULL);
                }
                char *v = emit_value(ctx, body, last);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, v);
                free(v);
            }
        }
        return result ? result : atom_nil();
    }

    /* v1 lowering: Has defers - use tur_frame */
    /* Save parent frame and set up new frame */
    const char *saved_frame = ctx->frame_var;
    char *frame_var = fresh_frame(ctx);
    ctx->frame_var = frame_var;

    /* Emit frame declaration and init */
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_frame %s;\n", frame_var);
    indent_buf(body, ctx->indent);
    if (saved_frame) {
        buf_printf(body, "tur_frame_init(&%s, &%s);\n", frame_var, saved_frame);
    } else {
        buf_printf(body, "tur_frame_init(&%s, NULL);\n", frame_var);
    }

    /* Find last non-defer item to source the do-block's value from. */
    int last_value_idx = -1;
    for (int i = (int)n - 1; i >= 0; i--) {
        if (e->as.do_.items[i]->kind != EX_DEFER) { last_value_idx = i; break; }
    }

    /* Phase 3/4: If there's a return or throw, emit all items as statements */
    if (has_return_or_throw) {
        /* Phase R1: The do may still produce a value on the non-divergent
         * path (e.g. `(do (if cond (return ...) ...) (defer ...))` whose
         * last non-defer item is the if, and the if's else branch yields
         * a value).  If the last value-producing item is non-divergent
         * and the do has a non-nil result type, materialise it into a
         * result tmp; otherwise fall back to statement-only emission. */
        const Expr *last_value = (last_value_idx >= 0)
                                     ? e->as.do_.items[last_value_idx] : NULL;
        bool last_yields_value = last_value &&
                                 e->type.kind != TY_NIL &&
                                 !expr_is_divergent(last_value);
        char *result_tmp = NULL;
        if (last_yields_value) {
            result_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s;\n", type_c_name(e->type), result_tmp);
        }

        /* Emit all items, registering defers */
        bool emitted_diverging = false;  /* Track if we've emitted a return/throw/panic */
        for (uint32_t i = 0; i < n; i++) {
            const Expr *it = e->as.do_.items[i];

            /* If we've already emitted a diverging expression (return/throw/panic),
             * skip remaining items as they're unreachable */
            if (emitted_diverging) {
                continue;
            }

            if (it->kind == EX_DEFER) {
                /* Register defer thunks */
                const Expr *defer_expr = it->as.defer_.body;
                const uint8_t n_captures = it->as.defer_.n_captures;
                Binding **captures = it->as.defer_.captures;

                if (n_captures == 0) {
                    char *thunk_name = fresh_defer_thunk(ctx);
                    register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, NULL);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "tur_frame_push_defer(&%s, %s, NULL);\n", frame_var, thunk_name);
                } else {
                    char *env_name = fresh_defer_env(ctx);
                    char *thunk_name = fresh_defer_thunk(ctx);
                    register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, env_name);

                    char *env_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "struct %s %s = {", env_name, env_tmp);
                    for (uint8_t j = 0; j < n_captures; j++) {
                        if (j > 0) buf_puts(body, ", ");
                        char *cn = name_for_binding(ctx, captures[j]);
                        char *field = raw_name_for_binding(captures[j]);
                        buf_printf(body, ".%s = %s", field, cn);
                        free(field);
                        free(cn);
                    }
                    buf_puts(body, "};\n");

                    indent_buf(body, ctx->indent);
                    buf_printf(body, "tur_frame_push_defer(&%s, %s, &%s);\n", frame_var, thunk_name, env_tmp);
                    free(env_tmp);
                    free(env_name);
                }
            } else if (last_yields_value && (int)i == last_value_idx) {
                char *bv = emit_value(ctx, body, it);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result_tmp, bv);
                free(bv);
            } else {
                emit_stmt(ctx, body, it);
                /* Check if this statement diverges (return/throw/panic) */
                if (it->kind == EX_RETURN ||
                    it->kind == EX_PANIC || it->kind == EX_PANIC_WITH) {
                    emitted_diverging = true;
                }
            }
        }

        /* Fire frame defers on the non-divergent path before producing the
         * do's value.  Divergent paths already fired via tur_frame_fire_chain
         * in the emitted return/throw/panic statement. */
        if (last_yields_value && !emitted_diverging) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);
        }

        ctx->frame_var = saved_frame;
        free(frame_var);
        return result_tmp ? result_tmp : atom_nil();
    }

    /* Emit non-defer items and register defer thunks */
    for (uint32_t i = 0; i < n; i++) {
        const Expr *it = e->as.do_.items[i];
        if (it->kind == EX_DEFER) {
            /* v1 lowering: Generate thunk and register with frame */
            const Expr *defer_expr = it->as.defer_.body;
            const uint8_t n_captures = it->as.defer_.n_captures;
            Binding **captures = it->as.defer_.captures;
            
            if (n_captures == 0) {
                /* No captures - generate thunk */
                char *thunk_name = fresh_defer_thunk(ctx);
                register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, NULL);
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_push_defer(&%s, %s, NULL);\n", frame_var, thunk_name);
            } else {
                /* Has captures - generate thunk with env struct */
                char *env_name = fresh_defer_env(ctx);
                char *thunk_name = fresh_defer_thunk(ctx);
                register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, env_name);
                
                /* Emit env struct type definition at file scope */
                /* We'll emit this in emit_pending_defer_thunks, but we need to
                 * also create the env instance here and pass its address */
                
                /* Create env instance and register with address */
                char *env_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "struct %s %s = {", env_name, env_tmp);
                for (uint8_t j = 0; j < n_captures; j++) {
                    if (j > 0) buf_puts(body, ", ");
                    char *cn = name_for_binding(ctx, captures[j]);
                    char *field = raw_name_for_binding(captures[j]);
                    buf_printf(body, ".%s = %s", field, cn);
                    free(field);
                    free(cn);
                }
                buf_puts(body, "};\n");

                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_push_defer(&%s, %s, &%s);\n", frame_var, thunk_name, env_tmp);
                free(env_tmp);
                free(env_name);
            }
        } else if ((int)i == last_value_idx) {
            continue; /* emitted as value below */
        } else {
            emit_stmt(ctx, body, it);
        }
    }

    char *result = NULL;
    if (last_value_idx >= 0) {
        const Expr *last = e->as.do_.items[last_value_idx];
        if (last->type.kind == TY_NIL) {
            emit_stmt(ctx, body, last);
        } else {
            /* Hoist value into a temp so defers can fire after it's computed. */
            result = fresh_tmp(ctx);
            emit_temp_decl(ctx, body, last->type, result, NULL);
            char *v = emit_value(ctx, body, last);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", result, v);
            free(v);
        }
    }

    /* Fire all defers LIFO at scope exit */
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);

    /* Restore frame state */
    ctx->frame_var = saved_frame;
    free(frame_var);

    return result ? result : atom_nil();
}

/* GF1: Emit the C struct typedef, _create function, and _next function for a generator.
 * Written to ctx->pending_handler_fns so they are flushed to file scope before the
 * enclosing function definition (same pattern as algebraic-effect handler functions). */
static void emit_gen_functions(EmitCtx *ctx, const GenDef *def) {
    /* Use pending_handler_fns so generator structs/functions land at file scope
     * before the function that creates them (emit_fn_def drains this buffer). */
    Buf *out = ctx->pending_handler_fns ? ctx->pending_handler_fns : ctx->file;

    /* Emit common gen header typedef once per compilation unit */
    if (!ctx->gen_hdr_emitted) {
        buf_puts(out, "typedef struct { int32_t __state; void *(*__next_fn)(void *); } __tur_gen_hdr_t;\n");
        ctx->gen_hdr_emitted = true;
    }

    /* Struct typedef: __state, __next_fn, then all struct_binding fields */
    buf_printf(out, "typedef struct { int32_t __state; void *(*__next_fn)(void *);");
    for (uint32_t i = 0; i < def->n_struct_bindings; i++) {
        Binding *b = def->struct_bindings[i];
        char *fn = raw_name_for_binding(b);
        buf_printf(out, " %s %s;", type_c_name(b->type), fn);
        free(fn);
    }
    buf_printf(out, " } %s;\n", def->struct_name);

    /* Forward-declare _next before _create so _create can take its address */
    buf_printf(out, "static void *%s(void *__gv);\n", def->next_fn);

    /* _create function */
    buf_printf(out, "static void *%s(", def->create_fn);
    if (def->n_captures == 0) {
        buf_puts(out, "void");
    } else {
        for (uint32_t i = 0; i < def->n_captures; i++) {
            if (i > 0) buf_puts(out, ", ");
            Binding *b = def->captures[i];
            char *fn = raw_name_for_binding(b);
            buf_printf(out, "%s %s", type_c_name(b->type), fn);
            free(fn);
        }
    }
    buf_puts(out, ") {\n");
    buf_printf(out, "    %s *__g = (%s *)malloc(sizeof(%s));\n",
               def->struct_name, def->struct_name, def->struct_name);
    buf_printf(out, "    __g->__state = 0;\n");
    buf_printf(out, "    __g->__next_fn = %s;\n", def->next_fn);
    for (uint32_t i = 0; i < def->n_captures; i++) {
        Binding *b = def->captures[i];
        char *fn = raw_name_for_binding(b);
        buf_printf(out, "    __g->%s = %s;\n", fn, fn);
        free(fn);
    }
    buf_printf(out, "    return (void *)__g;\n");
    buf_printf(out, "}\n");

    /* _next function body -- built into a separate buf, then appended */
    Buf fn_body;
    buf_init(&fn_body);
    buf_printf(&fn_body, "static void *%s(void *__gv) {\n", def->next_fn);
    buf_printf(&fn_body, "    %s *__g = (%s *)__gv;\n", def->struct_name, def->struct_name);

    /* State dispatch at top -- jump to resume label for each yield point */
    for (uint32_t i = 0; i < def->n_yield_points; i++) {
        buf_printf(&fn_body, "    if (__g->__state == %u) goto __yield_%u;\n", i + 1, i);
    }
    buf_printf(&fn_body, "    if (__g->__state == -1) return NULL;\n");

    /* Save and override gen context in ctx */
    Binding    **saved_gsb   = ctx->gen_struct_bindings;
    uint32_t    saved_n_gsb  = ctx->n_gen_struct_bindings;
    const char *saved_gvn    = ctx->gen_var_name;
    const char *saved_gst    = ctx->gen_struct_type;
    int         saved_indent = ctx->indent;

    ctx->gen_struct_bindings   = def->struct_bindings;
    ctx->n_gen_struct_bindings = def->n_struct_bindings;
    ctx->gen_var_name          = "__g";
    ctx->gen_struct_type       = def->struct_name;
    ctx->indent                = 4;

    emit_stmt(ctx, &fn_body, def->body);

    /* After body: mark done and return NULL */
    buf_printf(&fn_body, "    __g->__state = -1;\n");
    buf_printf(&fn_body, "    return NULL;\n");
    buf_printf(&fn_body, "}\n");

    /* Restore context */
    ctx->gen_struct_bindings   = saved_gsb;
    ctx->n_gen_struct_bindings = saved_n_gsb;
    ctx->gen_var_name          = saved_gvn;
    ctx->gen_struct_type       = saved_gst;
    ctx->indent                = saved_indent;

    /* Append _next to out */
    buf_putc(&fn_body, '\0');
    buf_puts(out, fn_body.data);
    buf_free(&fn_body);
}


/* Phase D: returns true when struct_expr is an EX_VAR reference to a current-function
 * parameter that is passed by pointer (const T*) rather than by value. */
static bool expr_is_pbp_param(EmitCtx *ctx, const Expr *struct_expr) {
    if (!ctx || !struct_expr || struct_expr->kind != EX_VAR) return false;
    Binding *b = struct_expr->as.var.binding;
    if (!b) return false;
    for (uint8_t _i = 0; _i < ctx->n_pbp_params; _i++) {
        if (ctx->pbp_param_ptrs[_i] == b) return true;
    }
    return false;
}

char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    switch (e->kind) {
        case EX_NIL_LIT:  return atom_nil();
        case EX_BOOL_LIT: return atom_bool(e->as.b);
        case EX_INT_LIT:  return atom_int_typed(e->as.i, e->type.kind);
        case EX_FLOAT_LIT:
            if (e->type.kind == TY_FLOAT32) return atom_float32(e->as.f);
            return atom_float(e->as.f);
        case EX_CSTR_LIT: return atom_cstr(e->as.s);
        case EX_DEFAULT_OF: {
            /* M2b: (default-of T) -> C99 compound literal (T){0}.  Zero-inits any
             * scalar, pointer, or aggregate; for value structs every field is
             * recursively zeroed.  emit_type_c_name resolves to the concrete C
             * name (already accounts for parametric struct instantiations). */
            const char *cname = emit_type_c_name(ctx, e->type);
            Buf out; buf_init(&out);
            buf_printf(&out, "(%s){0}", cname);
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_SYM_LIT: {
            /* SYM1: reference the TU-local static record for this keyword. */
            const char *cid = sym_codegen_register(e->as.sym_lit_.sym);
            Buf out; buf_init(&out);
            buf_printf(&out, "((const struct __tur_sym *)&%s)", cid);
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_CPS_CONT_APP:
            /* CPS2 stub: EX_CPS_CONT_APP is generated only by the CPS3 lowering pass;
             * reaching emit_value here means a CPS-lowered node leaked into a
             * context that has not yet been updated to handle it. */
            fprintf(stderr, "tur: internal error: EX_CPS_CONT_APP reached emit_value "
                    "(CPS3 lowering not yet active for this path)\n");
            abort();
        case EX_VAR:      return atom_var(ctx, e->as.var.binding);
        case EX_CAST: {
            /* (as TargetType expr) — emit as C cast: (target_c_type)(inner) */
            char *inner = emit_value(ctx, body, e->as.cast_.expr);
            Type target = type_simple(e->as.cast_.target_kind, CK_COPY);
            Buf out; buf_init(&out);
            buf_printf(&out, "((%s)(%s))", type_c_name(target), inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_REINTERPRET: {
            if (e->as.reinterpret_.expr && e->as.reinterpret_.expr->kind == EX_CALL) {
                const Expr *inner_call = e->as.reinterpret_.expr;
                for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
                    const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
                    if (spec->call_expr == inner_call &&
                        type_eq(spec->result_type, e->type)) {
                        return emit_value(ctx, body, inner_call);
                    }
                }
                /* KB-015: the first call to a specialised clone registers spec->call_expr
                 * pointing to that first EX_CALL node.  Subsequent calls to the same
                 * instantiation get fresh EX_CALL nodes that appear only in
                 * specialized_call_exprs[] -- spec->call_expr does not cover them.
                 * Without this second check, the reinterpretation fires on a call that
                 * already returns the concrete type, truncating the double to int64_t. */
                if (ctx) {
                    for (uint32_t si = 0; si < ctx->n_specialized_calls; si++) {
                        if (ctx->specialized_call_exprs[si] == inner_call) {
                            return emit_value(ctx, body, inner_call);
                        }
                    }
                }
            }
            TypeKind src_kind = e->as.reinterpret_.source_kind;
            TypeKind dst_kind = e->as.reinterpret_.target_kind;
            int src_size = reinterpret_kind_size_bytes(src_kind);
            int dst_size = reinterpret_kind_size_bytes(dst_kind);
            if (!reinterpret_kind_is_scalar(src_kind) || !reinterpret_kind_is_scalar(dst_kind) ||
                src_size <= 0 || src_size != dst_size) {
                fprintf(stderr,
                        "tur: emit: invalid EX_REINTERPRET %s -> %s\n",
                        typekind_to_string(src_kind), typekind_to_string(dst_kind));
                abort();
            }

            char *inner = emit_value(ctx, body, e->as.reinterpret_.expr);
            Type src = type_simple(src_kind, CK_COPY);
            Type dst = type_simple(dst_kind, CK_COPY);
            Buf out; buf_init(&out);
            buf_printf(&out, "((union { %s s; %s d; }){.s = %s}).d",
                       type_c_name(src), type_c_name(dst), inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_UNION_INJECT: {
            /* IT4: wrap a member value into tur_tagged_t via TUR_TAG(tag_idx, val).
             * Carrier-compatible payloads (int/bool/float/nil/cstr/ptr and ADT
             * handles) are cast to int64_t directly.
             *
             * TY2.2: a by-value struct cannot be cast to int64_t, so box_struct
             * is set: emit a heap copy (malloc + assign) and store the pointer
             * as the tagged payload.  Unbox is the reverse (deref) in EX_ANY_CAST. */
            char *inner = emit_value(ctx, body, e->as.union_inject_.value);
            Buf out; buf_init(&out);
            const StructDef *bs = e->as.union_inject_.box_struct;
            int64_t tag = e->as.union_inject_.tag_idx;
            if (bs) {
                /* heap-box: ({ T *__b = malloc(sizeof(T)); *__b = <inner>;
                 *             TUR_TAG(tag, (int64_t)(intptr_t)__b); }) */
                buf_printf(&out,
                    "__extension__ ({ %s *__tur_box = (%s *)malloc(sizeof(%s)); "
                    "*__tur_box = (%s); TUR_TAG(%lld, (int64_t)(intptr_t)__tur_box); })",
                    bs->name, bs->name, bs->name, inner, (long long)tag);
            } else if (tag == (int64_t)TY_FLOAT) {
                /* TY2.2: a double does not survive an integer cast -- store its
                 * IEEE-754 bit pattern in the payload via a union reinterpret. */
                buf_printf(&out,
                    "TUR_TAG(%lld, ((union { double d; int64_t i; }){.d = (%s)}).i)",
                    (long long)tag, inner);
            } else {
                buf_printf(&out, "TUR_TAG(%lld, (int64_t)(intptr_t)(%s))",
                           (long long)tag, inner);
            }
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_ANY_TYPE_OF: {
            /* IT4: (type-of x) — return cstr type name via __tur_any_type_name(tag) */
            char *inner = emit_value(ctx, body, e->as.any_type_of_.value);
            Buf out; buf_init(&out);
            buf_printf(&out, "__tur_any_type_name(TUR_GETTAG(%s))", inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_ANY_IS: {
            /* TY3: (is? x T) — compare the box tag to the tested TypeKind. */
            char *inner = emit_value(ctx, body, e->as.any_is_.value);
            Buf out; buf_init(&out);
            buf_printf(&out, "(TUR_GETTAG(%s) == %lld)",
                       inner, (long long)e->as.any_is_.test_tag);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_ANY_CAST: {
            /* TY2.3: (cast x T) — checked downcast.  Verify the box tag matches
             * the target TypeKind; tur_panic on mismatch, otherwise unbox.
             * TY2.2: a struct target unboxes by dereferencing the heap pointer. */
            char *inner = emit_value(ctx, body, e->as.any_cast_.value);
            const StructDef *ts = e->as.any_cast_.target_struct;
            int64_t target_tag = (int64_t)e->as.any_cast_.target_kind;
            Buf out; buf_init(&out);
            if (ts) {
                buf_printf(&out,
                    "__extension__ ({ tur_tagged_t __tur_c = (%s); "
                    "__tur_any_cast_check(TUR_GETTAG(__tur_c), %lld); "
                    "*(%s *)(intptr_t)TUR_UNTAG(__tur_c); })",
                    inner, (long long)target_tag, ts->name);
            } else if (target_tag == (int64_t)TY_FLOAT) {
                /* TY2.2: reverse the float bit-reinterpret stored on inject. */
                buf_printf(&out,
                    "__extension__ ({ tur_tagged_t __tur_c = (%s); "
                    "__tur_any_cast_check(TUR_GETTAG(__tur_c), %lld); "
                    "((union { int64_t i; double d; }){.i = TUR_UNTAG(__tur_c)}).d; })",
                    inner, (long long)target_tag);
            } else {
                Type target = type_simple(e->as.any_cast_.target_kind, CK_COPY);
                buf_printf(&out,
                    "__extension__ ({ tur_tagged_t __tur_c = (%s); "
                    "__tur_any_cast_check(TUR_GETTAG(__tur_c), %lld); "
                    "(%s)(intptr_t)TUR_UNTAG(__tur_c); })",
                    inner, (long long)target_tag, type_c_name(target));
            }
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_LET:      return emit_let_value(ctx, body, e);
        case EX_LETREC:   return emit_letrec_value(ctx, body, e);
        case EX_IF:       return emit_if_value(ctx, body, e);
        case EX_DO:       return emit_do_value(ctx, body, e);
        case EX_BUILTIN:  return emit_builtin(ctx, body, e);
        case EX_WHILE:    emit_while_stmt(ctx, body, e); return atom_nil();
        case EX_SET:      emit_set_stmt(ctx, body, e);   return atom_nil();
        case EX_SET_DEREF: emit_set_deref_stmt(ctx, body, e); return atom_nil();
        case EX_SET_FIELD: emit_set_field_stmt(ctx, body, e); return atom_nil();
        case EX_DEF:      /* handled at top level — shouldn't appear nested. */
        case EX_PROGRAM:
        case EX_DEFMODULE: /* Phase M0: module metadata node */
        case EX_TYPECLASS_DEF:
            /* Typeclass definitions are compile-time only - no runtime code */
            return atom_nil();
        case EX_INSTANCE_DEF:
            /* Handled in emit_stmt - file scope only */
            return atom_nil();
        /* Phase H §1: dictionary passing — either load method field (for dict
         * dispatch via fn_expr) or emit singleton address (bare dict value). */
        case EX_DICT: {
            char dict_name[128];
            emit_dict_name(dict_name, sizeof(dict_name), e->as.dict_.instance);
            Buf out; buf_init(&out);
            if (e->as.dict_.method_name[0] != '\0') {
                /* Dictionary dispatch: emit function-pointer field access.
                 * The EX_CALL indirect-call path then casts and calls it:
                 *   ((ret_t (*)(...))(intptr_t)(dict_X_singleton.method))(args) */
                buf_printf(&out, "%s_singleton.%s", dict_name, e->as.dict_.method_name);
            } else {
                /* Bare dictionary value: singleton address as int64_t. */
                buf_printf(&out, "(int64_t)(intptr_t)(&%s_singleton)", dict_name);
            }
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        /* Phase R2: Panic */
        case EX_PANIC: {
            /* (panic msg) - evaluate msg (cstr), call tur_panic[_abort] */
            const Expr *payload = e->as.panic_.payload;
            char *msg_val = emit_value(ctx, body, payload);
            indent_buf(body, ctx->indent);
            if (ctx->no_unwind) {
                /* Phase R5: #[no-unwind] — abort directly; skip defer chain */
                if (payload->type.kind == TY_CSTR) {
                    buf_printf(body, "tur_panic_abort(%s);\n", msg_val);
                } else {
                    buf_puts(body, "tur_panic_abort(\"(non-string panic)\");\n");
                }
            } else {
                /* Set the current frame for panic to fire defers */
                if (ctx->frame_var) {
                    buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
                }
                if (payload->type.kind == TY_CSTR) {
                    buf_printf(body, "tur_panic(%s);\n", msg_val);
                } else {
                    /* For non-cstr, use a generic message */
                    buf_printf(body, "tur_panic(\"(non-string panic)\");\n");
                }
            }
            free(msg_val);
            return atom_nil();
        }
        case EX_PANIC_WITH: {
            /* (panic-with payload) - panic with typed payload */
            const Expr *payload = e->as.panic_with_.payload;
            char *payload_val = emit_value(ctx, body, payload);
            indent_buf(body, ctx->indent);
            if (ctx->no_unwind) {
                /* Phase R5: #[no-unwind] — abort directly; skip defer chain */
                buf_puts(body, "tur_panic_abort(\"(panic-with)\");\n");
            } else {
                /* Set the current frame for panic to fire defers */
                if (ctx->frame_var) {
                    buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
                }
                /* tur_panic_with takes type_tag (int), payload (void*), file, line */
                buf_printf(body, "tur_panic_with(%d, (void*)%s, __FILE__, __LINE__);\n",
                           (int)payload->type.kind, payload_val);
            }
            free(payload_val);
            return atom_nil();
        }
        case EX_CATCH_UNWIND: {
            /* (catch-unwind thunk) - run thunk behind a panic boundary; yields a
             * result box (ok = thunk value, err = opaque Panic handle). */
            const Expr *thunk = e->as.catch_unwind_.thunk;
            char *thunk_val = emit_value(ctx, body, thunk);
            char result_var[64];
            snprintf(result_var, sizeof(result_var), "__catch_result_%d", ctx->tmp_n++);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_catch_unwind_box((int64_t)(intptr_t)%s);\n",
                       result_var, thunk_val);
            free(thunk_val);
            return strdup(result_var);
        }
        case EX_CATCH_PANIC_OF: {
            /* (catch-panic-of Type thunk) - typed catch boundary; re-raises panics
             * whose payload type does not match Type. */
            const Expr *thunk = e->as.catch_panic_of_.thunk;
            TypeKind type_kind = e->as.catch_panic_of_.type_kind;
            char *thunk_val = emit_value(ctx, body, thunk);
            char result_var[64];
            snprintf(result_var, sizeof(result_var), "__catch_panic_of_result_%d", ctx->tmp_n++);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_catch_panic_of_box(%d, (int64_t)(intptr_t)%s);\n",
                       result_var, (int)type_kind, thunk_val);
            free(thunk_val);
            return strdup(result_var);
        }
        /* Phase S4: throw / try-catch */
        case EX_THROW: {
            const Expr *thrown = e->as.throw_.value;
            char *val = emit_value(ctx, body, thrown);
            indent_buf(body, ctx->indent);
            if (ctx->no_unwind) {
                buf_printf(body, "tur_panic_abort(\"(throw)\");\n");
            } else {
                if (ctx->frame_var) {
                    buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
                }
                buf_printf(body, "tur_panic_with(%d, (void*)%s, __FILE__, __LINE__);\n",
                           (int)thrown->type.kind, val);
            }
            free(val);
            return atom_nil();
        }
        case EX_TRY_CATCH: {
            /* Phase S4: wrap the try body in a tur_thunk_fn, call tur_catch_unwind,
             * and dispatch the first catch clause on panic.
             *
             * Fall back to body-only emission when:
             *   - no_unwind: panics already abort at the C level, catch is unreachable.
             *   - no pending_handler_fns: we have nowhere to put the thunk.
             */
            if (ctx->no_unwind || !ctx->pending_handler_fns) {
                return emit_value(ctx, body, e->as.try_catch_.body);
            }

            const Expr    *try_body      = e->as.try_catch_.body;
            uint32_t       n_catches     = e->as.try_catch_.n_catches;
            Binding      **catch_binds   = e->as.try_catch_.catch_bindings;
            Expr         **catch_hdlrs   = e->as.try_catch_.catch_handlers;

            /* Collect free variables (captures) from the try body */
            uint32_t   n_caps = 0;
            Binding  **caps   = collect_handle_captures(try_body, &n_caps);

            int thunk_id = ctx->tmp_n++;
            Buf *pbuf    = ctx->pending_handler_fns;

            /* Emit env struct if there are captures */
            if (n_caps > 0) {
                buf_printf(pbuf, "typedef struct { ");
                for (uint32_t ci = 0; ci < n_caps; ci++) {
                    Binding *cap = caps[ci];
                    char *rn = raw_name_for_binding(cap);
                    /* A captured fn value is the int64_t fn-ABI carrier, not its
                     * result type's C name (type_c_name(TY_FN) -> "double" for a
                     * :float result would alias the closure pointer through a
                     * floating-point field). */
                    const char *cap_ctype = cap->type.kind == TY_FN
                        ? "int64_t"
                        : type_c_name(cap->type);
                    buf_printf(pbuf, "%s %s; ", cap_ctype, rn);
                    free(rn);
                }
                buf_printf(pbuf, "} __try_env_%d;\n", thunk_id);
            }

            /* Build the thunk body in a temporary buffer */
            Buf thunk_body; buf_init(&thunk_body);

            EmitCtx tctx              = *ctx;
            tctx.file                 = pbuf;
            tctx.pending_handler_fns  = pbuf;
            tctx.indent               = 4;
            tctx.fn_params            = NULL;
            tctx.n_fn_params          = 0;
            tctx.closure              = NULL;
            tctx.env_var_name         = (n_caps > 0) ? "__e" : NULL;
            tctx.defer_captures       = caps;
            tctx.n_defer_captures     = (uint8_t)(n_caps > 255 ? 255 : n_caps);
            tctx.frame_var            = NULL;
            tctx.return_emitted       = false;

            bool body_void = (try_body->type.kind == TY_NIL ||
                              try_body->type.kind == TY_NEVER ||
                              expr_is_divergent(try_body));
            char *body_ret = NULL;
            if (body_void) {
                emit_stmt(&tctx, &thunk_body, try_body);
            } else {
                body_ret = emit_value(&tctx, &thunk_body, try_body);
            }
            ctx->tmp_n = tctx.tmp_n;

            /* Emit the thunk function to pbuf */
            buf_printf(pbuf, "static void __try_thunk_%d(void *__env, tur_result *__out) {\n", thunk_id);
            if (n_caps > 0) {
                buf_printf(pbuf, "    __try_env_%d *__e = (__try_env_%d *)__env;\n", thunk_id, thunk_id);
            } else {
                buf_puts(pbuf, "    (void)__env;\n");
            }
            if (thunk_body.len > 0)
                buf_write(pbuf, thunk_body.data, thunk_body.len);
            buf_puts(pbuf, "    __out->tag = TUR_RESULT_OK;\n");
            if (body_ret) {
                buf_printf(pbuf, "    __out->u.ok_val = (int64_t)%s;\n", body_ret);
                free(body_ret);
            } else {
                buf_puts(pbuf, "    __out->u.ok_val = 0;\n");
            }
            buf_puts(pbuf, "}\n\n");
            buf_free(&thunk_body);

            /* ---- call site ---- */

            /* Create env struct on stack (or pass NULL) */
            const char *env_arg;
            char *env_var = NULL;
            if (n_caps > 0) {
                env_var = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "__try_env_%d %s = { ", thunk_id, env_var);
                for (uint32_t ci = 0; ci < n_caps; ci++) {
                    if (ci > 0) buf_puts(body, ", ");
                    char *cn = name_for_binding(ctx, caps[ci]);
                    buf_puts(body, cn);
                    free(cn);
                }
                buf_puts(body, " };\n");
                env_arg = env_var;
            } else {
                env_arg = "NULL";
            }
            free(caps);

            /* tur_result + tur_catch_unwind call */
            char *res_var     = fresh_tmp(ctx);
            char *caught_var  = fresh_tmp(ctx);
            char *result_var  = fresh_tmp(ctx);

            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_result %s;\n", res_var);
            indent_buf(body, ctx->indent);
            if (n_caps > 0) {
                buf_printf(body, "bool %s = tur_catch_unwind(__try_thunk_%d, &%s, &%s);\n",
                           caught_var, thunk_id, env_arg, res_var);
            } else {
                buf_printf(body, "bool %s = tur_catch_unwind(__try_thunk_%d, %s, &%s);\n",
                           caught_var, thunk_id, env_arg, res_var);
            }
            free(env_var);

            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s;\n", result_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s) {\n", caught_var);
            free(caught_var);

            ctx->indent += 4;
            if (n_catches > 0) {
                /* Bind catch variable to the panic payload value */
                Binding *cb = catch_binds[0];
                if (cb) {
                    char *cb_name = name_for_binding(ctx, cb);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "int64_t %s = (int64_t)(intptr_t)tur_panic_payload_value(%s.u.err);\n",
                        cb_name, res_var);
                    free(cb_name);
                }
                char *h = emit_value(ctx, body, catch_hdlrs[0]);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = (int64_t)%s;\n", result_var, h);
                free(h);
            } else {
                /* No catch clause: re-panic with the original payload */
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "tur_panic_with(%s.u.err->type_tag, "
                    "(void*)tur_panic_payload_value(%s.u.err), __FILE__, __LINE__);\n",
                    res_var, res_var);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = 0;\n", result_var);
            }
            ctx->indent -= 4;

            indent_buf(body, ctx->indent);
            buf_puts(body, "} else {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s.u.ok_val;\n", result_var, res_var);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            free(res_var);

            return result_var;
        }
        case EX_PANIC_PAYLOAD_TYPE: {
            const Expr *payload = e->as.panic_payload_type_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "(int64_t)tur_panic_payload_type((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_VALUE: {
            const Expr *payload = e->as.panic_payload_value_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "tur_panic_payload_value((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_FILE: {
            const Expr *payload = e->as.panic_payload_file_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "tur_panic_payload_file((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_LINE: {
            const Expr *payload = e->as.panic_payload_line_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "(int64_t)tur_panic_payload_line((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_DOWNS: {
            const Expr *payload = e->as.panic_payload_downs_.payload;
            TypeKind target_type = e->as.panic_payload_downs_.target_type;
            char *payload_var = emit_value(ctx, body, payload);
            free(payload_var);
            (void)target_type; /* unused in v1 */
            return atom_nil();
        }
        /* Phase 18: Delimited continuations */
        case EX_RESET:            return emit_effects_reset(ctx, body, e);
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET:  return emit_effects_cloneable_reset(ctx, body, e);
        case EX_SHIFT:            return emit_effects_shift(ctx, body, e);
        case EX_SHIFT0:           return emit_effects_shift0(ctx, body, e);
        case EX_CALLCC:           return emit_cps_callcc(ctx, body, e);
        case EX_CLONEABLE_SHIFT:  return emit_effects_cloneable_shift(ctx, body, e);
        /* Phase 21: Serializable continuations */
        case EX_SERIAL_RESET:     return emit_effects_serial_reset(ctx, body, e);
        case EX_SERIAL_SHIFT:     return emit_effects_serial_shift(ctx, body, e);
        /* Phase 2 */
        case EX_FN_DEF:
            /* fn should only appear at top level for phase 2 */
            fprintf(stderr, "tur: emit: EX_FN_DEF in value position (nested fn not yet supported)\n");
            abort();
        case EX_FN:
            fprintf(stderr, "tur: emit: EX_FN not yet implemented\n");
            abort();
        case EX_CALL: {
            Binding *fn_binding = e->as.call_.fn_binding;

            /* Phase 16 v2: indirect capability field call — fn_expr is EX_GET_FIELD.
             * Emit: ((ret_t (*)(arg_t, ...))(intptr_t)(struct_val).field_name)(args...)
             * Effect-row annotation is advisory (erased to a plain function pointer).
             * Phase E: when the field is a typed fn ptr (concrete), call directly
             * without the intptr_t round-trip. */
            if (e->as.call_.fn_expr) {
                Expr *gf = e->as.call_.fn_expr;
                char *fn_ptr_val = emit_value(ctx, body, gf);
                const char *ret_c = type_c_name(e->type);

                /* Phase E: detect concrete typed fn-ptr field. */
                bool is_typed_fn_field = false;
                if (gf->kind == EX_GET_FIELD) {
                    StructDef *gf_def = gf->as.get_field_.def;
                    uint32_t  gf_fi   = gf->as.get_field_.field_idx;
                    const StructField *gf_field = &gf_def->fields[gf_fi];
                    if (gf_field->kind == TY_FN && gf_field->full_type &&
                            gf_field->full_type->kind == TY_FN) {
                        is_typed_fn_field =
                            (register_fn_ptr_typedef(gf_field->full_type) != NULL);
                    }
                }
                /* M4-rest (end-to-end-monomorphization-plan): a typeclass
                 * dictionary method slot is already typed in the dict struct
                 * generated by emit_stmt.c, so the dispatch can call the slot
                 * directly: `(dict_X_singleton.method)(args)` rather than
                 * `((ret_t (*)(...))(intptr_t)(dict_X_singleton.method))(args)`.
                 * The intptr_t round-trip is dead weight -- the field type
                 * already matches the call signature.
                 *
                 * Narrow gate: only fire when fn_expr is an EX_DICT carrying a
                 * non-empty method_name (i.e. the EX_DICT node was built to
                 * select a specific slot, not just hand back the singleton
                 * address).  Arg casts still go through the same paths below
                 * (a TY_FN arg destined for an int64_t slot still needs its
                 * fn->int64 cast); this flag only controls the OUTER call-
                 * format selection at line 1758. */
                bool is_direct_dict_dispatch =
                    (gf->kind == EX_DICT
                     && gf->as.dict_.method_name[0] != '\0');
                bool emit_direct_call =
                    is_typed_fn_field || is_direct_dict_dispatch;

                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                bool *arg_cast = (bool *)malloc(e->as.call_.n_args * sizeof(bool));
                if (!arg_cast) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                    /* Apply same function-ptr → int64_t cast as the direct-call path:
                     * C99 forbids implicit conversion from function pointer to integer.
                     * Track which args were cast so the signature uses int64_t, not
                     * void *, for those positions — passing int64_t to void * is
                     * -Wint-conversion error in C99.
                     * Phase E: skip fn cast when field is already typed (no carrier). */
                    bool needs_fn_cast = !is_typed_fn_field &&
                        (e->as.call_.args[i]->type.kind == TY_FN ||
                         e->as.call_.args[i]->type.kind == TY_PTR_VOID);
                    /* KB-012/KB-021: dictionary method pointers use the carrier
                     * ABI (int64_t) for carrier-ABI types (TY_APP / TY_ADT /
                     * parametric struct).  Non-parametric struct instance bodies
                     * use the concrete by-value ABI (KB-031).  Carrier-ABI values
                     * are already int64_t carriers in value position (KB-021), so
                     * no bridge transformation is applied -- but the emitted
                     * fn-pointer signature must still declare the parameter as
                     * int64_t (arg_cast stays set) rather than the concrete
                     * struct typedef. */
                    bool needs_carrier_bridge = !needs_fn_cast && ctx &&
                        type_uses_carrier_in_dispatch(e->as.call_.args[i]->type);
                    arg_cast[i] = needs_fn_cast || needs_carrier_bridge;
                    if (needs_fn_cast) {
                        Buf cast; buf_init(&cast);
                        buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    } else if (needs_carrier_bridge &&
                               expr_emits_byvalue_carrier_abi(ctx, e->as.call_.args[i])) {
                        /* KB-021: only a by-value aggregate (struct literal, by-value
                         * var/param, or concrete spec result) needs bridging to the
                         * carrier; an already-carrier value passes through. */
                        raw = emit_carrier_bridge(ctx, body, raw,
                                                  CK_CONCRETE, CK_CARRIER,
                                                  e->as.call_.args[i]->type);
                    }
                    arg_strs[i] = raw;
                }

                Buf out; buf_init(&out);
                if (emit_direct_call) {
                    /* Phase E / M4-rest: typed fn-ptr or typed dict slot --
                     * call directly, no intptr_t round-trip. */
                    buf_printf(&out, "(%s)(", fn_ptr_val);
                } else {
                    /* Cast function pointer via intptr_t to the right signature. */
                    buf_printf(&out, "((%s (*)(", ret_c);
                    if (e->as.call_.n_args == 0) {
                        buf_puts(&out, "void");
                    } else {
                        for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                            if (i > 0) buf_puts(&out, ", ");
                            /* If we cast this arg to int64_t above, the signature must
                             * say int64_t (not void *) to avoid -Wint-conversion. */
                            if (arg_cast[i]) {
                                buf_puts(&out, "int64_t");
                            } else {
                                buf_puts(&out, type_c_name(e->as.call_.args[i]->type));
                            }
                        }
                    }
                    buf_printf(&out, "))(intptr_t)(%s))(", fn_ptr_val);
                }
                free(arg_cast);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_puts(&out, arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(fn_ptr_val);
                return result;
            }
            
            /* Phase HRT1: poly call through rank-2 fn param: emit fn_name.fn(fn_name.env, arg0, ...) */
            if (e->as.call_.is_poly_call) {
                char *fn_name = emit_call_name(ctx, e, fn_binding);
                uint32_t n = e->as.call_.n_args;

                /* Phase F: Use concrete typed dispatch when all arg/result types are
                 * sub-64-bit integer kinds.  Cast fn.fn to the concrete signature
                 * instead of widening to the int64_t carrier.
                 *
                 * Safety (x86-64 SysV ABI): sub-64-bit integer args are zero-extended
                 * into 64-bit registers by the caller, and the callee truncates back to
                 * the concrete type — so the bit pattern survives the round-trip.
                 * Return values are placed in rax; reading eax (lower 32 bits) yields
                 * the correct narrow integer regardless of sign-extension in rax.
                 * This makes casting fn.fn from int64_t(*)(void*,int64_t) back to the
                 * concrete type correct for both closure thunks (native concrete sig)
                 * and generic poly wrappers (which truncate internally). */
                /* F5 typed `:fn` carrier: when the carrier binding has a concrete
                 * signature (poly_type is a TY_FN, not a forall or NULL), its
                 * stored thunk is natively typed -- a generic poly wrapper that
                 * retypes float args (make_poly_wrapper) or a closure's own
                 * concrete thunk.  Cast fn.fn to the exact R(*)(void*, A...)
                 * signature and pass native-typed args for *every* kind, so
                 * float/cstr/ptr round-trip without int64 truncation or the
                 * pointer<->int64 -Wint-conversion warnings the generic carrier
                 * path emits.  See docs/reported/fn-first-class-float-carrier-gap.md. */
                bool typed_carrier = fn_binding && fn_binding->poly_type &&
                                     fn_binding->poly_type->kind == TY_FN;
                bool phase_f_concrete = !e->as.call_.poly_arg_mask &&
                                        (typed_carrier ||
                                         type_kind_is_poly_concrete(e->type.kind));
                for (uint32_t i = 0; i < n && phase_f_concrete && !typed_carrier; i++) {
                    if (!type_kind_is_poly_concrete(e->as.call_.args[i]->type.kind))
                        phase_f_concrete = false;
                }

                char **arg_strs = n ? (char **)malloc(n * sizeof(char *)) : NULL;
                if (n && !arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < n; i++) {
                    char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                    if (!phase_f_concrete) {
                        /* Generic carrier path: apply poly_arg_mask / needs_cast wrapping */
                        if (e->as.call_.poly_arg_mask & (1u << i)) {
                            /* Phase HRT3: nested poly fn arg — pass by pointer as int64_t */
                            Buf cast; buf_init(&cast);
                            buf_printf(&cast, "(int64_t)(intptr_t)(&(%s))", raw);
                            buf_putc(&cast, '\0');
                            free(raw);
                            raw = strdup(cast.data);
                            buf_free(&cast);
                        } else {
                            /* Cast fn ptrs and void* through intptr_t */
                            bool needs_cast = (e->as.call_.args[i]->type.kind == TY_FN ||
                                               e->as.call_.args[i]->type.kind == TY_PTR_VOID);
                            if (needs_cast) {
                                Buf cast; buf_init(&cast);
                                buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                                buf_putc(&cast, '\0');
                                free(raw);
                                raw = strdup(cast.data);
                                buf_free(&cast);
                            }
                        }
                    }
                    /* Phase F concrete path: args used as-is, no int64_t widening. */
                    arg_strs[i] = raw;
                }

                Buf out; buf_init(&out);
                if (phase_f_concrete) {
                    /* Phase F: cast fn.fn to the concrete signature and call directly */
                    buf_printf(&out, "((%s (*)(void*", emit_type_c_name(ctx, e->type));
                    for (uint32_t i = 0; i < n; i++) {
                        buf_printf(&out, ", %s", emit_type_c_name(ctx, e->as.call_.args[i]->type));
                    }
                    buf_printf(&out, "))%s.fn)(%s.env", fn_name, fn_name);
                    for (uint32_t i = 0; i < n; i++) {
                        buf_printf(&out, ", %s", arg_strs[i]);
                    }
                } else if (n > 1) {
                    /* Generic carrier dispatch, N-ary: the carrier's `fn` field is
                     * typed unary (int64_t(*)(void*,int64_t)), but it points at an
                     * N-ary int64 wrapper/thunk.  Calling it with >1 argument
                     * through the unary prototype is undefined and drops args, so
                     * recast `fn` to the matching int64 N-ary signature first. */
                    buf_printf(&out, "((int64_t(*)(void*");
                    for (uint32_t i = 0; i < n; i++) buf_puts(&out, ", int64_t");
                    buf_printf(&out, "))%s.fn)(%s.env", fn_name, fn_name);
                    for (uint32_t i = 0; i < n; i++) {
                        buf_printf(&out, ", (int64_t)(%s)", arg_strs[i]);
                    }
                } else {
                    /* Generic carrier dispatch, unary: call through the field as-is. */
                    buf_printf(&out, "%s.fn(%s.env", fn_name, fn_name);
                    for (uint32_t i = 0; i < n; i++) {
                        buf_printf(&out, ", (int64_t)(%s)", arg_strs[i]);
                    }
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                free(arg_strs);
                free(fn_name);
                return result;
            }

            if (fn_binding->closure_fn_binding) {
                /* This is a closure - emit call to thunk function with closure value as first arg */
                Binding *thunk_binding = fn_binding->closure_fn_binding;
                char *thunk_name = raw_name_for_binding(thunk_binding);
                
                /* Closure value is the env struct variable. */
                char *closure_val = name_for_binding(ctx, fn_binding);

                char **arg_strs = (char **)malloc((e->as.call_.n_args + 1) * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }

                /* First arg is the closure value (the box) passed as the thunk's
                 * `void *` env param.  CRU B-1: a boxed-TY_FN closure binding is
                 * declared as the int64_t carrier (not void *), so coerce the box
                 * to void * here -- otherwise clang trips -Wint-conversion.  The
                 * cast is a no-op for a TY_PTR_VOID closure binding. */
                Buf env0; buf_init(&env0);
                buf_printf(&env0, "(void *)(intptr_t)(%s)", closure_val);
                buf_putc(&env0, '\0');
                arg_strs[0] = strdup(env0.data);
                buf_free(&env0);
                free(closure_val);
                
                /* Rest of the args */
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                    /* CC2 (curried-call-cast-rough-edges-plan): a let-bound
                     * closure thunk has C parameter slots typed by the thunk's
                     * declared kinds.  When the formal is int64_t (TY_INT or
                     * any other int64_t-carrier kind) and the actual is a
                     * pointer (TY_PTR_VOID closure value or TY_FN), wrap with
                     * the standard (int64_t)(intptr_t) coercion -- otherwise
                     * clang rejects the implicit pointer-to-int conversion.
                     *
                     * vec-typed-fat-closure-readback: a TY_FN formal (a `^fat`
                     * closure param, e.g. `(sf sig)` where `sig` is declared
                     * `(fn [float] float)`) is *also* emitted as the int64_t
                     * carrier (emit_fns.c: closure params of kind TY_FN -> int64_t).
                     * Applying such a closure to a `:ptr<void>` SF box passed the
                     * void* straight into the int64_t slot with no cast, tripping
                     * -Wint-conversion (and "working" only because pointers and
                     * int64 share a width).  Treat TY_FN formals as int64_t
                     * carriers here too. */
                    TypeKind formal = TY_INT;
                    if (thunk_binding->type.kind == TY_FN &&
                        (uint8_t)(i + 1) < thunk_binding->type.as.fn.arity) {
                        formal = thunk_binding->type.as.fn.arg_kinds[i + 1];
                    }
                    TypeKind actual = e->as.call_.args[i]->type.kind;
                    /* SF-application carrier bridge: the thunk's formal slot may be
                     * declared TY_INT *or* TY_FN; both lower to the int64_t carrier
                     * in the lifted body's C signature.  If the actual is a TY_PTR_VOID
                     * (or another TY_FN value held as void *), bridge it through
                     * intptr_t -- otherwise clang trips -Wint-conversion at the call. */
                    bool formal_is_int64_carrier =
                        (formal == TY_INT || formal == TY_FN);
                    bool formal_is_ptr =
                        (formal == TY_PTR_VOID || formal == TY_RC ||
                         formal == TY_REF || formal == TY_WEAK);
                    /* The actual C value is int64_t when:
                     *   - the arg expression is a bare ^fat-bound variable
                     *     (^fat lowers to int64_t even when its elab type is TY_FN), or
                     *   - the arg expression is a let-bound variable whose binding type
                     *     resolved to TY_FN (Turmeric's fn-carrier ABI is int64_t). */
                    const Expr *arg_e = e->as.call_.args[i];
                    bool arg_is_int64_carrier = false;
                    if (arg_e && arg_e->kind == EX_VAR && arg_e->as.var.binding) {
                        Binding *ab = arg_e->as.var.binding;
                        if (ab->is_fat) arg_is_int64_carrier = true;
                        if (ab->type.kind == TY_FN) arg_is_int64_carrier = true;
                    }
                    if (formal_is_int64_carrier &&
                        (actual == TY_PTR_VOID || actual == TY_FN)) {
                        Buf cast; buf_init(&cast);
                        buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    } else if (formal_is_ptr && arg_is_int64_carrier) {
                        /* Reverse direction: formal slot is a pointer (void *),
                         * but the arg's C value is the int64_t carrier (because
                         * the binding holds a function-typed value via the
                         * fn-carrier ABI, or because it was annotated ^fat).
                         * Bridge through intptr_t so the pointer slot accepts it. */
                        Buf cast; buf_init(&cast);
                        buf_printf(&cast, "(void *)(intptr_t)(%s)", raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    }
                    arg_strs[i + 1] = raw;
                }
                
                Buf out; buf_init(&out);
                buf_printf(&out, "%s(", thunk_name);
                for (uint32_t i = 0; i <= e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_printf(&out, "%s", arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i <= e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(thunk_name);
                return result;
            }

            /* Callback call through ptr<void> parameter. */
            if (fn_binding->type.kind == TY_PTR_VOID) {
                char *fn_ptr = name_for_binding(ctx, fn_binding);
                uint32_t n = e->as.call_.n_args;
                if (n == 0 && !fn_binding->is_fat) {
                    /* Raw :ptr<void> callback (NOT a fat sink): treat fn_ptr as a
                     * bare function pointer and call it directly.  CRU B-2: a
                     * *fat* :ptr<void> sink (is_fat -- a ^fat param or a closure
                     * param) instead falls through to the fat-dispatch branch
                     * below, which reads slot 0 of the box.  This is_fat gate is
                     * the disambiguator the nullary :ptr<void> direct call lacked:
                     * a captureless bare fn through a plain :ptr<void> stays thin
                     * (the test-runner-callback case), while a closure/^fat sink
                     * dispatches fat -- both correct at n==0. */
                    Buf out; buf_init(&out);
                    buf_printf(&out, "((%s (*)(void))%s)()",
                               type_c_name(e->type), fn_ptr);
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(fn_ptr);
                    return result;
                } else {
                    /* CY2: Fat-closure dynamic dispatch with n_args > 0.
                     * The fat closure has layout: struct { int64_t __fn; ... }
                     * Extract __fn as a function pointer and call it with fat_ptr + args. */
                    /* poly-closure-result-specialization: inside an inner-body
                     * spec, the dispatched closure's result/arg tyvars resolve
                     * to concrete floats; use emit_resolve_type so the typedef
                     * cast is xmm0-correct.  Identity outside a spec. */
                    Type _disp_result = emit_resolve_type(ctx, e->type);
                    const char *ret_c = type_c_name(_disp_result);
                    Type arg_types[MAX_FN_ARITY];
                    char **arg_strs = (char **)malloc(n * sizeof(char *));
                    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                    for (uint32_t i = 0; i < n; i++) {
                        arg_types[i] = emit_resolve_type(ctx, e->as.call_.args[i]->type);
                        arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                    }
                    char *thunk_typedef = ensure_typed_thunk_typedef(ctx, ctx->file,
                        _disp_result, n > 0 ? arg_types : NULL, (uint8_t)n);
                    Buf out; buf_init(&out);
                    if (thunk_typedef) {
                        /* TS1: typed fat-closure layout stores __fn as a typed function pointer. */
                        buf_printf(&out, "(*( %s *)(%s))(%s", thunk_typedef, fn_ptr, fn_ptr);
                    } else {
                        /* Legacy fallback: polymorphic fat closures still store __fn as int64_t. */
                        buf_printf(&out, "((%s (*)(void*", ret_c);
                        for (uint32_t i = 0; i < n; i++) {
                            buf_printf(&out, ", %s", type_c_name(arg_types[i]));
                        }
                        buf_printf(&out, "))(intptr_t)((int64_t *)(%s))[0])(%s", fn_ptr, fn_ptr);
                    }
                    for (uint32_t i = 0; i < n; i++) {
                        /* SF-application carrier bridge (fat-dispatch arg slot):
                         * a ^fat-typed binding is lowered to int64_t in C even
                         * though its elaborator type is TY_FN ("void *" per
                         * type_c_name).  The typed-thunk-typedef cast expects
                         * "void *" at each arg slot, so a bare ^fat variable
                         * reference must be bridged with (void *)(intptr_t)
                         * to dodge clang's -Wint-conversion. */
                        const Expr *arg = e->as.call_.args[i];
                        bool needs_ptr_cast = false;
                        if (arg && arg->kind == EX_VAR && arg->as.var.binding &&
                            arg->as.var.binding->is_fat &&
                            (arg->type.kind == TY_FN || arg->type.kind == TY_PTR_VOID)) {
                            needs_ptr_cast = true;
                        }
                        if (needs_ptr_cast) {
                            buf_printf(&out, ", (void *)(intptr_t)(%s)", arg_strs[i]);
                        } else {
                            buf_printf(&out, ", %s", arg_strs[i]);
                        }
                    }
                    buf_puts(&out, ")");
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(thunk_typedef);
                    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                    free(arg_strs);
                    free(fn_ptr);
                    return result;
                }
            }
            
            /* ER2: Callback call through a local TY_FN parameter.
             * When a function parameter is annotated with :(fn [...] #{} :T), it is
             * stored as int64_t in C (a function address or closure pointer cast to
             * int).  Calling it requires the same cast-and-invoke pattern as TY_PTR_VOID
             * callbacks.  Only applies to non-global bindings (local params). */
            if (fn_binding->type.kind == TY_FN && !fn_binding->is_global) {
                /* A#1: a ^fat parameter holds a fat closure ({ thunk, env... }),
                 * not a bare function pointer.  Invoking it directly via (g x)
                 * must dispatch through slot 0 with the box as the env argument
                 * -- the same fat-call protocol as the TY_PTR_VOID path above.
                 * Emitting a thin ((R (*)(A...))g)(args) call here would treat the
                 * fat box's address as code and jump to garbage. */
                if (fn_binding->is_fat || fn_binding->type.as.fn.boxed) {
                    char *raw_ptr = name_for_binding(ctx, fn_binding);
                    /* A TY_FN parameter is stored as int64_t in C; the fat-call
                     * protocol wants the box as a void *, so coerce once. */
                    Buf fnb; buf_init(&fnb);
                    buf_printf(&fnb, "(void *)(intptr_t)(%s)", raw_ptr);
                    buf_putc(&fnb, '\0');
                    char *fn_ptr = strdup(fnb.data);
                    buf_free(&fnb);
                    free(raw_ptr);
                    uint32_t n = e->as.call_.n_args;
                    /* poly-closure-inner-dispatch-result-erased (Direction 3):
                     * e->type is the int64 carrier (erased) when the call is
                     * inside a generic closure body.  If the callee binding is a
                     * typed (fn [..] R) parameter whose result_full_type carries a
                     * named TY_TYVAR, resolve it through the current spec to
                     * recover the concrete C return type (e.g. double for float). */
                    Type disp_result = emit_resolve_type(ctx, e->type);
                    if (type_uses_carrier_abi(disp_result) &&
                        fn_binding->type.kind == TY_FN &&
                        fn_binding->type.as.fn.result_full_type) {
                        Type rfull_resolved = emit_resolve_type(ctx,
                            *fn_binding->type.as.fn.result_full_type);
                        if (!type_uses_carrier_abi(rfull_resolved))
                            disp_result = rfull_resolved;
                    }
                    const char *ret_c = type_c_name(disp_result);
                    Type arg_types[MAX_FN_ARITY];
                    char **arg_strs = (n > 0)
                        ? (char **)malloc(n * sizeof(char *)) : NULL;
                    if (n > 0 && !arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                    /* two-level-sf-closure: the fat-dispatch thunk cast must follow
                     * the CALLEE's declared parameter ABI, not the actual argument's
                     * static type.  Calling `(sf sig)` where sf is
                     * `^fat sf :(fn [:ptr<void>] :ptr<void>)` and sig is itself a
                     * `^fat sig :(fn [:float] :float)` must cast slot 0 to
                     * `R(*)(void*, void*)` (sf's :ptr<void> param), NOT
                     * `R(*)(void*, double)` derived from sig's :float thunk signature
                     * -- the latter passes the fat-box pointer in an FP register and
                     * corrupts the call.  Prefer sf's declared arg_full_types /
                     * arg_kinds; fall back to the argument's type only when the callee
                     * carries no per-arg type info. */
                    bool have_decl_args = (fn_binding->type.kind == TY_FN &&
                                           fn_binding->type.as.fn.arity == n);
                    for (uint32_t i = 0; i < n; i++) {
                        Type *decl = NULL;
                        if (have_decl_args && fn_binding->type.as.fn.arg_full_types)
                            decl = fn_binding->type.as.fn.arg_full_types[i];
                        if (decl) {
                            /* Direction 3: resolve TY_TYVAR arg types through spec. */
                            arg_types[i] = emit_resolve_type(ctx, *decl);
                        } else if (have_decl_args) {
                            arg_types[i] = emit_type_from_kind(fn_binding->type.as.fn.arg_kinds[i]);
                        } else {
                            arg_types[i] = e->as.call_.args[i]->type;
                        }
                        arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                    }
                    char *thunk_typedef = ensure_typed_thunk_typedef(ctx, ctx->file,
                        disp_result, n > 0 ? arg_types : NULL, (uint8_t)n);
                    Buf out; buf_init(&out);
                    if (thunk_typedef) {
                        /* TS1: typed fat-closure layout -- slot 0 is a typed thunk ptr. */
                        buf_printf(&out, "(*( %s *)(%s))(%s", thunk_typedef, fn_ptr, fn_ptr);
                    } else {
                        buf_printf(&out, "((%s (*)(void*", ret_c);
                        for (uint32_t i = 0; i < n; i++) {
                            buf_printf(&out, ", %s", type_c_name(arg_types[i]));
                        }
                        buf_printf(&out, "))(intptr_t)((int64_t *)(%s))[0])(%s", fn_ptr, fn_ptr);
                    }
                    for (uint32_t i = 0; i < n; i++) {
                        /* SF-application carrier bridge (decl-arg-typed fat-dispatch):
                         * the thunk-typedef cast follows the callee's DECLARED arg
                         * types (arg_types[i] above), so each slot's C type is
                         * `type_c_name(arg_types[i])`.  A bare ^fat-bound variable
                         * is held as int64_t in C even when its elab type is TY_FN
                         * (lowers to "void *") or TY_PTR_VOID -- bridge it through
                         * (void *)(intptr_t) so clang accepts the pointer slot. */
                        const Expr *arg = e->as.call_.args[i];
                        bool slot_is_ptr =
                            (arg_types[i].kind == TY_PTR_VOID ||
                             arg_types[i].kind == TY_RC ||
                             arg_types[i].kind == TY_REF ||
                             arg_types[i].kind == TY_WEAK ||
                             (arg_types[i].kind == TY_FN &&
                              arg_types[i].as.fn.boxed));
                        bool var_is_int64_carrier =
                            arg && arg->kind == EX_VAR && arg->as.var.binding &&
                            arg->as.var.binding->is_fat;
                        if (slot_is_ptr && var_is_int64_carrier) {
                            buf_printf(&out, ", (void *)(intptr_t)(%s)", arg_strs[i]);
                        } else {
                            buf_printf(&out, ", %s", arg_strs[i]);
                        }
                    }
                    buf_puts(&out, ")");
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(thunk_typedef);
                    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                    free(arg_strs);
                    free(fn_ptr);
                    return result;
                }
                char *fn_ptr = name_for_binding(ctx, fn_binding);
                uint32_t n = e->as.call_.n_args;
                const char *ret_c = type_c_name(e->type);
                /* two-level-sf-closure-return-miscompiles-out-binding: when this
                 * thin local fn returns a *function value* (e.g. (sf input) where
                 * sf : (fn [sig] (fn [t] float))), the result is a concrete thin
                 * function pointer, not a value of the inner result kind.
                 * type_c_name(TY_FN) collapses a non-boxed primitive-result fn to
                 * its result type's C name ("double"), so the cast would claim the
                 * call returns `double` while it actually returns
                 * `double (*)(double)` -- a hard `cc` "incompatible types" error at
                 * the let binding that captures it.  Type the cast's return as the
                 * matching fn-ptr typedef instead (a boxed closure result already
                 * lowers to "void *" above and is left untouched). */
                if (e->type.kind == TY_FN && !e->type.as.fn.boxed) {
                    const char *ret_td = register_fn_ptr_typedef(&e->type);
                    if (ret_td) ret_c = ret_td;
                }
                if (n == 0) {
                    Buf out; buf_init(&out);
                    buf_printf(&out, "((%s (*)(void))(intptr_t)%s)()",
                               ret_c, fn_ptr);
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(fn_ptr);
                    return result;
                } else {
                    char **arg_strs = (char **)malloc(n * sizeof(char *));
                    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                    for (uint32_t i = 0; i < n; i++) {
                        arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                    }
                    Buf out; buf_init(&out);
                    buf_printf(&out, "((%s (*)(", ret_c);
                    for (uint32_t i = 0; i < n; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        buf_puts(&out, type_c_name(e->as.call_.args[i]->type));
                    }
                    buf_printf(&out, "))(intptr_t)%s)(", fn_ptr);
                    for (uint32_t i = 0; i < n; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        /* closure-carrier-return-and-arg-int-pointer-warnings:
                         * when the two-level-SF fix retypes an already-fat arg to
                         * the :ptr<void> carrier, the formal param lowers to a
                         * pointer C type ("void *") but the actual arg's C value
                         * is the int64_t closure carrier -- drive's `input` is a
                         * fat fn-typed parameter whose C variable is declared
                         * int64_t (TY_FN params emit as int64_t).  Bridge with the
                         * standard (<ptr>)(intptr_t) coercion -- the same cast the
                         * fat thunk path uses for the box -- otherwise clang trips
                         * -Wint-conversion ("pointer from integer").  Restricted to
                         * a bare variable reference whose binding lowers to the
                         * int64_t carrier: a call/expression that already yields a
                         * pointer (e.g. make_hyadder() returning void*) needs no
                         * cast, so this avoids gratuitous casts on those args. */
                        const Expr *arg = e->as.call_.args[i];
                        const char *pty = type_c_name(arg->type);
                        size_t plen = strlen(pty);
                        bool ptr_formal = plen > 0 && pty[plen - 1] == '*';
                        bool var_is_int64_carrier =
                            arg->kind == EX_VAR && arg->as.var.binding &&
                            (arg->as.var.binding->type.kind == TY_FN ||
                             arg->as.var.binding->type.kind == TY_INT);
                        if (ptr_formal && var_is_int64_carrier) {
                            buf_printf(&out, "(%s)(intptr_t)(%s)", pty, arg_strs[i]);
                        } else {
                            buf_puts(&out, arg_strs[i]);
                        }
                    }
                    buf_puts(&out, ")");
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                    free(arg_strs);
                    free(fn_ptr);
                    return result;
                }
            }

            /* Phase G0: 0-arg constructor call — emit ctor_Name() */
            if (fn_binding->type.kind == TY_ADT) {
                char *_mc = mangle_field_name(fn_binding->name->name);
                /* TS4P2: use per-instance ctor if the call result is a concrete ADT app */
                char *suffix = (e->type.kind == TY_APP)
                    ? type_adt_app_ctor_suffix(e->type) : NULL;
                Buf out; buf_init(&out);
                if (suffix) {
                    buf_printf(&out, "ctor_%s%s()", _mc, suffix);
                    free(suffix);
                } else {
                    buf_printf(&out, "ctor_%s()", _mc);
                }
                free(_mc);
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                return result;
            }

            /* Phase G0: N-arg constructor call — fn has TY_FN w/ result TY_ADT,
             * but the fn name is the constructor name so we emit ctor_Name(args).
             * Phase G3: result_full_type is set for non-constructor ADT-returning
             * functions (e.g. equal-sym) — those must fall through to regular calls.
             * TS4P2: use per-instance ctor when the call result is a concrete ADT app. */
            if (fn_binding->type.kind == TY_FN &&
                fn_binding->type.as.fn.result_kind == TY_ADT &&
                !fn_binding->type.as.fn.result_full_type) {
                /* TS4P2: choose per-instance ctor name if the result is a concrete ADT app */
                char *suffix = (e->type.kind == TY_APP)
                    ? type_adt_app_ctor_suffix(e->type) : NULL;
                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    const Expr *arg = e->as.call_.args[i];
                    /* TS4P2: For a monomorphised constructor, unwrap any EX_REINTERPRET
                     * that was added to box a concrete type (e.g. float) into int64_t.
                     * The monomorphised ctor accepts the concrete type directly. */
                    if (suffix && arg && arg->kind == EX_REINTERPRET &&
                        arg->as.reinterpret_.target_kind == TY_INT &&
                        arg->as.reinterpret_.expr) {
                        arg = arg->as.reinterpret_.expr;
                    }
                    arg_strs[i] = emit_value(ctx, body, arg);
                }
                char *_mc = mangle_field_name(fn_binding->name->name);
                Buf out; buf_init(&out);
                if (suffix) {
                    buf_printf(&out, "ctor_%s%s(", _mc, suffix);
                    free(suffix);
                } else {
                    buf_printf(&out, "ctor_%s(", _mc);
                }
                free(_mc);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_puts(&out, arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                return result;
            }

            /* Regular function call */
            /* Emit function call: fn(arg1, arg2, ...) */
            /* Use raw name for function calls (functions are defined with raw names) */
            char *fn_name = emit_call_name(ctx, e, fn_binding);
            const EmitAbiSpecialization *matched_spec =
                find_matched_abi_spec(ctx, e, fn_binding);
            char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
            if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *arg_expr = e->as.call_.args[i];
                /* M5 residual-straddle (docs/upcoming/m5-residual-straddle-
                 * retirement.md): the strip below historically erased
                 * EX_ASCRIBE wrappers before emit_value so the call could
                 * see the underlying value directly.  But a Path A spec
                 * body uses `(:: x :int)` to widen a by-value carrier-ABI
                 * param (e.g. Vec__int) to the int64 carrier for a downstream
                 * carrier-helper call (vec-get, etc.).  Stripping the
                 * EX_ASCRIBE bypassed the CK_CONCRETE -> CK_CARRIER bridge
                 * at emit_expr.c:4393, so the by-value value reached the
                 * int64 formal as-is and cc failed.  Preserve the EX_ASCRIBE
                 * wrapper when it's the kind that would trigger the byval-
                 * to-carrier bridge: ascribe target is TY_INT, inner is a
                 * by-value carrier-ABI EX_VAR (a spec param), and the
                 * inner's elab-time type uses carrier ABI. */
                bool preserve_ascribe_for_bridge = false;
                if (arg_expr && arg_expr->kind == EX_ASCRIBE &&
                    arg_expr->type.kind == TY_INT &&
                    arg_expr->as.ascribe_.inner &&
                    arg_expr->as.ascribe_.inner->type.kind != TY_INT &&
                    type_uses_carrier_abi(arg_expr->as.ascribe_.inner->type) &&
                    expr_emits_byvalue_carrier_abi(ctx, arg_expr->as.ascribe_.inner)) {
                    preserve_ascribe_for_bridge = true;
                }
                if (!preserve_ascribe_for_bridge) {
                    while (arg_expr && arg_expr->kind == EX_ASCRIBE) arg_expr = arg_expr->as.ascribe_.inner;
                }
                const Expr *emit_arg = arg_expr;
                if (matched_spec && arg_expr && arg_expr->kind == EX_REINTERPRET &&
                    arg_expr->as.reinterpret_.expr &&
                    type_eq(matched_spec->arg_types[i], arg_expr->as.reinterpret_.expr->type)) {
                    emit_arg = arg_expr->as.reinterpret_.expr;
                }
                char *raw = emit_value(ctx, body, emit_arg);
                /* Phase HKT H3/H4: When a function-reference (TY_FN) is passed to an
                 * int64_t parameter, emit an explicit (int64_t)(intptr_t) cast so the
                 * generated C99 code is valid.  Function pointers cannot be implicitly
                 * converted to integers in C99; a reinterpret via intptr_t is needed. */
                /* Phase HKT §5: also cast TY_PTR_VOID (capturing-closure env ptr) to
                 * int64_t when passing to an int64_t parameter. */
                /* Phase HRT1: EX_POLY_WRAP emits a tur_poly_fn_t struct literal;
                 * it must NOT be cast — pass directly as tur_poly_fn_t to poly params. */
                /* The original arg may be an `(:: <closure> int)` ascription
                 * stripped above to its inner fn/closure value -- so the visible
                 * arg type is TY_INT but the emitted value is a fn/void* pointer.
                 * Consult the *stripped* emit_arg's type too, otherwise a closure
                 * stashed into an int64 carrier slot (`(some (:: (fn ...) int))`)
                 * reaches a carrier param uncast and trips -Wint-conversion.  See
                 * the residual under root cause C of
                 * docs/reported/m5-suite-residual-6-failures-2026-06-14.md. */
                bool emit_arg_is_fnptr = emit_arg &&
                    (emit_arg->type.kind == TY_FN || emit_arg->type.kind == TY_PTR_VOID);
                bool needs_fn_cast = (e->as.call_.args[i]->kind != EX_POLY_WRAP) &&
                                     (e->as.call_.args[i]->type.kind == TY_FN ||
                                      e->as.call_.args[i]->type.kind == TY_PTR_VOID ||
                                      emit_arg_is_fnptr);
                /* When param expects void * (TY_PTR_VOID), cast to void * not int64_t.
                 * Passing int64_t to void * is invalid in C99 (-Wint-conversion error). */
                bool cast_to_void_ptr = false;
                /* typed-c-abi-function-pointers: when the callee param is a
                 * cfnptr (declared `R (*)(A...)`), the fn argument must be cast
                 * through that typedef -- a raw (int64_t) cast would be an
                 * int->pointer mismatch in C.  NULL = use the int64_t/void*
                 * carrier cast as before. */
                const char *fn_cast_typedef = NULL;
                /* Phase P3: TY_INT (int64_t) arg passed to a TY_PTR_VOID (void*) param
                 * requires (void*)(intptr_t) coercion. Occurs when persistent-map
                 * lowering passes a map handle (int64_t) to hamt/count etc. */
                if (!needs_fn_cast && fn_binding->type.kind == TY_FN) {
                    uint8_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i : (uint32_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind _pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    /* Bridge int64_t carrier to pointer when callee param is a pointer
                     * type (TY_PTR_VOID, TY_RC, TY_REF, TY_WEAK).  Check the stripped
                     * emit_arg type (after EX_ASCRIBE removal) since the outer type
                     * may have been promoted to the concrete type by the elaborator. */
                    bool _emit_is_int = (emit_arg && (emit_arg->type.kind == TY_INT
                                                      || emit_arg->type.kind == TY_EXISTS
                                                      || emit_arg->type.kind == TY_FORALL));
                    bool _arg_is_int = (e->as.call_.args[i]->type.kind == TY_INT
                                        || e->as.call_.args[i]->type.kind == TY_EXISTS
                                        || e->as.call_.args[i]->type.kind == TY_FORALL);
                    if ((_emit_is_int || _arg_is_int) &&
                        (_pk == TY_PTR_VOID || _pk == TY_RC || _pk == TY_REF || _pk == TY_WEAK)) {
                        needs_fn_cast = true;
                        cast_to_void_ptr = true;
                    }
                }
                if (needs_fn_cast && fn_binding->type.kind == TY_FN) {
                    uint8_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i : (uint32_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    /* Apply cast when param is int64_t in C: TY_INT, opaque TY_STRUCT
                     * (HKT container). */
                    if (pk == TY_INT || pk == TY_STRUCT) {
                        needs_fn_cast = true;
                        cast_to_void_ptr = false;
                    } else if (pk == TY_PTR_VOID || pk == TY_RC
                               || pk == TY_REF || pk == TY_WEAK) {
                        /* Param is a pointer type — cast to void * via intptr_t.
                         * Needed when a TY_FN/int64_t carrier is passed to a pointer param. */
                        needs_fn_cast = true;
                        cast_to_void_ptr = true;
                    } else if (pk == TY_FN) {
                        /* typed-c-abi-function-pointers: a cfnptr param is the
                         * concrete `R (*)(A...)` typedef, not the int64_t
                         * carrier; cast the (captureless) fn argument through
                         * that typedef so the emitted C is well-typed. */
                        const Type *pft = fn_binding->type.as.fn.arg_full_types
                            ? fn_binding->type.as.fn.arg_full_types[param_idx] : NULL;
                        if (pft && pft->kind == TY_FN && pft->as.fn.cfnptr) {
                            const char *td = register_fn_ptr_typedef(pft);
                            if (td) {
                                fn_cast_typedef = td;
                                needs_fn_cast = true;
                            } else {
                                needs_fn_cast = true;
                                cast_to_void_ptr = false;
                            }
                        } else {
                            /* ER2: TY_FN parameter — stored as int64_t in C.  A function
                             * reference (TY_FN) or closure pointer (TY_PTR_VOID) passed to it
                             * must be cast to int64_t via intptr_t. */
                            needs_fn_cast = true;
                            cast_to_void_ptr = false;
                        }
                    } else if ((pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS)
                               && (fn_binding->body_is_inline_c || !matched_spec)) {
                        /* Polymorphic param emitted as the int64 carrier: an
                         * inline-C body keeps `val` typed int64_t regardless of A,
                         * and the generic (non-spec, !matched_spec) emit of a
                         * make-struct/carrier body likewise declares the param
                         * int64_t (e.g. `static int64_t some(int64_t)`).  A
                         * TY_FN/TY_PTR_VOID actual (a closure stashed into the
                         * carrier slot) must be bridged through (int64_t)(intptr_t)
                         * -- otherwise clang trips -Wint-conversion.  A matched
                         * spec resolves A to a concrete C type, so it keeps the
                         * existing (no-cast) handling. */
                        needs_fn_cast = true;
                        cast_to_void_ptr = false;
                    } else {
                        needs_fn_cast = false;
                    }
                }
                /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: an
                 * inline-C-bodied callee declares its ^fat param as an int64_t
                 * carrier (so the C body can treat it as a handle without a
                 * void*->int64_t warning), so the argument must be coerced to
                 * int64_t rather than the old void* up-cast.  Only inline-C
                 * bodies opt into the int64_t spelling; ordinary bodies keep the
                 * void* fat carrier and the typeclass-dictionary slot types. */
                if (needs_fn_cast && fn_binding->type.kind == TY_FN &&
                    fn_binding->body_is_inline_c) {
                    uint8_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i : (uint32_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    if (fn_binding->type.as.fn.arg_fat[param_idx])
                        cast_to_void_ptr = false;
                }
                if (needs_fn_cast) {
                    Buf cast; buf_init(&cast);
                    if (fn_cast_typedef) {
                        /* typed-c-abi-function-pointers: cast through the bare
                         * `R (*)(A...)` typedef (a captureless fn's C name
                         * decays to a function pointer of that exact shape). */
                        buf_printf(&cast, "(%s)(%s)", fn_cast_typedef, raw);
                    } else if (cast_to_void_ptr) {
                        buf_printf(&cast, "(void *)(intptr_t)(%s)", raw);
                    } else {
                        buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                    }
                    buf_putc(&cast, '\0');
                    free(raw);
                    raw = strdup(cast.data);
                    buf_free(&cast);
                }
                /* Phase HRT3: arg is a nested poly fn passed via int64_t pointer —
                 * dereference it back to tur_poly_fn_t for the direct call. */
                if (e->as.call_.poly_arg_mask & (1u << i)) {
                    Buf cast; buf_init(&cast);
                    buf_printf(&cast, "*(tur_poly_fn_t*)(intptr_t)(%s)", raw);
                    buf_putc(&cast, '\0');
                    free(raw);
                    raw = strdup(cast.data);
                    buf_free(&cast);
                }
                /* ACB (KB-004): when a specialized call expects a concrete aggregate
                 * argument but the emitted value is a carrier (int64_t), bridge it
                 * here.  Skip when needs_fn_cast already applied a different coercion.
                 *
                 * Prereq 2 (typeclass-method-parameterized-result-carrier-mismatch.md):
                 * also fire when the emit_arg is an aggregate type that uses the
                 * carrier ABI at C level (e.g. a parameterized struct like
                 * `(Result int cstr)`), is NOT a by-value producer, AND the spec
                 * wants the concrete by-value form.  Without this extension, the
                 * bridge missed the canonical `ok-val (:: (typeclass-method ...)
                 * (Result A B))` shape -- the ascription's inner is a TY_APP at
                 * the elab level but a uniform int64_t carrier at C level, so the
                 * old TY_INT-only gate skipped a needed unbox. */
                if (!needs_fn_cast && matched_spec &&
                    emit_arg &&
                    type_kind_is_aggregate(matched_spec->arg_types[i].kind) &&
                    (emit_arg->type.kind == TY_INT ||
                     (type_kind_is_aggregate(emit_arg->type.kind) &&
                      type_uses_carrier_abi(emit_arg->type) &&
                      !expr_emits_byvalue_carrier_abi(ctx, emit_arg)))) {
                    raw = emit_carrier_bridge(ctx, body, raw,
                                             CK_CARRIER, CK_CONCRETE,
                                             matched_spec->arg_types[i]);
                }
                /* Phase D / tuplen-struct-param: a pass-by-pointer struct
                 * *parameter* is materialized in C as `const T*`, but several
                 * callee shapes take the struct *by value* even when it crosses
                 * the >16-byte pass-by-ptr threshold:
                 *   - an ABI specialization (concrete-by-value clone, e.g. a
                 *     `tupleN-Nth` accessor) -- matched_spec;
                 *   - an inline-C body (declares struct params by value, DS1);
                 *   - an extern-C function (C ABI, by value).
                 * The carrier bridge above only fires for an int64_t carrier
                 * arg; a pbp param is a real pointer, not a carrier, so deref it
                 * to pass a by-value copy and match the callee's formal.
                 * Without this, a Tuple3+ parameter forwarded to such a callee
                 * emitted `callee(const T*)` against a by-value formal -- a hard
                 * cc type error.
                 *
                 * expr_is_pbp_param is checked first: it is side-effect-free,
                 * whereas type_struct_pass_by_ptr registers the struct app as a
                 * side effect (types.c:register_struct_app), which would emit a
                 * spurious typedef for non-pbp aggregate args. A genuine pbp
                 * param's struct type is already registered (its signature was
                 * emitted with pass-by-ptr), so the guard adds no registration. */
                else if (!needs_fn_cast && emit_arg &&
                         expr_is_pbp_param(ctx, emit_arg) &&
                         (matched_spec
                          ? type_kind_is_aggregate(matched_spec->arg_types[i].kind)
                          : (fn_binding->body_is_inline_c
                             || fn_binding->is_extern_c)) &&
                         type_struct_pass_by_ptr(e->as.call_.args[i]->type)) {
                    Buf _db; buf_init(&_db);
                    buf_printf(&_db, "(*(%s))", raw);
                    buf_putc(&_db, '\0');
                    free(raw);
                    raw = strdup(_db.data);
                    buf_free(&_db);
                }
                /* Phase H §1: direct call to a typeclass instance impl (dict_arg
                 * is set by elab to mark statically-resolved typeclass dispatch).
                 * Bridge concrete aggregate → carrier so the impl reads the
                 * heap-pointer carrier it expects.  Mirrors the carrier_bridge
                 * applied on the dict-dispatch path.  Gated on dict_arg so it
                 * does not fire on ordinary direct calls inside instance bodies,
                 * where the receiver var is already int64_t at the C level even
                 * though its elab type is TY_APP/TY_ADT.  Also require the
                 * post-strip emit_arg type to be aggregate (concrete struct/ADT/
                 * APP) — when emit_arg->type is TY_INT, the EX_ASCRIBE was
                 * stripped above and the value is already a carrier int64_t,
                 * which the impl accepts as-is. */
                /* KB-021: after standardising carrier-ABI values on the int64_t
                 * carrier, only a by-value struct literal still needs bridging
                 * here; a plain var / call result / temp is already a carrier
                 * int64_t that the impl accepts as-is. */
                if (!needs_fn_cast && !matched_spec &&
                    e->as.call_.dict_arg != NULL &&
                    emit_arg && type_kind_is_aggregate(emit_arg->type.kind) &&
                    type_kind_is_aggregate(e->as.call_.args[i]->type.kind) &&
                    type_uses_carrier_in_dispatch(e->as.call_.args[i]->type) &&
                    expr_emits_byvalue_carrier_abi(ctx, emit_arg) &&
                    fn_binding->type.kind == TY_FN &&
                    i < fn_binding->type.as.fn.arity &&
                    fn_binding->type.as.fn.arg_kinds[i] == TY_INT) {
                    raw = emit_carrier_bridge(ctx, body, raw,
                                             CK_CONCRETE, CK_CARRIER,
                                             e->as.call_.args[i]->type);
                }
                /* M4c Path A (RETIRED -- M5 D.4, end-to-end-monomorphization
                 * plan): a by-value spec body that called an int64-carrier-sink
                 * stdlib helper (e.g. `(vec-len x)` / `(vec-get x i)` inside an
                 * `Eq Vec` Path A spec) used to spill the by-value `Vec__int`
                 * receiver to a temp and pass `&temp` as an int64 carrier here.
                 * Option C (the by-value twin redirect in
                 * emit_abi_try_byval_twin_redirect, emit_module.c) now retargets
                 * such calls to the helper's `*-byval` twin spec at the call
                 * boundary, so the carrier never has to be re-formed and this
                 * `CK_CONCRETE -> CK_CARRIER` site has zero producers. Verified
                 * by an emit-c sweep over the full fixture suite (0 firings).
                 * The branch is deleted outright; if a future carrier helper
                 * lacks a by-value twin the consistency-gated redirect simply
                 * leaves the call alone, which is a clean elaborator error
                 * rather than a silent re-spill. */
                /* Phase D: large struct args must be passed as const T*.
                 * Only apply when the CALLEE also uses pass-by-ptr for that param
                 * (i.e., was compiled with Phase D signatures). Generic/typeclass
                 * callees use int64_t; passing a pointer there would be wrong.
                 * If the arg is already a pbp param (already a pointer), pass as-is.
                 * Otherwise spill to a temp local and take its address. */
                /* Phase D: check whether the callee's i-th param uses pass-by-ptr.
                 * arg_full_types is only set for poly/rank-2 params; for ordinary
                 * struct params fall back to arg_kinds: TY_STRUCT/TY_APP means the
                 * callee has a concrete struct param (same type as the arg by type
                 * safety), so pbp applies iff the arg itself is pbp.
                 * TY_INT / TY_TYVAR means generic/opaque int64_t -- no pbp. */
                bool _callee_pbp = false;
                if (fn_binding->type.kind == TY_FN && i < fn_binding->type.as.fn.arity) {
                    if (fn_binding->type.as.fn.arg_full_types &&
                            fn_binding->type.as.fn.arg_full_types[i]) {
                        _callee_pbp = type_struct_pass_by_ptr(
                            *fn_binding->type.as.fn.arg_full_types[i]);
                    } else {
                        TypeKind _cpk = fn_binding->type.as.fn.arg_kinds[i];
                        _callee_pbp = (_cpk == TY_STRUCT || _cpk == TY_APP) &&
                            type_struct_pass_by_ptr(e->as.call_.args[i]->type);
                    }
                }
                /* DS1: An inline-C callee declares its struct params by
                 * value even when they cross the pass-by-ptr threshold
                 * (emit_fns.c:423 / emit_module.c:1105). The call site
                 * must respect that and skip the `&temp` wrap, otherwise
                 * we pass `T *` to a formal of type `T`. Mirrors the
                 * formal-side `!body_is_inline_c` guard via the
                 * binding-cached flag set in elab_fns.c. */
                if (!needs_fn_cast && !fn_binding->is_extern_c &&
                    !fn_binding->body_is_inline_c &&
                    !matched_spec &&
                    _callee_pbp &&
                    type_struct_pass_by_ptr(e->as.call_.args[i]->type) &&
                    !expr_is_pbp_param(ctx, emit_arg)) {
                    char *_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = %s;\n",
                               emit_type_c_name(ctx, e->as.call_.args[i]->type), _tmp, raw);
                    free(raw);
                    Buf _ab; buf_init(&_ab);
                    buf_printf(&_ab, "&%s", _tmp);
                    buf_putc(&_ab, '\0');
                    raw = strdup(_ab.data);
                    buf_free(&_ab);
                    free(_tmp);
                }
                arg_strs[i] = raw;
            }
            Buf out; buf_init(&out);
            buf_printf(&out, "%s(", fn_name);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (i > 0) buf_puts(&out, ", ");
                buf_printf(&out, "%s", arg_strs[i]);
            }
            buf_puts(&out, ")");
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
            free(arg_strs);
            free(fn_name);
            return result;
        }
        case EX_EXTERN_C: {
            /* extern-c declarations emit nothing in value position (they're file-scope) */
            return atom_nil();
        }
        case EX_INLINE_C: {
            /* Return the inline C code as a value string (with __TUR_CAP_N__ /
             * __TUR_VAL_N__ substitution).  Statement-position use goes through
             * emit_stmt which handles EX_INLINE_C separately. */
            InlineC *ic = e->as.inline_c_.inline_c;
            return inline_c_substitute(ctx, body, ic);
        }
        /* Phase 3 */
        /* Phase 4 */
        case EX_DEFER: {
            /* Defer in value position: emit as nil (defer evaluates to nil) */
            /* The body is emitted as a statement in emit_stmt */
            return atom_nil();
        }
        case EX_RETURN: {
            /* (return expr) or (return) - in value position, just emit the value expression
             * The return statement itself will be emitted by emit_stmt */
            if (e->as.return_.value) {
                return emit_value(ctx, body, e->as.return_.value);
            } else {
                /* return with no value - this is only valid in void functions */
                /* Emit a placeholder */
                return atom_nil();
            }
        }
        /* Phase 5 */
        case EX_REF: {
            /* (ref expr) - allocate on heap and return pointer */
            /* ref<T> lowers to void* in C for simplicity in v1 */
            char *inner = emit_value(ctx, body, e->as.ref_.expr);
            
            /* Emit: malloc + store value */
            char *tmp = fresh_tmp(ctx);
            char *inner_type_c = strdup(type_c_name(e->as.ref_.expr->type));
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = malloc(sizeof(%s));\n", 
                       type_c_name(e->type), tmp, inner_type_c);
            indent_buf(body, ctx->indent);
            buf_printf(body, "*((%s *)%s) = %s;\n", inner_type_c, tmp, inner);
            free(inner);
            free(inner_type_c);
            /* Mark that we need stdlib.h for malloc/free */
            /* We'll use a flag in the EmitCtx - but ctx is passed by pointer */
            /* For now, we'll just always include stdlib.h when we see a ref */
            /* This is a simplification - in production we'd track this properly */
            return tmp;
        }
        case EX_DEREF: {
            /* (@ expr) - dereference ref<T>, rc<T>, ptr<T>, &T, or &mut T */
            char *inner = emit_value(ctx, body, e->as.deref_.expr);
            
            /* For ref<T> / lref<T>, we need to cast and dereference */
            if (e->as.deref_.expr->type.kind == TY_REF
                || e->as.deref_.expr->type.kind == TY_LREF) {
                char *inner_type_c = strdup(type_c_name(e->type));
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = *((%s *)%s);\n", 
                           inner_type_c, tmp, inner_type_c, inner);
                free(inner);
                free(inner_type_c);
                return tmp;
            } else if (e->as.deref_.expr->type.kind == TY_REF_IMMUT
                       || e->as.deref_.expr->type.kind == TY_REF_MUT) {
                /* Phase 12: &T / &mut T dereference: *((T *)ptr) */
                const char *inner_type_c = type_c_name(e->type);
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = *((%s *)%s);\n",
                           inner_type_c, tmp, inner_type_c, inner);
                free(inner);
                return tmp;
            } else {
                /* For ptr<T>, just cast to the appropriate type and dereference */
                /* For now, ptr<void> stays as void* */
                return inner;
            }
        }
        /* Phase 3 */
        case EX_CLOSURE: {
            /* Emit closure: the closure value IS the env struct (passed by value to thunk) */
            struct Closure *closure = e->as.closure_.closure;
            const Symbol *env_name = closure->env_name;
            /* poly-closure-result-specialization: when emitting the body of an
             * outer (closure-returning) spec, the EX_CLOSURE it constructs must
             * store the register-class-correct inner-body clone's thunk and use
             * its suffixed env struct.  Resolve the thunk's result/param tyvars
             * to the spec's concrete (float) types via emit_resolve_type so the
             * typed thunk slot is xmm0-correct.  No-op outside a matching spec. */
            const char *thunk_sym_override = NULL;
            const EmitAbiSpecialization *_cur_spec = ctx->current_abi_specialization;
            if (_cur_spec && _cur_spec->inner_closure_spec_idx >= 0) {
                const EmitAbiSpecialization *_isp =
                    &ctx->abi_specializations[_cur_spec->inner_closure_spec_idx];
                if (_isp->binding == closure->fn->binding) {
                    if (_isp->env_name_override) env_name = _isp->env_name_override;
                    thunk_sym_override = _isp->clone_name;
                }
            }

            /* Emit env struct type definition at file scope if not already emitted */
            /* Check if we've already emitted this env struct */
            bool already_emitted = false;
            if (ctx->env_struct_names) {
                for (uint8_t i = 0; i < ctx->n_env_struct_names; i++) {
                    if (ctx->env_struct_names[i] == env_name) {
                        already_emitted = true;
                        break;
                    }
                }
            }
            if (!already_emitted) {
                Type thunk_result = emit_resolve_type(ctx, emit_fn_result_type_from_type(closure->fn->binding->type));
                uint8_t thunk_arity = closure->fn->n_params > 0 ? (uint8_t)(closure->fn->n_params - 1) : 0;
                Type _rtp[MAX_FN_ARITY];
                Type *thunk_params = closure->fn->n_params > 1 ? &closure->fn->param_types[1] : NULL;
                if (thunk_params && thunk_sym_override) {
                    for (uint8_t _t = 0; _t < thunk_arity; _t++)
                        _rtp[_t] = emit_resolve_type(ctx, closure->fn->param_types[_t + 1]);
                    thunk_params = _rtp;
                }
                char *thunk_typedef = ensure_typed_thunk_typedef(ctx, ctx->file, thunk_result, thunk_params, thunk_arity);
                /* Track this env struct */
                if (ctx->n_env_struct_names >= ctx->cap_env_struct_names) {
                    ctx->cap_env_struct_names = ctx->cap_env_struct_names ? ctx->cap_env_struct_names * 2 : 8;
                    ctx->env_struct_names = (const Symbol **)realloc(ctx->env_struct_names,
                        ctx->cap_env_struct_names * sizeof(const Symbol *));
                }
                ctx->env_struct_names[ctx->n_env_struct_names++] = env_name;
                
                /* Phase HKT §5: fat closure struct — __fn first, then captures. */
                if (thunk_typedef) {
                    buf_printf(ctx->file, "struct %s { %s __fn; ", env_name->name, thunk_typedef);
                } else {
                    buf_printf(ctx->file, "struct %s { int64_t __fn; ", env_name->name);
                }
                for (uint8_t i = 0; i < closure->n_captures; i++) {
                    Binding *captured = closure->captures[i];
                    char *field = raw_name_for_binding(captured);
                    /* A captured function value (fat closure or bare fn ptr) is
                     * carried as the int64_t fn-ABI carrier -- the same C type
                     * fn-typed parameters use (see emit_fns.c, TY_FN param
                     * branch).  type_c_name(TY_FN) returns the *result* type's C
                     * name (e.g. "double" for a :float-returning closure), which
                     * would store the closure pointer in a floating-point field
                     * and reinterpret it through a double on read -- a latent
                     * miscompile that only survives because valid pointers fit
                     * exactly in a double's 53-bit integer range.  Pin the field
                     * to the carrier so the stored bits and the fat-dispatch
                     * read-back agree. */
                    const char *field_ctype = captured->type.kind == TY_FN
                        ? "int64_t"
                        : emit_type_c_name(ctx, captured->type);
                    buf_printf(ctx->file, "%s %s; ", field_ctype, field);
                    free(field);
                }
                buf_puts(ctx->file, "};\n");
                free(thunk_typedef);
            }
            
            /* Phase HKT §5: heap-allocate the fat closure struct so that the
             * fat pointer can safely escape from the local stack frame and be
             * passed to HKT helpers as an opaque int64_t.  Layout:
             *   struct __env_N { int64_t __fn; <captures...> }
             * The __fn field holds the thunk pointer (as int64_t).  Callers
             * using the generic fat-closure protocol recover the thunk as:
             *   thunk = (int64_t(*)(void*,int64_t))(intptr_t)fat->__fn
             * and invoke it as thunk(fat_ptr, arg). */
            Type thunk_result = emit_resolve_type(ctx, emit_fn_result_type_from_type(closure->fn->binding->type));
            uint8_t thunk_arity = closure->fn->n_params > 0 ? (uint8_t)(closure->fn->n_params - 1) : 0;
            Type _rtp2[MAX_FN_ARITY];
            Type *thunk_params = closure->fn->n_params > 1 ? &closure->fn->param_types[1] : NULL;
            if (thunk_params && thunk_sym_override) {
                for (uint8_t _t = 0; _t < thunk_arity; _t++)
                    _rtp2[_t] = emit_resolve_type(ctx, closure->fn->param_types[_t + 1]);
                thunk_params = _rtp2;
            }
            char *thunk_typedef = ensure_typed_thunk_typedef(ctx, ctx->file, thunk_result, thunk_params, thunk_arity);
            char *thunk_sym = thunk_sym_override
                ? strdup(thunk_sym_override)
                : raw_name_for_binding(closure->fn->binding);
            char *fat_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "struct %s *%s = (struct %s *)malloc(sizeof(struct %s));\n",
                       env_name->name, fat_tmp, env_name->name, env_name->name);
            indent_buf(body, ctx->indent);
            if (thunk_typedef) {
                buf_printf(body, "%s->__fn = (%s)%s;\n", fat_tmp, thunk_typedef, thunk_sym);
            } else {
                buf_printf(body, "%s->__fn = (int64_t)(intptr_t)%s;\n", fat_tmp, thunk_sym);
            }
            for (uint8_t i = 0; i < closure->n_captures; i++) {
                Binding *captured = closure->captures[i];
                char *cn = name_for_binding(ctx, captured);
                char *field = raw_name_for_binding(captured);
                indent_buf(body, ctx->indent);
                /* B5: a captured struct/ADT that is a pass-by-pointer parameter
                 * of the *enclosing* function arrives as `const T *`, but the
                 * env field is declared by value (type_c_name => `T`).  Deref so
                 * the closure stores its own copy of the value rather than the
                 * caller's pointer (which would dangle once the frame returns).
                 * See docs/upcoming/stdlib-type-erasure-cleanup-plan.md (B5). */
                bool captured_is_pbp = false;
                for (uint8_t _p = 0; _p < ctx->n_pbp_params; _p++) {
                    if (ctx->pbp_param_ptrs[_p] == captured) {
                        captured_is_pbp = true;
                        break;
                    }
                }
                buf_printf(body, "%s->%s = %s%s;\n",
                           fat_tmp, field, captured_is_pbp ? "*" : "", cn);
                free(field);
                free(cn);
            }
            char *ptr_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = %s;\n", ptr_tmp, fat_tmp);
            free(thunk_typedef);
            free(thunk_sym);
            free(fat_tmp);

            return ptr_tmp;
        }
        /* Phase 9: rc<T> + weak<T> operations */
        case EX_RC_OF: {
            /* (rc/of x) - allocate control block, copy value into it */
            char *inner = emit_value(ctx, body, e->as.rc_of_.expr);
            char *inner_type_c = strdup(type_c_name(e->as.rc_of_.expr->type));
            
            /* Emit: allocate value separately, then attach it to rc control block. */
            char *val_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                       inner_type_c, val_tmp, inner_type_c, inner_type_c);
            indent_buf(body, ctx->indent);
            buf_printf(body, "*%s = %s;\n", val_tmp, inner);

            char *cb_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Phase 11 / DS3: if the inner value is a struct with rc fields,
             * pass its drop glue and -- so the cycle walker can trace
             * through -- its walk glue via rc_cb_alloc_struct. */
            const char *drop_fn_name = "NULL";
            char dg_name_buf[256];
            char wg_name_buf[256];
            bool struct_with_rc_fields = false;
            if (e->as.rc_of_.expr->type.kind == TY_STRUCT) {
                StructDef *sdef = e->as.rc_of_.expr->type.as.struct_.def;
                if (sdef && sdef->needs_drop_glue) {
                    snprintf(dg_name_buf, sizeof(dg_name_buf), "drop_glue_%s", sdef->name);
                    snprintf(wg_name_buf, sizeof(wg_name_buf), "walk_glue_%s", sdef->name);
                    drop_fn_name = dg_name_buf;
                    struct_with_rc_fields = true;
                }
            }
            if (struct_with_rc_fields) {
                buf_printf(body, "RcControlBlock *%s = rc_cb_alloc_struct(0, %d, %s, %s);\n",
                           cb_tmp, e->as.rc_of_.expr->type.kind,
                           drop_fn_name, wg_name_buf);
            } else {
                buf_printf(body, "RcControlBlock *%s = rc_cb_alloc(0, %d, %s);\n",
                           cb_tmp, e->as.rc_of_.expr->type.kind, drop_fn_name);
            }
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->value = %s;\n", cb_tmp, val_tmp);

            free(inner);
            free(inner_type_c);
            free(val_tmp);
            return cb_tmp;
        }
        case EX_RC_CLONE: {
            /* (rc/clone r) - increment strong count, return same cb */
            char *inner = emit_value(ctx, body, e->as.rc_clone_.expr);
            if (e->as.rc_clone_.elide) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "/* rc-elision: skipped rc_strong_increment(%s) \xe2\x80\x94 last-use clone */\n", inner);
            } else {
                indent_buf(body, ctx->indent);
                buf_printf(body, "rc_strong_increment(%s);\n", inner);
            }
            /* Return the same pointer (now with incremented count, or elided).
             * Duplicate the string because the caller takes ownership of the
             * returned buffer and `inner` is freed locally here. */
            char *result = strdup(inner);
            free(inner);
            return result;
        }
        case EX_RC_DROP: {
            /* (rc/drop r) - decrement strong count */
            char *inner = emit_value(ctx, body, e->as.rc_drop_.expr);
            if (e->as.rc_drop_.elide) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "/* rc-elision: skipped rc_strong_decrement(%s) \xe2\x80\x94 matched elided clone */\n", inner);
            } else {
                indent_buf(body, ctx->indent);
                buf_printf(body, "rc_strong_decrement(%s);\n", inner);
                indent_buf(body, ctx->indent);
                buf_printf(body, "rc_free_queue_drain();\n");
            }
            free(inner);
            return atom_nil();
        }
        case EX_RC_PTR: {
            /* (rc->ptr r) - borrow ptr<T> from rc<T> */
            char *inner = emit_value(ctx, body, e->as.rc_ptr_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = rc_get_value(%s);\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_RC_COUNT: {
            /* (rc/strong-count r) - get strong count */
            char *inner = emit_value(ctx, body, e->as.rc_count_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Bridge int64_t carrier (e.g. existential/forall field) to RcControlBlock*. */
            TypeKind inner_kind = e->as.rc_count_.expr->type.kind;
            bool inner_needs_bridge = (inner_kind == TY_INT || inner_kind == TY_EXISTS
                                       || inner_kind == TY_FORALL);
            if (inner_needs_bridge) {
                buf_printf(body, "int64_t %s = rc_strong_count((RcControlBlock *)(intptr_t)(%s));\n",
                           tmp, inner);
            } else {
                buf_printf(body, "int64_t %s = rc_strong_count(%s);\n", tmp, inner);
            }
            free(inner);
            return tmp;
        }
        case EX_RC_FROM_REF: {
            /* (rc/from-ref r) - move ref<T> into rc<T> */
            char *inner = emit_value(ctx, body, e->as.rc_from_ref_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "RcControlBlock *%s = tur_rc_from_ref(%s, %d);\n",
                       tmp, inner, e->type.as.rc.inner);
            free(inner);
            return tmp;
        }
        case EX_REF_FROM_RC: {
            /* (ref/from-rc r) - extract unique ref<T> from rc<T> */
            char *inner = emit_value(ctx, body, e->as.ref_from_rc_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = tur_ref_from_rc(%s);\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_WEAK: {
            /* (weak r) - create weak<T> from rc<T> */
            char *inner = emit_value(ctx, body, e->as.weak_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* weak shares the same control block, increments weak count */
            buf_printf(body, "RcControlBlock *%s = %s; rc_weak_increment(%s);\n", tmp, inner, inner);
            free(inner);
            return tmp;
        }
        case EX_WEAK_UPGRADE: {
            /* (upgrade w) -> option<rc<T>> as ptr<void>: some(rc) or none (NULL) */
            char *inner = emit_value(ctx, body, e->as.weak_upgrade_.expr);
            char *cb_tmp  = fresh_tmp(ctx);
            char *opt_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "RcControlBlock *%s = rc_upgrade(%s);\n", cb_tmp, inner);
            free(inner);
            indent_buf(body, ctx->indent);
            buf_printf(body, "struct { bool is_some; int64_t value; } *%s = NULL;\n", opt_tmp);
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s) { %s = malloc(sizeof(*%s)); %s->is_some = true; "
                             "%s->value = (int64_t)(intptr_t)%s; }\n",
                       cb_tmp, opt_tmp, opt_tmp, opt_tmp, opt_tmp, cb_tmp);
            free(cb_tmp);
            return opt_tmp;
        }
        case EX_WEAK_PRED: {
            /* (weak? w) - check if w is weak<T> */
            char *inner = emit_value(ctx, body, e->as.weak_pred_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Check if the type kind is TY_WEAK - but at runtime we can't easily check this */
            /* For Phase 9, we'll use a simple approach: check if it's a RcControlBlock pointer */
            /* This is a simplification - in a proper implementation we'd need type info */
            buf_printf(body, "bool %s = true; // weak? not fully implemented in Phase 9\n", tmp);
            free(inner);
            return tmp;
        }
        case EX_REF_PRED: {
            /* (ref? x) - check if x is ref<T>: always true if elaboration accepted it */
            char *inner = emit_value(ctx, body, e->as.ref_pred_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* ref<T> is a void* in C; check non-NULL */
            buf_printf(body, "bool %s = (%s != NULL); /* ref? — non-null pointer check */\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_CONT_PRED:        return emit_effects_cont_pred(ctx, body, e);
        /* Phase T21-F: async/await */
        case EX_ASYNC: {
            /* (async fn-expr) — launch fn-expr in a fiber, return TurFuture* as ptr<void>.
             *
             * Two paths:
             *  (a) fn-expr has type TY_FN (a no-arg function): use it as a function pointer
             *      directly (existing behaviour).
             *  (b) fn-expr is an expression (e.g., (handle ...) from (with-handler ...)):
             *      T25 — wrap in a static thunk emitted at file scope, then call tur_async_fiber
             *      with the thunk.  v1 limitation: the expression must not capture outer-scope
             *      variables (same limitation as effect handler bodies). */
            const Expr *fn_expr = e->as.async_.fn_expr;
            char *tmp = fresh_tmp(ctx);
            if (fn_expr->type.kind == TY_FN) {
                /* Path (a): fn-expr is already a function pointer */
                char *fn_val = emit_value(ctx, body, fn_expr);
                indent_buf(body, ctx->indent);
                buf_printf(body, "void *%s = (void *)tur_async_fiber((int64_t(*)(void))(intptr_t)%s);\n", tmp, fn_val);
                free(fn_val);
            } else {
                /* Path (b): wrap the expression in a static thunk (T25 with-handler support).
                 *
                 * Emission order is important: any nested handler functions (emitted to
                 * pending_handler_fns) must appear BEFORE the thunk's opening brace in the
                 * output.  We achieve this by:
                 *   1. Emit the forward declaration to pbuf.
                 *   2. Emit the thunk body into a separate temporary buffer (thunk_body_buf),
                 *      routing pending_handler_fns to pbuf so nested handler fns land there.
                 *   3. Write the thunk definition (brace + body buf + return + close) to pbuf.
                 */
                char *thunk_name = (char *)malloc(32);
                snprintf(thunk_name, 32, "__async_thunk_%d", ctx->tmp_n++);

                Buf *pbuf = ctx->pending_handler_fns;

                /* Step 1: forward declaration */
                buf_printf(pbuf, "static int64_t %s(void);\n", thunk_name);

                /* Step 2: emit thunk body into a separate buffer */
                Buf thunk_body_buf;
                buf_init(&thunk_body_buf);

                EmitCtx tctx = *ctx;
                tctx.file = pbuf;
                tctx.pending_handler_fns = pbuf; /* nested handler fns → pbuf (file scope) */
                tctx.indent = 4;
                tctx.fn_params = NULL;
                tctx.n_fn_params = 0;
                tctx.closure = NULL;
                tctx.env_var_name = NULL;
                tctx.defer_captures = NULL;
                tctx.n_defer_captures = 0;
                tctx.frame_var = NULL;
                tctx.return_emitted = false;

                char *ret = NULL;
                if (fn_expr->type.kind == TY_NIL || fn_expr->type.kind == TY_NEVER) {
                    emit_stmt(&tctx, &thunk_body_buf, fn_expr);
                } else {
                    ret = emit_value(&tctx, &thunk_body_buf, fn_expr);
                }
                ctx->tmp_n = tctx.tmp_n;

                /* Step 3: write thunk definition to pbuf — after nested handler fns */
                buf_printf(pbuf, "static int64_t %s(void) {\n", thunk_name);
                if (thunk_body_buf.len > 0)
                    buf_write(pbuf, thunk_body_buf.data, thunk_body_buf.len);
                if (ret) {
                    buf_printf(pbuf, "    return (int64_t)%s;\n", ret);
                    free(ret);
                } else {
                    buf_puts(pbuf, "    return 0;\n");
                }
                buf_puts(pbuf, "}\n\n");
                buf_free(&thunk_body_buf);

                indent_buf(body, ctx->indent);
                buf_printf(body, "void *%s = (void *)tur_async_fiber(%s);\n", tmp, thunk_name);
                free(thunk_name);
            }
            return tmp;
        }
        case EX_AWAIT: {
            /* (await fut) — await a future using shift + scheduler callback */
            char *fut_val = emit_value(ctx, body, e->as.await_.fut_expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_await_future((TurFuture*)(intptr_t)%s);\n", tmp, fut_val);
            free(fut_val);
            return tmp;
        }
        /* Phase SEL1: fair multi-channel select */
        case EX_SELECT: {
            /*
             * (select ((ch :recv v) body) ... (:default body))
             *
             * Generates:
             *   TurSelectClause __sel_N[n];
             *   __sel_N[0].chan = (void*)(intptr_t)<ch_val>;
             *   __sel_N[0].op  = 0;  // recv
             *   __sel_N[0].val = 0;
             *   ...
             *   int64_t __sel_idx_N = (int64_t)tur_select_blocking(__sel_N, n, has_default);
             *   int64_t __sel_result_N;
             *   if (__sel_idx_N == 0) { int64_t v = __sel_N[0].val; __sel_result_N = <body0>; }
             *   else if (__sel_idx_N == 1) { ... }
             *   else { __sel_result_N = <default_body>; }
             */
            uint32_t n    = e->as.select_.n_clauses;
            int has_def   = e->as.select_.has_default;
            const SelectClauseEntry *cls = e->as.select_.clauses;

            /* Allocate unique suffix for this select to avoid name collisions */
            int sel_id = ctx->tmp_n++;

            char sel_arr[32], sel_idx[32], sel_res[32];
            snprintf(sel_arr, sizeof(sel_arr), "__sel_%d",     sel_id);
            snprintf(sel_idx, sizeof(sel_idx), "__sel_idx_%d", sel_id);
            snprintf(sel_res, sizeof(sel_res), "__sel_res_%d", sel_id);

            /* Declare clause array */
            indent_buf(body, ctx->indent);
            buf_printf(body, "TurSelectClause %s[%u];\n", sel_arr, n > 0 ? n : 1);

            /* Fill clause array */
            for (uint32_t ci = 0; ci < n; ci++) {
                const SelectClauseEntry *cl = &cls[ci];

                /* Emit channel expression */
                char *ch_val = emit_value(ctx, body, cl->chan);

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s[%u].chan = (void *)(intptr_t)%s;\n",
                           sel_arr, ci, ch_val);
                free(ch_val);

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s[%u].op = %d;\n", sel_arr, ci, cl->op);

                if (cl->op == 1 && cl->send_val) {
                    /* send: fill val field before call */
                    char *sv = emit_value(ctx, body, cl->send_val);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s[%u].val = (int64_t)%s;\n", sel_arr, ci, sv);
                    free(sv);
                } else {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s[%u].val = 0;\n", sel_arr, ci);
                }
            }

            /* Call tur_select_blocking */
            indent_buf(body, ctx->indent);
            buf_printf(body,
                "int64_t %s = (int64_t)tur_select_blocking(%s, %d, %d);\n",
                sel_idx, sel_arr, (int)n, has_def);

            /* Declare result variable (zero-initialized to silence -Wuninitialized) */
            const char *res_ctype = type_c_name(e->type);
            bool has_result = (e->type.kind != TY_NIL && e->type.kind != TY_NEVER);
            if (has_result) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = 0;\n", res_ctype, sel_res);
            }

            /* Dispatch: if-else chain over clause index */
            for (uint32_t ci = 0; ci < n; ci++) {
                const SelectClauseEntry *cl = &cls[ci];

                indent_buf(body, ctx->indent);
                if (ci == 0) {
                    buf_printf(body, "if (%s == %d) {\n", sel_idx, (int)ci);
                } else {
                    buf_printf(body, "} else if (%s == %d) {\n", sel_idx, (int)ci);
                }
                ctx->indent += 4;

                if (cl->op == 0 && cl->recv_binding) {
                    /* recv: declare binding with the received value */
                    char *bname = name_for_binding(ctx, cl->recv_binding);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "int64_t %s = %s[%u].val;\n",
                               bname, sel_arr, ci);
                    free(bname);
                }

                if (has_result) {
                    char *bv = emit_value(ctx, body, cl->body);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s = (%s)%s;\n", sel_res, res_ctype, bv);
                    free(bv);
                } else {
                    emit_stmt(ctx, body, cl->body);
                }

                ctx->indent -= 4;
            }

            /* Default branch */
            if (has_def) {
                indent_buf(body, ctx->indent);
                if (n == 0) {
                    buf_printf(body, "if (1) {\n");
                } else {
                    buf_puts(body, "} else {\n");
                }
                ctx->indent += 4;
                if (has_result) {
                    char *dv = emit_value(ctx, body, e->as.select_.default_body);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s = (%s)%s;\n", sel_res, res_ctype, dv);
                    free(dv);
                } else {
                    emit_stmt(ctx, body, e->as.select_.default_body);
                }
                ctx->indent -= 4;
            }

            /* Close final brace */
            if (n > 0 || has_def) {
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
            }

            if (has_result) {
                char *tmp = strdup(sel_res);
                return tmp;
            } else {
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = 0;\n", tmp);
                return tmp;
            }
        }
        /* Phase 20: Software Transactional Memory */
        case EX_STM: {
            /* (stm expr1 expr2 ...) -- a transactional block.  When directly
             * under `atomically`, the EX_ATOMICALLY case inlines stm_.body and
             * this arm is not reached.  It IS reached for nested stm blocks --
             * notably the two branches of `or-else` -- which previously emitted
             * a no-op stub (docs/reported/
             * stm-or-else-compiled-branches-are-noop-stubs.md).  Emit the body
             * for real: statements in order, the last as the block's value.
             * After a `check`/`retry` requests a retry, short-circuit the rest
             * of the block (mirroring the interpreter's EX_STM), so a
             * speculative write that follows an aborted guard is not buffered. */
            uint32_t n = e->as.stm_.n_body;
            Type rt = e->type;
            bool has_val = (rt.kind != TY_NIL && rt.kind != TY_NEVER);
            char *res = NULL;
            if (has_val) {
                res = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = 0;\n", type_c_name(rt), res);
            }
            int opened = 0;
            for (uint32_t i = 0; i < n; i++) {
                const Expr *be = e->as.stm_.body[i];
                if (i > 0) {
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "if (!__tur_stm_should_retry(tur_stm_current_tx())) {\n");
                    ctx->indent += 4;
                    opened++;
                }
                if (i == n - 1 && has_val) {
                    char *v = emit_value(ctx, body, be);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s = %s;\n", res, v);
                    free(v);
                } else {
                    emit_stmt(ctx, body, be);
                }
            }
            while (opened-- > 0) {
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
            }
            return has_val ? res : atom_nil();
        }
        case EX_ATOMICALLY: {
            /* (atomically stm-expr) - execute the stm block atomically */
            /* Emit the transaction loop inline, with the STM expressions inside */
            char *result = fresh_tmp(ctx);
            Type result_type = e->type;
            
            /* Declare result variable - but void type is not allowed in C */
            if (result_type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(result_type), result);
            }
            
            /* atomically block */
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* atomically */\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;
            
            /* Create transaction */
            indent_buf(body, ctx->indent);
            buf_puts(body, "STM_Transaction *__tx = tur_stm_new_transaction();\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "STM_Transaction *__prev_tx = tur_stm_current_tx();\n");
            
            /* Transaction loop */
            indent_buf(body, ctx->indent);
            buf_puts(body, "while (1) {\n");
            ctx->indent += 4;
            
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->retry_requested = false;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->aborted = false;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->read_count = 0;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->write_count = 0;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__tx);\n");
            
            /* Emit STM block body directly */
            const Expr *stm_expr = e->as.atomically_.stm_expr;
            char *stm_last_val = NULL;
            if (stm_expr->as.stm_.n_body > 0) {
                for (uint32_t i = 0; i < stm_expr->as.stm_.n_body; i++) {
                    const Expr *expr = stm_expr->as.stm_.body[i];
                    if (i < stm_expr->as.stm_.n_body - 1) {
                        /* Not the last expression - emit as statement */
                        emit_stmt(ctx, body, expr);
                    } else {
                        /* Last expression - emit as value */
                        if (expr->type.kind != TY_NIL && expr->type.kind != TY_NEVER) {
                            stm_last_val = emit_value(ctx, body, expr);
                        } else {
                            emit_stmt(ctx, body, expr);
                        }
                    }
                }
            }
            
            /* Check if retry is needed */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (__tur_stm_should_retry(__tx)) {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "continue;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            /* Commit */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (tur_stm_commit(__tx)) {\n");
            ctx->indent += 4;
            if (result_type.kind != TY_NIL && stm_last_val) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, stm_last_val);
                free(stm_last_val);
            } else {
                indent_buf(body, ctx->indent);
                buf_puts(body, "/* STM block returned nil or void */\n");
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "free(__tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "break;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            /* Retry */
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            return result;
        }
        case EX_RETRY: {
            /* (retry) - request transaction retry */
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_retry(tur_stm_current_tx());\n");
            return atom_nil();
        }
        case EX_CHECK: {
            /* (check cond) - abort if condition is false */
            char *cond_val = emit_value(ctx, body, e->as.check_.cond);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_stm_check(%s);\n", cond_val);
            free(cond_val);
            return atom_nil();
        }
        case EX_OR_ELSE: {
            /* (or-else stm1 stm2) - try stm1, retry with stm2 on retry */
            /* Simplified: emit stm1, if retry then emit stm2 */
            /* Note: This is a simplified implementation that doesn't properly handle
             * the retry semantics. A full implementation would need to track
             * which branch was taken. */
            char *result = fresh_tmp(ctx);
            Type result_type = e->type;
            
            if (result_type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(result_type), result);
            }
            
            indent_buf(body, ctx->indent);
            buf_puts(body, "/* or-else */\n");
            
            /* Save retry state before stm1 */
            indent_buf(body, ctx->indent);
            buf_puts(body, "bool __or_else_retry_before = tur_stm_current_tx()->retry_requested;\n");
            
            /* Emit stm1 */
            if (e->as.or_else_.stm1->type.kind != TY_NIL) {
                char *stm1_val = emit_value(ctx, body, e->as.or_else_.stm1);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, stm1_val);
                free(stm1_val);
            } else {
                emit_stmt(ctx, body, e->as.or_else_.stm1);
            }
            
            /* If stm1 caused retry, clear it and emit stm2 */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (!__or_else_retry_before && tur_stm_current_tx()->retry_requested) {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_current_tx()->retry_requested = false;\n");
            if (e->as.or_else_.stm2->type.kind != TY_NIL) {
                char *stm2_val = emit_value(ctx, body, e->as.or_else_.stm2);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, stm2_val);
                free(stm2_val);
            } else {
                emit_stmt(ctx, body, e->as.or_else_.stm2);
            }
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            return result;
        }
        case EX_TVAR_NEW: {
            /* (TVar::new init) - create a new TVar */
            char *init_val = emit_value(ctx, body, e->as.tvar_new_.init);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* For now, we emit a malloc-based TVar - proper impl needs TypeInfo */
            buf_printf(body, "TVar *%s = tur_tvar_new(NULL, (void*)(intptr_t)%s);\n", tmp, init_val);
            free(init_val);
            return tmp;
        }
        case EX_TVAR_READ: {
            /* (TVar::read tvar) - read a TVar within a transaction */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_read_.tvar);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = tur_tvar_read(tur_stm_current_tx(), (TVar*)%s);\n", tmp, tvar_val);
            free(tvar_val);
            return tmp;
        }
        case EX_TVAR_WRITE: {
            /* (TVar::write tvar value) - write to a TVar within a transaction */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_write_.tvar);
            char *val_val = emit_value(ctx, body, e->as.tvar_write_.value);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_tvar_write(tur_stm_current_tx(), (TVar*)%s, (void*)(intptr_t)%s);\n", tvar_val, val_val);
            free(tvar_val);
            free(val_val);
            return atom_nil();
        }
        case EX_TVAR_MODIFY: {
            /* Dead arm: elab_tvar_modify lowers (tvar/modify tv f) to
             * (let [g tv] (tvar/swap g (f (tvar/read g)))) so the function
             * application goes through the normal call-dispatch path.  This is
             * kept defensive in case a raw EX_TVAR_MODIFY ever reaches codegen. */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_modify_.tvar);
            char *fn_val = emit_value(ctx, body, e->as.tvar_modify_.fn);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* TVar::modify %s %s (lowered in elab) */ void *%s = NULL;\n", tvar_val, fn_val, tmp);
            free(tvar_val);
            free(fn_val);
            return tmp;
        }
        case EX_TVAR_SWAP: {
            /* (TVar::swap tvar new) - swap value and return old */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_swap_.tvar);
            char *new_val = emit_value(ctx, body, e->as.tvar_swap_.new_val);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = tur_tvar_swap(tur_stm_current_tx(), (TVar*)%s, (void*)(intptr_t)%s);\n", tmp, tvar_val, new_val);
            free(tvar_val);
            free(new_val);
            return tmp;
        }
        case EX_TVAR_CAS: {
            /* (TVar::cas tvar old new) - compare-and-swap */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_cas_.tvar);
            char *old_val = emit_value(ctx, body, e->as.tvar_cas_.old_val);
            char *new_val = emit_value(ctx, body, e->as.tvar_cas_.new_val);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "bool %s = tur_tvar_cas(tur_stm_current_tx(), (TVar*)%s, (void*)(intptr_t)%s, (void*)(intptr_t)%s);\n", tmp, tvar_val, old_val, new_val);
            free(tvar_val);
            free(old_val);
            free(new_val);
            return tmp;
        }
        /* Phase 12: Borrow traits */
        case EX_BORROW_IMMUT: {
            /* (& expr) - immutable borrow */
            Type inner_type = e->as.borrow_immut_.expr->type;
            char *inner = emit_value(ctx, body, e->as.borrow_immut_.expr);
            if (inner_type.kind == TY_REF_IMMUT || inner_type.kind == TY_REF_MUT) {
                /* Reborrow: the pointer value IS the borrow — return as-is */
                return inner;
            } else if (inner_type.kind == TY_REF) {
                /* Borrow from ref<T>: ref<T> is a void* pointing to T */
                char *result = (char *)malloc(strlen(inner) + 24);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 24, "((const void *)%s)", inner);
                free(inner);
                return result;
            } else {
                /* Plain value borrow: take address */
                char *result = (char *)malloc(strlen(inner) + 10);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 10, "&%s", inner);
                free(inner);
                return result;
            }
        }
        case EX_BORROW_MUT: {
            /* (&mut expr) - mutable borrow */
            Type inner_type = e->as.borrow_mut_.expr->type;
            char *inner = emit_value(ctx, body, e->as.borrow_mut_.expr);
            if (inner_type.kind == TY_REF_MUT) {
                /* Mutable reborrow: return pointer directly */
                return inner;
            } else if (inner_type.kind == TY_REF) {
                /* Borrow from ref<T>: ref<T> is a void* pointing to T */
                char *result = (char *)malloc(strlen(inner) + 20);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 20, "((void *)%s)", inner);
                free(inner);
                return result;
            } else {
                /* Plain value mutable borrow: take address */
                char *result = (char *)malloc(strlen(inner) + 10);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 10, "&%s", inner);
                free(inner);
                return result;
            }
        }
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:           return emit_effects_defect(ctx, body, e);
        /* DV2: Dynamic vars */
        case EX_DEFDYNAMIC:
            /* Declaration is compile-time only; root init is in emit_module.c Pass 2. */
            return atom_nil();
        case EX_DYNVAR_READ: {
            DynVarEntry *dv_entry = e->as.dynvar_read_.entry;
            char *mname = mangle_dynvar_name(dv_entry->name->name);
            const char *vctype = type_c_name(dv_entry->value_type);
            char *ftmp = fresh_tmp(ctx);
            char *rtmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "TurDynFrame *%s = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n",
                       ftmp, mname);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = %s ? *(%s *)%s->value : _dynvar_root_%s;\n",
                       vctype, rtmp, ftmp, vctype, ftmp, mname);
            free(mname);
            free(ftmp);
            return rtmp;
        }
        case EX_DYNVAR_BINDING: {
            const DynBinding *pairs = e->as.dynvar_binding_.pairs;
            uint32_t n_pairs = e->as.dynvar_binding_.n_pairs;
            bool nil_result = (e->type.kind == TY_NIL);
            char *result_tmp = NULL;
            if (!nil_result) {
                result_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(e->type), result_tmp);
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;
            for (uint32_t pi = 0; pi < n_pairs; pi++) {
                DynVarEntry *dv_entry = pairs[pi].entry;
                char *mname = mangle_dynvar_name(dv_entry->name->name);
                const char *vctype = type_c_name(dv_entry->value_type);
                /* Emit override value */
                char *val = emit_value(ctx, body, pairs[pi].override_expr);
                char *vslot = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = %s;\n", vctype, vslot, val);
                free(val);
                /* Emit binding frame */
                char *frame = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "TurDynFrame %s = { (TurDynFrame *)pthread_getspecific(_dynvar_key_%s), &%s };\n",
                    frame, mname, vslot);
                indent_buf(body, ctx->indent);
                buf_printf(body, "pthread_setspecific(_dynvar_key_%s, &%s);\n", mname, frame);
                /* Cleanup guard pointer -- cleanup fn pops the frame even on longjmp */
                char *fptr = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "TurDynFrame *%s __attribute__((cleanup(_dynvar_pop_%s))) = &%s;\n",
                    fptr, mname, frame);
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)%s;\n", fptr);
                free(mname);
                free(vslot);
                free(frame);
                free(fptr);
            }
            /* Emit body */
            char *bval = emit_value(ctx, body, e->as.dynvar_binding_.body);
            if (!nil_result) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result_tmp, bval);
            }
            free(bval);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            return nil_result ? atom_nil() : result_tmp;
        }
        case EX_DYNVAR_SET: {
            DynVarEntry *dv_entry = e->as.dynvar_set_.entry;
            char *mname = mangle_dynvar_name(dv_entry->name->name);
            const char *vctype = type_c_name(dv_entry->value_type);
            char *val = emit_value(ctx, body, e->as.dynvar_set_.value);
            char *ftmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "{ TurDynFrame *%s = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n",
                       ftmp, mname);
            ctx->indent += 2;
            indent_buf(body, ctx->indent);
            buf_printf(body,
                "if (!%s) { fprintf(stderr, \"tur: set! on dynamic var '%s' with no active binding\\n\"); abort(); }\n",
                ftmp, dv_entry->name->name);
            indent_buf(body, ctx->indent);
            buf_printf(body, "*(%s *)%s->value = %s;\n", vctype, ftmp, val);
            ctx->indent -= 2;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            free(mname);
            free(val);
            free(ftmp);
            return atom_nil();
        }
        case EX_PERFORM:          return emit_effects_perform(ctx, body, e);
        case EX_HANDLE:          return emit_effects_handle(ctx, body, e);
        case EX_HANDLER_LIT:     return emit_effects_handler_lit(ctx, body, e);
        case EX_WITH_HANDLER:    return emit_effects_with_handler(ctx, body, e);
        case EX_COMPOSE_HANDLERS: return emit_effects_compose_handlers(ctx, body, e);
        case EX_RESUME:          return emit_effects_resume(ctx, body, e);
        case EX_DISCONTINUE:     return emit_effects_discontinue(ctx, body, e);
        case EX_MAKE_STRUCT: {
            /* (make-struct StructName v1 v2 ...) - emit C99 compound literal */
            StructDef *def = e->as.make_struct_.def;
            /* SC7: a transparent int newtype IS its single int64 field -- emit
             * the field value directly (cast to int64), no compound literal. */
            if (type_is_transparent_int_newtype(e->type)) {
                char *fv = emit_value(ctx, body, e->as.make_struct_.field_values[0]);
                Buf id; buf_init(&id);
                buf_printf(&id, "(int64_t)(%s)", fv);
                buf_putc(&id, '\0');
                free(fv);
                char *result = strdup(id.data);
                buf_free(&id);
                return result;
            }
            Buf lit; buf_init(&lit);
            /* end-to-end-monomorphization: for a :heap-tagged type, make-struct
             * builds the by-value header literal (`(Vec__int){...}`) and then
             * heap-boxes it, returning the typed pointer `Vec__int *` (== the
             * type of `(Vec A)`).  So the compound literal must use the by-value
             * name, not the pointer name type_c_name now returns for a heap
             * type.  Non-heap types are unchanged. */
            Type ms_resolved = emit_resolve_type(ctx, e->type);
            bool ms_heap = type_is_heap_struct(ms_resolved);
            const char *struct_c_name = ms_heap
                ? type_struct_value_c_name(ms_resolved)
                : emit_type_c_name(ctx, e->type);
            /* M2b: when the make-struct's e->type carries unresolved tyvars
             * (e.g. body of `(defn ok [A B] [x : A] : (Result A B)
             *   (make-struct Result :err-val (default-of B) ...))` — B isn't a
             * param, so the current ABI spec's `bindings[]` doesn't bind it),
             * struct_c_name collapses to "int64_t" because type_c_name(TY_APP)
             * falls back when type_has_concrete_codegen_layout is false.  The
             * StructDef plus its FIELDS were elaborated correctly, so the body
             * already emits `.is_ok = ...` per-field assignments — only the
             * outer cast is wrong.  When the enclosing spec's result_type has
             * the same StructDef, use its concrete C name instead.  Narrowly
             * scoped: never fires outside specialization, and never fires when
             * the resolver already produced a non-carrier name.
             *
             * The same recovered spine args are also used to type per-field
             * `(default-of T)` values whose T resolves to a bare unresolved
             * TYVAR -- without this, the compound literal lands as
             * `.cstr_field = (int64_t){0}`, which is an int-to-ptr conversion
             * error in C. */
            Type rt_recovered_args[16]; uint8_t n_rt_recovered = 0;
            bool have_rt_recovered = false;
            if (def && ctx->current_abi_specialization) {
                Type rt = ctx->current_abi_specialization->result_type;
                StructDef *rt_def = NULL;
                if (type_extract_struct_app(&rt, &rt_def, rt_recovered_args,
                                            &n_rt_recovered)
                    && rt_def == def) {
                    have_rt_recovered = true;
                    if (strcmp(struct_c_name, "int64_t") == 0) {
                        /* For a heap type, recover the by-value header name (not
                         * the pointer name) so the compound literal is valid. */
                        const char *recovered = ms_heap
                            ? type_struct_value_c_name(emit_resolve_type(ctx, rt))
                            : emit_type_c_name(ctx, rt);
                        if (recovered && strcmp(recovered, "int64_t") != 0) {
                            struct_c_name = recovered;
                        }
                    }
                }
            }
            /* end-to-end-monomorphization: a :heap make-struct whose element type
             * is still abstract (the constructor's carrier base, e.g. `vec-new`'s
             * `[A]` base where `(Vec A)` is the int64 carrier) has struct_c_name
             * collapse to "int64_t".  The header layout is element-agnostic, so
             * box through the unspecialized header typedef (`def->name`, e.g.
             * `Vec`) and return the pointer as an int64 carrier.  Without this the
             * literal would be the invalid `(int64_t){.data = ...}`. */
            bool ms_heap_abstract_base = ms_heap && def
                && strcmp(struct_c_name, "int64_t") == 0;
            if (ms_heap_abstract_base)
                struct_c_name = def->name;
            buf_printf(&lit, "(%s){", struct_c_name);
            bool any_field_emitted = false;
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
                const Expr *fve = e->as.make_struct_.field_values[i];
                /* M2b dead-slot elision: when the field's value is
                 * `(default-of T)` (zero-valued, no side effects), drop the
                 * entire `.field = (T){0}` slot — C99 compound literals
                 * zero-initialize any field not named in the designator list,
                 * so the elided field still ends up zeroed.  This is the
                 * "Risk" section's elision rule from the M2b design doc: the
                 * default-of value for a sum-type's dead slot becomes a no-op
                 * at the emit level.  Per-field; skips bool/discriminator
                 * fields' default-of values too (always benign — they zero to
                 * `false`, the natural default). */
                if (fve && fve->kind == EX_DEFAULT_OF) continue;
                char *fv = NULL;
                if (fve->kind == EX_DEFAULT_OF && have_rt_recovered
                    && def->fields[i].full_type
                    && def->fields[i].full_type->kind == TY_TYVAR
                    && def->fields[i].full_type->as.tyvar_.name) {
                    const char *fty_name = def->fields[i].full_type->as.tyvar_.name;
                    uint8_t tp_idx = UINT8_MAX;
                    for (uint8_t tp = 0; tp < def->n_type_params; tp++) {
                        if (def->type_params[tp]
                            && strcmp(def->type_params[tp], fty_name) == 0) {
                            tp_idx = tp; break;
                        }
                    }
                    if (tp_idx != UINT8_MAX && tp_idx < n_rt_recovered) {
                        const char *fty = emit_type_c_name(ctx,
                            rt_recovered_args[tp_idx]);
                        if (fty) {
                            Buf fb; buf_init(&fb);
                            buf_printf(&fb, "(%s){0}", fty);
                            buf_putc(&fb, '\0');
                            fv = strdup(fb.data);
                            buf_free(&fb);
                        }
                    }
                }
                if (!fv) fv = emit_value(ctx, body, fve);
                bool is_fn_field = (def->fields[i].kind == TY_FN);
                bool val_is_fn = (fve->type.kind == TY_FN);
                if (any_field_emitted) buf_puts(&lit, ", ");
                any_field_emitted = true;
                char *mfn = mangle_field_name(def->fields[i].name);
                if (is_fn_field && val_is_fn) {
                    /* Phase E: use typed fn-ptr cast for concrete fields; fall
                     * back to int64_t carrier for generic/bare :fn fields. */
                    const char *fn_td = (def->fields[i].full_type &&
                                         def->fields[i].full_type->kind == TY_FN)
                        ? register_fn_ptr_typedef(def->fields[i].full_type) : NULL;
                    if (fn_td) {
                        buf_printf(&lit, ".%s = (%s)%s", mfn, fn_td, fv);
                    } else {
                        buf_printf(&lit, ".%s = (int64_t)(intptr_t)%s", mfn, fv);
                    }
                } else {
                    /* Bridge when a pointer-typed value is assigned to an int64_t
                     * carrier field (e.g. packed existential into :exists field). */
                    TypeKind vek = e->as.make_struct_.field_values[i]->type.kind;
                    bool val_is_c_ptr = (vek == TY_PTR_VOID || vek == TY_RC
                                         || vek == TY_REF || vek == TY_WEAK
                                         || vek == TY_EXISTS || vek == TY_FORALL
                                         || vek == TY_CSTR);
                    bool field_is_c_ptr = (def->fields[i].kind == TY_PTR_VOID
                                           || def->fields[i].kind == TY_RC
                                           || def->fields[i].kind == TY_WEAK
                                           || def->fields[i].kind == TY_REF
                                           || def->fields[i].kind == TY_LREF
                                           || def->fields[i].kind == TY_CSTR);
                    if (val_is_c_ptr && !field_is_c_ptr) {
                        buf_printf(&lit, ".%s = (int64_t)(intptr_t)%s", mfn, fv);
                    } else {
                        buf_printf(&lit, ".%s = %s", mfn, fv);
                    }
                }
                free(mfn);
                free(fv);
            }
            /* If every field was elided (every value was `(default-of T)`),
             * emit `{0}` rather than the empty `{}` (which is a GNU extension,
             * not standard C99). */
            if (!any_field_emitted) buf_puts(&lit, "0");
            buf_puts(&lit, "}");
            buf_putc(&lit, '\0');
            /* end-to-end-monomorphization: heap-box the by-value header literal.
             * Emits `T *tmp = malloc(sizeof(T)); *tmp = (T){...};` and returns
             * the typed pointer `tmp`, which is the C representation of the
             * :heap type `(T A)`.  This is the boxing step that replaces a
             * separate `heap-box` primitive -- the constructor itself owns it. */
            if (ms_heap && strcmp(struct_c_name, "int64_t") != 0) {
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                           struct_c_name, tmp, struct_c_name, struct_c_name);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*%s = %s;\n", tmp, lit.data);
                buf_free(&lit);
                /* Carrier base: the function returns the int64 carrier, so pack
                 * the typed pointer.  Concrete spec: return the typed pointer
                 * (`Vec__int *`) directly -- it IS the type of `(Vec A)`. */
                if (ms_heap_abstract_base) {
                    Buf cb; buf_init(&cb);
                    buf_printf(&cb, "(int64_t)(intptr_t)%s", tmp);
                    buf_putc(&cb, '\0');
                    free(tmp);
                    char *r = strdup(cb.data);
                    buf_free(&cb);
                    return r;
                }
                return tmp;
            }
            char *result = strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        case EX_SET_LIT: {
            /* Phase X3: #s(e1 e2 ...) -> tur_set_from_items(n, (int64_t[]){v1, v2, ...}) */
            uint32_t n = e->as.set_lit_.n;
            /* Evaluate each item into a temp so side effects happen before the call */
            char **item_vals = malloc(n * sizeof(char *));
            for (uint32_t i = 0; i < n; i++) {
                item_vals[i] = emit_value(ctx, body, e->as.set_lit_.items[i]);
            }
            Buf lit; buf_init(&lit);
            buf_printf(&lit, "tur_set_from_items(%u, (int64_t[]){", n);
            for (uint32_t i = 0; i < n; i++) {
                if (i > 0) buf_puts(&lit, ", ");
                buf_puts(&lit, item_vals[i]);
                free(item_vals[i]);
            }
            free(item_vals);
            buf_puts(&lit, "})");
            buf_putc(&lit, '\0');
            char *result = strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        case EX_GET_FIELD: {
            /* (.field s) - emit s.field_name; for rc<Struct> receivers,
             * auto-deref through the rc-block's value pointer.
             * CS1b: for parameterized struct receivers that arrive via the
             * int64_t carrier ABI (TY_STRUCT with n_type_params > 0), cast
             * through the unspecialized carrier typedef so field access is
             * valid C even though the C parameter type is int64_t.
             * Phase D: for pass-by-ptr params (const T*), use -> instead of . */
            char *sv = emit_value(ctx, body, e->as.get_field_.struct_expr);
            StructDef *def = e->as.get_field_.def;
            /* SC7: a transparent int newtype IS its single field -- the access
             * is the identity (the value already holds the int64 payload). */
            if (type_is_transparent_int_newtype(e->as.get_field_.struct_expr->type)) {
                return sv;
            }
            const char *fname_raw = def->fields[e->as.get_field_.field_idx].name;
            char *fname = mangle_field_name(fname_raw);
            /* end-to-end-monomorphization: a :heap receiver is a typed pointer
             * (`Vec__int *`) in monomorphic code -- field access dereferences
             * directly (`(sv)->field`), bypassing the carrier/by-value/pbp
             * dichotomy below.  Resolve the receiver type through the active
             * spec so an element-erased / tyvar receiver still classifies. */
            {
                Type recv_rty = emit_resolve_type(ctx,
                    e->as.get_field_.struct_expr->type);
                /* Only the typed-pointer (concrete monomorphic) receiver derefs
                 * directly.  An abstract-element receiver in a carrier base
                 * (e.g. `vec-len-byval`'s `[A]` base, where `(Vec A)` is the
                 * int64 carrier because A has no concrete layout) must fall
                 * through to the element-agnostic carrier-deref path below;
                 * `(sv)->field` there would deref an int64_t. */
                if (type_is_concrete_heap_struct(recv_rty)) {
                    Buf hb; buf_init(&hb);
                    buf_printf(&hb, "(%s)->%s", sv, fname);
                    buf_putc(&hb, '\0');
                    free(sv);
                    free(fname);
                    char *r = strdup(hb.data);
                    buf_free(&hb);
                    return r;
                }
            }
            bool through_rc = e->as.get_field_.struct_expr->type.kind == TY_RC;
            bool through_carrier = !through_rc
                && e->as.get_field_.struct_expr->type.kind == TY_STRUCT
                && def->n_type_params > 0;
            /* M5 single-body-two-ABIs (docs/upcoming/m5-residual-straddle-
             * retirement.md, Finding 5): a by-value Vec/Cons helper written in
             * pure Turmeric -- e.g. `(defn vec-len-byval [A] [v : (Vec A)]
             * (.len v))` -- has a `(Vec A)` (TY_APP) receiver.  Its
             * monomorphized spec receives `v` by value (direct `.field`), but
             * its carrier base (kept alive for the uniform dictionary slot,
             * reached on untyped/abstract-element dispatch) gets `v` as the
             * int64 carrier and must deref through the carrier typedef
             * (`.len`/`.data`/`.cap` offsets are element-agnostic).  Gate
             * precisely on the param's tracked representation: cast only when
             * the receiver is a carrier-represented EX_VAR param (its
             * `emit_byvalue_carrier_abi` flag is false -- C type is the int64
             * carrier).  A by-value TY_APP receiver (Option/Result/Tuple specs,
             * value-struct payloads -- flag true) keeps direct field access. */
            if (!through_rc && !through_carrier && def->n_type_params > 0
                && e->as.get_field_.struct_expr->type.kind == TY_APP
                && e->as.get_field_.struct_expr->kind == EX_VAR
                && e->as.get_field_.struct_expr->as.var.binding
                && !e->as.get_field_.struct_expr->as.var.binding->emit_byvalue_carrier_abi) {
                through_carrier = true;
            }
            /* M4c Path A.2 (docs/upcoming/m4c-execution-plan.md): inside an
             * instance-method spec where the receiver is a class-var param,
             * the spec's arg type is the concrete monomorphized struct
             * (e.g. Tuple2__int__int) — by-value, no `n_type_params`.  The
             * elab-time receiver type is still bare `Tuple2` (TY_STRUCT
             * with type_params), so `through_carrier` defaults to true.
             * Override: walk to the param binding and consult the spec's
             * `arg_types[]`; a concrete by-value struct → direct `.field`
             * access, no carrier cast. */
            if (through_carrier
                && ctx->current_abi_specialization
                && ctx->current_abi_specialization->fn
                && ctx->current_abi_specialization->fn->owner_instance
                && e->as.get_field_.struct_expr->kind == EX_VAR) {
                Binding *recv_b = e->as.get_field_.struct_expr->as.var.binding;
                FnDef *fd = ctx->current_abi_specialization->fn;
                for (uint8_t pi = 0; pi < fd->n_params
                                     && pi < ctx->current_abi_specialization->n_args; pi++) {
                    if (fd->params[pi] == recv_b) {
                        Type spec_arg = ctx->current_abi_specialization->arg_types[pi];
                        StructDef *sa_def = NULL;
                        Type sa_args[16]; uint8_t sa_n = 0;
                        if (type_extract_struct_app(&spec_arg, &sa_def, sa_args, &sa_n)
                            && sa_def && sa_def == def) {
                            through_carrier = false;
                        } else if (spec_arg.kind == TY_STRUCT && spec_arg.as.struct_.def
                                   && !spec_arg.as.struct_.def->is_opaque
                                   && spec_arg.as.struct_.def->n_type_params == 0) {
                            through_carrier = false;
                        }
                        break;
                    }
                }
            }
            bool through_pbp = !through_rc && !through_carrier
                && expr_is_pbp_param(ctx, e->as.get_field_.struct_expr);
            /* Direction (1) of polymorphic-ok-in-typeclass-instance-method-...:
             * for Result__T__B / Option__T whose parametric field landed on a
             * value-struct, the field slot is a heap pointer (`T *`) not the
             * inline T value, so the access dereferences. Mirrors the
             * struct_field_c_type rule that picks the pointer layout. */
            bool field_is_heap_ptr_for_value_struct = false;
            if (def && def->name &&
                (strcmp(def->name, "Result") == 0 ||
                 strcmp(def->name, "Option") == 0)) {
                /* Resolve any TY_TYVARs in the receiver's type via the
                 * current spec context so the struct-app args are the
                 * concrete monomorphized types (e.g. User), not the
                 * unbound A/B tyvars from the polymorphic source. */
                Type rt = emit_resolve_type(ctx, e->as.get_field_.struct_expr->type);
                Type field_resolved = e->type;
                if (rt.kind == TY_APP || rt.kind == TY_STRUCT) {
                    Type extracted_args[16];
                    uint8_t n_extracted = 0;
                    StructDef *extracted_def = NULL;
                    if (type_extract_struct_app(&rt, &extracted_def, extracted_args, &n_extracted) &&
                        extracted_def && extracted_def == def) {
                        const StructField *f = &def->fields[e->as.get_field_.field_idx];
                        if (f->full_type) {
                            field_resolved =
                                substitute_struct_app_type(f->full_type, def, extracted_args);
                        }
                    }
                }
                if (field_resolved.kind == TY_STRUCT && field_resolved.as.struct_.def &&
                    !field_resolved.as.struct_.def->is_opaque &&
                    field_resolved.as.struct_.def->n_type_params == 0) {
                    field_is_heap_ptr_for_value_struct = true;
                }
            }
            Buf lit; buf_init(&lit);
            if (through_rc) {
                if (field_is_heap_ptr_for_value_struct)
                    buf_printf(&lit, "(*((%s *)((RcControlBlock *)(%s))->value)->%s)",
                               def->name, sv, fname);
                else
                    buf_printf(&lit, "((%s *)((RcControlBlock *)(%s))->value)->%s",
                               def->name, sv, fname);
            } else if (through_carrier) {
                if (field_is_heap_ptr_for_value_struct)
                    buf_printf(&lit, "(*((%s *)(intptr_t)(%s))->%s)", def->name, sv, fname);
                else
                    buf_printf(&lit, "((%s *)(intptr_t)(%s))->%s", def->name, sv, fname);
            } else if (through_pbp) {
                if (field_is_heap_ptr_for_value_struct)
                    buf_printf(&lit, "(*(%s)->%s)", sv, fname);
                else
                    buf_printf(&lit, "(%s)->%s", sv, fname);
            } else {
                if (field_is_heap_ptr_for_value_struct)
                    buf_printf(&lit, "(*(%s).%s)", sv, fname);
                else
                    buf_printf(&lit, "(%s).%s", sv, fname);
            }
            buf_putc(&lit, '\0');
            free(sv);
            free(fname);
            char *result = strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        /* Phase HRT1: rank-2 polymorphic function wrapper */
        case EX_POLY_WRAP: {
            if (!e->as.poly_wrap_.wrapper_binding) {
                if (e->as.poly_wrap_.is_closure) {
                    /* Phase CCL: fat closure — pack into tur_poly_fn_t.
                     * Typed closures store __fn as a function pointer field;
                     * polymorphic closures keep the legacy int64_t carrier. */
                    char *fat = emit_value(ctx, body, e->as.poly_wrap_.inner);
                    char *tmp = fresh_tmp(ctx);
                    Binding *thunk_binding = emit_expr_closure_fn_binding(e->as.poly_wrap_.inner);
                    char *thunk_typedef = NULL;
                    Type thunk_params[MAX_FN_ARITY];
                    uint8_t thunk_arity = 0;
                    if (thunk_binding && thunk_binding->type.kind == TY_FN) {
                        thunk_arity = thunk_binding->type.as.fn.arity > 0
                            ? (uint8_t)(thunk_binding->type.as.fn.arity - 1) : 0;
                        for (uint8_t i = 0; i < thunk_arity; i++) {
                            thunk_params[i] = emit_fn_arg_type_from_type(thunk_binding->type, (uint8_t)(i + 1));
                        }
                        thunk_typedef = ensure_typed_thunk_typedef(
                            ctx, ctx->file,
                            emit_fn_result_type_from_type(thunk_binding->type),
                            thunk_arity ? thunk_params : NULL,
                            thunk_arity);
                    }
                    indent_buf(body, ctx->indent);
                    /* The closure value may be carried as int64_t (a let-bound
                     * :ptr<void> closure value) or as a void *; cast through
                     * intptr_t so both forms convert without an int-to-pointer
                     * warning. */
                    buf_printf(body, "void *%s = (void *)(intptr_t)(%s);\n", tmp, fat);
                    free(fat);
                    Buf out; buf_init(&out);
                    if (thunk_typedef) {
                        buf_printf(&out,
                            "(tur_poly_fn_t){ %s, "
                            "(int64_t(*)(void*,int64_t))(*( %s *)(%s)) }",
                            tmp, thunk_typedef, tmp);
                    } else {
                        buf_printf(&out,
                            "(tur_poly_fn_t){ %s, "
                            "(int64_t(*)(void*,int64_t))(intptr_t)((int64_t*)%s)[0] }",
                            tmp, tmp);
                    }
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(thunk_typedef);
                    free(tmp);
                    return result;
                }
                /* Phase HRT4: pass-through — inner is already a tur_poly_fn_t, emit directly. */
                return emit_value(ctx, body, e->as.poly_wrap_.inner);
            }
            /* Emit (tur_poly_fn_t){ NULL, wrapper_fn_name }.
             * Phase F: cast to int64_t(*)(void*,int64_t) to match the field type;
             * concrete call sites reverse this cast via the concrete dispatch path. */
            char *wn = raw_name_for_binding(e->as.poly_wrap_.wrapper_binding);
            Buf out; buf_init(&out);
            buf_printf(&out, "(tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))%s }", wn);
            buf_putc(&out, '\0');
            free(wn);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        /* A#1: fat-closure auto-shim.  Build a 2-slot heap fat struct
         *   { int64_t __fn = wrapper, int64_t __orig = bare_fn_ptr }
         * plus a file-scope env-ignoring wrapper thunk whose signature mirrors a
         * real closure thunk ( <ret> (void *env, params...) ).  The wrapper reads
         * the bare fn pointer from slot 1 and forwards the args, so a fat-call
         * consumer (reactor cb, free-bind kont) can invoke a non-capturing fn
         * through the standard { thunk, env } protocol -- retiring the historical
         * capture-forcing dummy.  The value carries TY_PTR_VOID like EX_CLOSURE, so
         * the surrounding arg-emission casts treat it identically to a closure. */
        case EX_FN_TO_FAT: {
            const Expr *inner = e->as.fn_to_fat_.inner;
            Type fnty = inner->type;
            uint8_t arity = (fnty.kind == TY_FN) ? fnty.as.fn.arity : 0;

            /* Emit the bare fn pointer value (typically the lifted fn's C name). */
            char *fnptr = emit_value(ctx, body, inner);

            /* Heap-allocate { shim, orig_fn_ptr }.  For an all-int64_t closure
             * signature the preamble __tur_fatshim<arity> shim (int64_t carrier
             * ABI, shared with TUR_APPLY and the reactor's fat-call casts) is
             * used.  For a non-int64 signature (closure-typed-invocation-abi-plan)
             * a per-signature typed shim is emitted so slot 0 matches the typed-
             * thunk cast the call site applies -- otherwise a :float/:cstr/:ptr
             * closure would be invoked through a mismatched int64_t ABI. */
            Type fnt_result = (fnty.kind == TY_FN && fnty.as.fn.result_full_type)
                                  ? *fnty.as.fn.result_full_type
                                  : emit_type_from_kind(fnty.kind == TY_FN
                                                            ? fnty.as.fn.result_kind
                                                            : TY_INT);
            Type fnt_params[MAX_FN_ARITY];
            for (uint8_t i = 0; i < arity && i < MAX_FN_ARITY; i++) {
                fnt_params[i] = (fnty.as.fn.arg_full_types && fnty.as.fn.arg_full_types[i])
                                    ? *fnty.as.fn.arg_full_types[i]
                                    : emit_type_from_kind(fnty.as.fn.arg_kinds[i]);
            }
            char *typed_shim = ensure_typed_fatshim(ctx, fnt_result, fnt_params, arity);

            char *fat_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t *%s = (int64_t *)malloc(2 * sizeof(int64_t));\n",
                       fat_tmp);
            indent_buf(body, ctx->indent);
            if (typed_shim) {
                buf_printf(body, "%s[0] = (int64_t)(intptr_t)%s;\n", fat_tmp, typed_shim);
            } else {
                buf_printf(body, "%s[0] = (int64_t)(intptr_t)__tur_fatshim%u;\n",
                           fat_tmp, (unsigned)arity);
            }
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s[1] = (int64_t)(intptr_t)%s;\n", fat_tmp, fnptr);
            char *ptr_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = %s;\n", ptr_tmp, fat_tmp);
            free(fnptr);
            free(fat_tmp);
            free(typed_shim);
            return ptr_tmp;
        }
        case EX_POLY_TO_FAT: {
            /* SC7: box a tur_poly_fn_t {env,fn} into a fat-closure handle
             * { __tur_poly_to_fat<N>, fn, env }.  The sink's N-ary fat-call
             * invokes the handle by calling slot 0 with the box itself as the
             * env, so the thunk reads the original fn (slot 1) and its captured
             * env (slot 2) and forwards every argument.  Slot 1 holds the
             * method's real N-ary thunk (make_poly_wrapper), so binary or
             * higher-arity poly methods round-trip into a matching ^fat sink. */
            const Expr *inner = e->as.poly_to_fat_.inner;
            char *pv = emit_value(ctx, body, inner);
            char *pf = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_poly_fn_t %s = %s;\n", pf, pv);
            char *box = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t *%s = (int64_t *)malloc(3 * sizeof(int64_t));\n", box);
            /* poly-to-fat-typed-shim-plan + multiarg fix: pick the slot-0 shim
             * arity from the sink's declared ^fat fn signature.  When that
             * signature is concrete and non-int64, ensure_typed_poly_to_fat
             * emits a typed shim whose ABI matches the typed-thunk cast the sink
             * applies on invocation -- otherwise a :float/:cstr arg would be read
             * through the int64 carrier ABI from the wrong register class.  For
             * an int64/pointer signature (or no threaded signature) the preamble
             * __tur_poly_to_fat<N> shim already matches; keep it (churn-free). */
            char *poly_shim = NULL;
            uint32_t poly_arity = 1;
            const Type *sft = e->as.poly_to_fat_.sink_fn_type;
            if (sft && sft->kind == TY_FN) {
                poly_arity = sft->as.fn.arity;
                Type pr = sft->as.fn.result_full_type
                              ? *sft->as.fn.result_full_type
                              : emit_type_from_kind(sft->as.fn.result_kind);
                Type pa[MAX_FN_ARITY];
                for (uint32_t i = 0; i < poly_arity && i < MAX_FN_ARITY; i++) {
                    pa[i] = (sft->as.fn.arg_full_types && sft->as.fn.arg_full_types[i])
                                ? *sft->as.fn.arg_full_types[i]
                                : emit_type_from_kind(sft->as.fn.arg_kinds[i]);
                }
                poly_shim = ensure_typed_poly_to_fat(ctx, pr, pa, poly_arity);
            }
            indent_buf(body, ctx->indent);
            if (poly_shim) {
                buf_printf(body, "%s[0] = (int64_t)(intptr_t)%s;\n", box, poly_shim);
            } else {
                buf_printf(body, "%s[0] = (int64_t)(intptr_t)__tur_poly_to_fat%u;\n",
                           box, (unsigned)poly_arity);
            }
            free(poly_shim);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s[1] = (int64_t)(intptr_t)%s.fn;\n", box, pf);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s[2] = (int64_t)(intptr_t)%s.env;\n", box, pf);
            char *ptr_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = %s;\n", ptr_tmp, box);
            free(pv);
            free(pf);
            free(box);
            return ptr_tmp;
        }
        /* Phase HRT1: type ascription — erase type, emit inner expression.
         * ACB (KB-004): when the inner expression is a carrier (int64_t) and
         * the ascribed type is a concrete aggregate, insert the bridge so the
         * downstream concrete consumer gets the struct value, not an int64_t. */
        case EX_ASCRIBE: {
            char *inner_val = emit_value(ctx, body, e->as.ascribe_.inner);
            /* KB-021: an ascription `(:: (vec-new) (Vec int))` pins the static
             * type for dispatch discrimination but must NOT change the runtime
             * representation of a carrier-ABI aggregate.  The carrier (int64_t
             * heap pointer) flows straight through; dereferencing it into a
             * by-value struct copy would both mismatch the carrier ABI of
             * stdlib functions (vec-push!, ok, ...) and break mutation/aliasing.
             * Only non-carrier (by-value) aggregates need the carrier->concrete
             * bridge here. */
            /* An opaque newtype (defopaque) has no fields and emits as int64_t
             * everywhere, so `(:: <int> :Opaque)` is a pure type relabel -- the
             * value is already the carrier.  Without this guard the bridge below
             * would treat it as a heap-pointer aggregate and emit a spurious
             * `*(int64_t *)(value)` dereference (segfault).
             *
             * A bare type variable is represented the same way: a def-less
             * TY_STRUCT (the int64 carrier for an unresolved param) or a
             * TY_TYVAR.  Ascribing an int into such a generic slot
             * (`(:: <int> :A)`, e.g. a phantom-parameterized opaque accessor)
             * is likewise a pure relabel -- the value already IS the carrier --
             * so it must not go through the by-value carrier bridge either.
             * See docs/reported/parameterized-defopaque.md. */
            bool ascribe_to_opaque =
                (e->type.kind == TY_STRUCT && e->type.as.struct_.def &&
                 e->type.as.struct_.def->is_opaque) ||
                (e->type.kind == TY_STRUCT && e->type.as.struct_.def == NULL) ||
                e->type.kind == TY_TYVAR;
            if (!ascribe_to_opaque &&
                e->as.ascribe_.inner->type.kind == TY_INT &&
                type_kind_is_aggregate(e->type.kind) &&
                !type_uses_carrier_abi(e->type)) {
                return emit_carrier_bridge(ctx, body, inner_val,
                                           CK_CARRIER, CK_CONCRETE, e->type);
            }
            /* M4 follow-up: also unbox an int→TY_APP cast when the TY_APP
             * resolves to a concrete struct app — but only when the inner
             * is a PLAIN int (not a carrier-handle).  Mirrors the
             * `emit_binding_repr_c_name` gate so emit decisions stay in
             * sync.  The carrier-relabel case (`(:: (vec-of) (Vec int))`)
             * has inner as a carrier-call whose value is already a Vec*;
             * dereferencing it would double-deref. */
            const Expr *inner_p = e->as.ascribe_.inner;
            while (inner_p && inner_p->kind == EX_ASCRIBE) inner_p = inner_p->as.ascribe_.inner;
            bool inner_is_plain_int =
                inner_p && (inner_p->kind == EX_INT_LIT
                    || (inner_p->kind == EX_VAR && inner_p->as.var.binding
                        && !inner_p->as.var.binding->emit_byvalue_carrier_abi
                        && inner_p->type.kind == TY_INT));
            if (!ascribe_to_opaque
                && e->as.ascribe_.inner->type.kind == TY_INT
                && e->type.kind == TY_APP
                && inner_is_plain_int) {
                Type resolved = emit_resolve_type(ctx, e->type);
                StructDef *rd = NULL;
                Type rargs[16]; uint8_t rn = 0;
                if (type_extract_struct_app(&resolved, &rd, rargs, &rn)
                    && rd && !rd->is_opaque) {
                    return emit_carrier_bridge(ctx, body, inner_val,
                                               CK_CARRIER, CK_CONCRETE,
                                               resolved);
                }
            }
            /* M4c-pre-ext symmetric `(:: x :int)` spill bridge (RETIRED --
             * M5 D.4, end-to-end-monomorphization plan): a by-value concrete
             * struct ascribed back to `:int` inside a Path A spec body used to
             * spill to a temp and hand back its address as the int64 carrier,
             * so a downstream carrier inline-C helper (`vec-len`/`vec-get`)
             * could consume it.  Option C (the by-value twin redirect in
             * emit_abi_try_byval_twin_redirect, emit_module.c) now retargets
             * those carrier-helper calls to their `*-byval` twins at the call
             * boundary -- including inside instance-method specs, where the
             * receiver arrives element-erased and the redirect recovers its
             * concrete type from the active spec's arg_types[].  So the
             * `(:: x :int)`-widen-a-by-value-struct idiom is no longer emitted
             * by any stdlib/spice code; an emit-c sweep over the full fixture
             * suite (incl. -Xdata-literals) confirms this CK_CONCRETE ->
             * CK_CARRIER site has zero producers.  The branch is deleted; a
             * by-value struct ascribed to `:int` now falls through to the
             * carrier-relabel return below (a no-op for an already-carrier
             * value). */
            return inner_val;
        }
        /* Phase HRT2 / EX1e / EXG1: existential pack.
         *   - Unconstrained: emit a bare scalar/pointer cast as in HRT2.
         *   - Constrained (n_witnesses > 0): allocate a tur_existential_t
         *     inline as the payload of an RcControlBlock (single allocation
         *     for record + witnesses flexible array), store the boxed value
         *     and one vtable pointer per constraint, and return the control
         *     block pointer cast to tur_exists_t.  The strong count starts
         *     at 1 at the pack site; auto-drop at the enclosing let scope
         *     exits will decrement and free the record (see EXG1-5 in
         *     elab_forms.c).  EX1e-3 will arrange for method dispatch on a
         *     value bound by `open` to read through these pointers when the
         *     open scrutinee is constrained. */
        case EX_EXISTS_PACK: {
            char *val = emit_value(ctx, body, e->as.exists_pack_.value);
            Buf out; buf_init(&out);
            TypeKind vk = e->as.exists_pack_.value->type.kind;
            uint8_t n_w = e->as.exists_pack_.n_witnesses;
            if (n_w > 0 && e->as.exists_pack_.witnesses) {
                /* EXG1-3: combined allocation through rc_cb_alloc.  Payload
                 * size covers the fixed record fields plus one void* per
                 * witness (flexible-array tail).
                 *
                 * EXG5-3: tag the block as RCK_EXISTENTIAL and, when the
                 * packed value is itself an RC-managed pointer, mark the
                 * payload as RCEXP_RC.  The cycle walker reads these tags
                 * to follow the inner rc through the existential record
                 * (see gc_mark_phase).
                 *
                 * EXG6-3: `(exists :linear ...)` opts out of rc allocation
                 * entirely -- the linear discipline guarantees a single
                 * open consumer, so we malloc the bare record and let
                 * EX_EXISTS_OPEN emit a free() at the end of its body. */
                bool is_linear =
                    e->type.kind == TY_EXISTS &&
                    e->type.as.forall_.is_linear;
                bool payload_is_rc =
                    (vk == TY_RC) ||
                    (vk == TY_WEAK) ||
                    (vk == TY_EXISTS &&
                     e->as.exists_pack_.value->type.as.forall_.n_constraints > 0);
                int payload_kind_const = payload_is_rc ? 1 /* RCEXP_RC */
                                                       : 0 /* RCEXP_OPAQUE */;
                char *cb_tmp = NULL;
                char *rec_tmp = fresh_tmp(ctx);
                if (is_linear) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "tur_existential_t *%s = (tur_existential_t *)malloc(sizeof(tur_existential_t) + (size_t)%u * sizeof(void *));\n",
                        rec_tmp, (unsigned)n_w);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "if (!%s) { fprintf(stderr, \"rc: out of memory\\n\"); abort(); }\n",
                        rec_tmp);
                } else {
                    cb_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "RcControlBlock *%s = rc_cb_alloc_kinded(sizeof(tur_existential_t) + (size_t)%u * sizeof(void *), %d, tur_existential_drop, 1 /* RCK_EXISTENTIAL */, %d);\n",
                        cb_tmp, (unsigned)n_w, (int)TY_PTR_VOID, payload_kind_const);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "tur_existential_t *%s = (tur_existential_t *)(%s->value);\n",
                        rec_tmp, cb_tmp);
                }
                indent_buf(body, ctx->indent);
                if (vk == TY_PTR_VOID || vk == TY_EXISTS || vk == TY_FORALL
                    || vk == TY_RC || vk == TY_REF || vk == TY_WEAK) {
                    buf_printf(body, "%s->value = (int64_t)(intptr_t)(%s);\n",
                               rec_tmp, val);
                } else {
                    buf_printf(body, "%s->value = (int64_t)(%s);\n", rec_tmp, val);
                }
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s->n_witnesses = %u;\n", rec_tmp, (unsigned)n_w);
                for (uint8_t wi = 0; wi < n_w; wi++) {
                    char dict_name[128];
                    emit_dict_name(dict_name, sizeof(dict_name),
                                   e->as.exists_pack_.witnesses[wi]);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s->witnesses[%u] = (void *)(&%s_singleton);\n",
                               rec_tmp, (unsigned)wi, dict_name);
                }
                /* EXG6-3: linear packs return the bare record pointer;
                 * non-linear packs return the rc-block cast (the open
                 * site reads through cb->value to reach the record). */
                if (is_linear) {
                    buf_printf(&out, "(tur_exists_t)(%s)", rec_tmp);
                } else {
                    buf_printf(&out, "(tur_exists_t)(%s)", cb_tmp);
                }
                if (cb_tmp) free(cb_tmp);
                free(rec_tmp);
            } else if (vk == TY_PTR_VOID || vk == TY_EXISTS || vk == TY_FORALL) {
                /* Already a pointer — cast directly */
                buf_printf(&out, "(tur_exists_t)(%s)", val);
            } else {
                /* Scalar (int64_t, bool, etc.) — reinterpret via intptr_t (64-bit safe) */
                buf_printf(&out, "(tur_exists_t)(intptr_t)((int64_t)(%s))", val);
            }
            buf_putc(&out, '\0');
            free(val);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        /* Phase HRT2: existential open — unbox void*, bind v, emit body */
        case EX_EXISTS_OPEN: {
            bool nil_result = (e->type.kind == TY_NIL);
            char *tmp = NULL;
            if (!nil_result) {
                tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(e->type), tmp);
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;

            /* Emit and unbox the packed value.
             * EX1e/EXG1: if the packed scrutinee is a constrained
             * existential, the runtime value is the RcControlBlock pointer
             * whose `value` field points at the inline tur_existential_t
             * record; we read the boxed payload through that indirection.
             * Otherwise the value was reinterpreted as void* (unchanged
             * from HRT2). */
            char *packed_val = emit_value(ctx, body, e->as.exists_open_.packed);
            char *var_name = name_for_binding(ctx, e->as.exists_open_.var_binding);
            bool packed_is_record =
                e->as.exists_open_.packed->type.kind == TY_EXISTS
                && e->as.exists_open_.packed->type.as.forall_.n_constraints > 0;
            /* EXG6-3: for a `:linear` existential, the packed value is the
             * tur_existential_t* itself (no surrounding rc-block), and the
             * open must free that record at the end of its body. */
            bool packed_is_linear_record =
                packed_is_record
                && e->as.exists_open_.packed->type.as.forall_.is_linear;
            indent_buf(body, ctx->indent);
            TypeKind vk = e->as.exists_open_.var_binding->type.kind;
            bool vk_is_ptr = (vk == TY_PTR_VOID || vk == TY_EXISTS || vk == TY_FORALL
                              || vk == TY_FN || vk == TY_RC || vk == TY_REF || vk == TY_WEAK);
            if (packed_is_linear_record) {
                if (vk_is_ptr) {
                    buf_printf(body,
                        "void *%s = (void *)(intptr_t)((tur_existential_t *)(%s))->value;\n",
                        var_name, packed_val);
                } else {
                    buf_printf(body,
                        "int64_t %s = ((tur_existential_t *)(%s))->value;\n",
                        var_name, packed_val);
                }
            } else if (packed_is_record) {
                if (vk_is_ptr) {
                    buf_printf(body,
                        "void *%s = (void *)(intptr_t)((tur_existential_t *)((RcControlBlock *)(%s))->value)->value;\n",
                        var_name, packed_val);
                } else {
                    buf_printf(body,
                        "int64_t %s = ((tur_existential_t *)((RcControlBlock *)(%s))->value)->value;\n",
                        var_name, packed_val);
                }
            } else if (vk_is_ptr) {
                buf_printf(body, "void *%s = (void *)(%s);\n", var_name, packed_val);
            } else {
                /* Unbox scalar via intptr_t */
                buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                           var_name, packed_val);
            }

            /* Suppress unused-variable warning */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(void)%s;\n", var_name);
            free(var_name);

            /* Emit body */
            if (nil_result) {
                emit_stmt(ctx, body, e->as.exists_open_.body);
            } else {
                char *bv = emit_value(ctx, body, e->as.exists_open_.body);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", tmp, bv);
                free(bv);
            }

            /* EXG6-3: free the bare record now that the (only) open has
             * consumed it.  The linear discipline guarantees this is the
             * single use, so the free is unconditional. */
            if (packed_is_linear_record) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "free((void *)(%s));\n", packed_val);
            }
            free(packed_val);

            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            return nil_result ? atom_nil() : tmp;
        }
        /* Phase G0: EX_DEFDATA — nothing to emit in value position (handled in Pass 0) */
        case EX_DEFDATA:
        /* Phase G1: EX_DEFGADT — same as EX_DEFDATA, handled in Pass 0 */
        case EX_DEFGADT:
            return atom_nil();

        /* Phase G0: EX_MATCH -- emit as statement-expression ({ ... result; }) */
        case EX_MATCH: {
            /* SS2: Session offer match -- TY_SESSION_OFFER scrutinee.
             * The scrutinee emits a tur_session_recv_tag() call (returns int64_t tag).
             * Each arm binds the channel pointer to its arm variable. */
            if (g_sessions_enabled &&
                e->as.match_.scrutinee->type.kind == TY_SESSION_OFFER) {
                const Expr *scrut = e->as.match_.scrutinee;
                bool nil_result = (e->type.kind == TY_NIL);
                char *tmp = NULL;
                if (!nil_result) {
                    tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
                }
                /* Evaluate the scrutinee (returns the tag as int64_t). */
                char *tag_val = emit_value(ctx, body, scrut);
                char *tag_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = %s;\n", tag_tmp, tag_val);
                free(tag_val);

                /* Get the channel C name from the scrutinee's val_exprs[0]. */
                char *chan_name = NULL;
                if (scrut->kind == EX_INLINE_C) {
                    InlineC *sc_ic = scrut->as.inline_c_.inline_c;
                    if (sc_ic->n_val_exprs > 0 && sc_ic->val_exprs[0]
                        && sc_ic->val_exprs[0]->kind == EX_VAR) {
                        chan_name = name_for_binding(ctx, sc_ic->val_exprs[0]->as.var.binding);
                    }
                }
                if (!chan_name) chan_name = strdup("NULL");

                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                bool first_arm = true;
                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->union_member_idx < 0) {
                        buf_puts(body, first_arm ? "{\n" : "else {\n");
                    } else {
                        if (first_arm) {
                            buf_printf(body, "if (%s == %d) {\n",
                                       tag_tmp, pat->union_member_idx);
                        } else {
                            buf_printf(body, "else if (%s == %d) {\n",
                                       tag_tmp, pat->union_member_idx);
                        }
                    }
                    first_arm = false;
                    ctx->indent += 4;

                    /* Bind the arm variable to the channel pointer.
                     * Session-derived types (pair, recv-pair, offer) all lower to
                     * void* at runtime -- a TurChannel pointer. */
                    if (pat->n_bindings > 0 && pat->bindings[0]) {
                        Binding *fb = pat->bindings[0];
                        char *bname = name_for_binding(ctx, fb);
                        const char *arm_c_type =
                            (fb->type.kind == TY_SESSION ||
                             fb->type.kind == TY_SESSION_RECV_PAIR ||
                             fb->type.kind == TY_SESSION_PAIR ||
                             fb->type.kind == TY_SESSION_OFFER)
                            ? "void *"
                            : type_c_name(fb->type);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = (void *)%s;\n",
                                   arm_c_type, bname, chan_name);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "(void)%s;\n", bname);
                        free(bname);
                    }

                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }
                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                free(chan_name);
                free(tag_tmp);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                return nil_result ? atom_nil() : tmp;
            }

            /* IT4: Union match -- tag-based dispatch via TUR_GETTAG / TUR_UNTAG.
             * Emits an if/else-if/else chain; each type-narrowing arm checks
             * the discriminant tag and binds the untagged value to the arm variable. */
            if (e->as.match_.scrutinee->type.kind == TY_UNION ||
                e->as.match_.scrutinee->type.kind == TY_ANY) {
                bool nil_result = (e->type.kind == TY_NIL);
                /* Use {0} for tur_tagged_t results, 0 for scalar results */
                const char *zero_init = (e->type.kind == TY_UNION ||
                                         e->type.kind == TY_ANY) ? "{0}" : "0";
                char *tmp = NULL;
                if (!nil_result) {
                    tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = %s;\n",
                               type_c_name(e->type), tmp, zero_init);
                }

                char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);

                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                /* Store scrutinee once to avoid double-evaluation */
                char *scrut_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_tagged_t %s = %s;\n", scrut_tmp, scrut_val);
                free(scrut_val);

                bool first_arm = true;
                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->union_member_idx < 0) {
                        /* Wildcard or bare-variable arm — catches all remaining cases */
                        buf_puts(body, first_arm ? "{\n" : "else {\n");
                    } else {
                        /* Type-narrowing arm — check discriminant tag */
                        if (first_arm) {
                            buf_printf(body, "if (TUR_GETTAG(%s) == %d) {\n",
                                       scrut_tmp, pat->union_member_idx);
                        } else {
                            buf_printf(body, "else if (TUR_GETTAG(%s) == %d) {\n",
                                       scrut_tmp, pat->union_member_idx);
                        }
                    }
                    first_arm = false;
                    ctx->indent += 4;

                    /* Bind the narrowed variable: untagged value cast to the arm type */
                    if (pat->n_bindings > 0 && pat->bindings[0]) {
                        Binding *fb = pat->bindings[0];
                        const char *ctype = type_c_name(fb->type);
                        char *bname = name_for_binding(ctx, fb);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = (%s)(intptr_t)TUR_UNTAG(%s);\n",
                                   ctype, bname, ctype, scrut_tmp);
                        free(bname);
                    }

                    /* Emit arm body */
                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }

                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                free(scrut_tmp);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");

                return nil_result ? atom_nil() : tmp;
            }

            /* Phase S4-lit: Literal pattern match -- scrutinee is a primitive type.
             * Emits an if/else-if chain comparing the scrutinee against literal values. */
            {
                bool _has_lit = false;
                for (uint32_t _ai = 0; _ai < e->as.match_.n_arms && !_has_lit; _ai++)
                    _has_lit = e->as.match_.arms[_ai].pattern.is_literal;
                if (_has_lit) {
                    bool nil_result = (e->type.kind == TY_NIL);
                    char *tmp = NULL;
                    if (!nil_result) {
                        tmp = fresh_tmp(ctx);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
                    }
                    char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);
                    TypeKind _sk = e->as.match_.scrutinee->type.kind;
                    bool _is_str   = (_sk == TY_CSTR);
                    bool _is_float = (_sk == TY_FLOAT || _sk == TY_FLOAT32 || _sk == TY_FLOAT64);
                    char *scrut_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    if (_is_str)
                        buf_printf(body, "const char *%s = (const char *)(intptr_t)(%s);\n",
                                   scrut_tmp, scrut_val);
                    else if (_is_float)
                        buf_printf(body, "double %s = (double)(%s);\n",
                                   scrut_tmp, scrut_val);
                    else
                        buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                                   scrut_tmp, scrut_val);
                    free(scrut_val);
                    bool _first = true;
                    for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                        MatchArm *arm = &e->as.match_.arms[ai];
                        MatchPattern *pat = &arm->pattern;
                        indent_buf(body, ctx->indent);
                        if (pat->is_wildcard || pat->is_var) {
                            buf_puts(body, _first ? "{\n" : "else {\n");
                        } else {
                            const char *kw = _first ? "if" : "else if";
                            switch (pat->lit_kind) {
                            case F_INT:
                                buf_printf(body, "%s (%s == (int64_t)%lldLL) {\n",
                                           kw, scrut_tmp, (long long)pat->lit_int);
                                break;
                            case F_BOOL:
                                buf_printf(body, "%s (%s == %s) {\n",
                                           kw, scrut_tmp, pat->lit_bool ? "1" : "0");
                                break;
                            case F_FLOAT:
                                buf_printf(body, "%s (%s == %g) {\n",
                                           kw, scrut_tmp, pat->lit_float);
                                break;
                            case F_STR: {
                                Buf _slit; buf_init(&_slit);
                                StrSlice _ss; _ss.p = pat->lit_cstr ? pat->lit_cstr : "";
                                _ss.len = (uint32_t)strlen(_ss.p);
                                emit_c_string(&_slit, _ss);
                                buf_putc(&_slit, '\0');
                                buf_printf(body, "%s (%s != NULL && strcmp(%s, %s) == 0) {\n",
                                           kw, scrut_tmp, scrut_tmp, _slit.data);
                                buf_free(&_slit);
                                break;
                            }
                            case F_NIL:
                                buf_printf(body, "%s (%s == 0) {\n", kw, scrut_tmp);
                                break;
                            default:
                                buf_printf(body, "%s (0) {\n", kw);
                                break;
                            }
                        }
                        _first = false;
                        ctx->indent += 4;
                        /* Bind var capture to scrutinee value */
                        if (pat->is_var && pat->var_binding) {
                            const char *_ctype = type_c_name(e->as.match_.scrutinee->type);
                            char *_vname = name_for_binding(ctx, pat->var_binding);
                            indent_buf(body, ctx->indent);
                            if (_is_str)
                                buf_printf(body, "const char *%s = %s;\n", _vname, scrut_tmp);
                            else if (_is_float)
                                buf_printf(body, "%s %s = (%s)%s;\n", _ctype, _vname, _ctype, scrut_tmp);
                            else
                                buf_printf(body, "%s %s = (%s)(intptr_t)%s;\n",
                                           _ctype, _vname, _ctype, scrut_tmp);
                            free(_vname);
                        }
                        if (!nil_result) {
                            char *bv = emit_value(ctx, body, arm->body);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s = %s;\n", tmp, bv);
                            free(bv);
                        } else {
                            emit_stmt(ctx, body, arm->body);
                        }
                        ctx->indent -= 4;
                        indent_buf(body, ctx->indent);
                        buf_puts(body, "}\n");
                    }
                    free(scrut_tmp);
                    return nil_result ? atom_nil() : tmp;
                }
            }

            /* TP6: scrutinee may carry a TY_APP wrapper (e.g. `:(Opt2 int)` parameter).
             * Unwrap to find the base TY_ADT before accessing the def. */
            AdtDef *adt;
            {
                const Type *base = &e->as.match_.scrutinee->type;
                while (base && base->kind == TY_APP && base->as.app.fn) {
                    base = base->as.app.fn;
                }
                adt = (base && base->kind == TY_ADT) ? base->as.adt_.def
                                                     : e->as.match_.scrutinee->type.as.adt_.def;
            }
            char adt_c_name[256];
            {
                /* TS4P3: Use the monomorphised struct name when the scrutinee is
                 * a concrete ADT app (e.g. tur_adt_Maybe__float instead of tur_adt_Maybe). */
                const char *inst_name = (e->as.match_.scrutinee->type.kind == TY_APP)
                    ? type_register_adt_app(e->as.match_.scrutinee->type) : NULL;
                if (inst_name) {
                    snprintf(adt_c_name, sizeof(adt_c_name), "%s", inst_name);
                } else {
                    char *_mn = mangle_field_name(adt->name);
                    snprintf(adt_c_name, sizeof(adt_c_name), "tur_adt_%s", _mn);
                    free(_mn);
                }
            }

            bool nil_result = (e->type.kind == TY_NIL);
            char *tmp = NULL;
            if (!nil_result) {
                tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                /* Initialize to zero to silence -Wsometimes-uninitialized; exhaustiveness
                 * is guaranteed by the elaborator so the default branch is unreachable. */
                buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
            }

            /* Emit scrutinee */
            char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);

            /* Phase G4: Check if any arm has a guard */
            bool has_any_guard = false;
            for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                if (e->as.match_.arms[ai].guard) { has_any_guard = true; break; }
            }

            if (has_any_guard) {
                /* Phase G4: Emit as if-chain with goto for guard fallthrough */
                char *end_label = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
                           adt_c_name, adt_c_name, scrut_val);
                free(scrut_val);

                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->is_var) {
                        buf_puts(body, "{\n");
                    } else {
                        buf_printf(body, "if (__scrut->tag == %u) {\n", pat->ctor->tag);
                    }
                    ctx->indent += 4;

                    /* Bind fields */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        char *_mctor = mangle_field_name(pat->ctor->name);
                        for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                            Binding *fb = pat->bindings[bi];
                            const char *ctype = type_c_name(fb->type);
                            char *bname = name_for_binding(ctx, fb);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s %s = (%s)__scrut->as.%s._%u;\n",
                                       ctype, bname, ctype, _mctor, bi);
                            free(bname);
                        }
                        free(_mctor);
                    }

                    /* Emit guard */
                    if (arm->guard) {
                        char *gv = emit_value(ctx, body, arm->guard);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "if (%s) {\n", gv);
                        free(gv);
                        ctx->indent += 4;
                    }

                    /* Emit body */
                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "goto __%s;\n", end_label);

                    if (arm->guard) {
                        ctx->indent -= 4;
                        indent_buf(body, ctx->indent);
                        buf_puts(body, "}\n");
                    }

                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                indent_buf(body, ctx->indent);
                buf_printf(body, "__%s:;\n", end_label);

                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                free(end_label);
            } else {
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
                           adt_c_name, adt_c_name, scrut_val);
                free(scrut_val);

                indent_buf(body, ctx->indent);
                buf_puts(body, "switch (__scrut->tag) {\n");

                bool has_default = false;
                /* Phase G0: Track emitted constructor tags to skip redundant
                 * arms (arms whose constructor was already covered by an
                 * earlier arm).  The elaborator emits a warning for these;
                 * the emitter must not produce a duplicate case label. */
                bool emitted_tags[256] = {false};
                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    /* Skip redundant constructor arms (duplicate case label) */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        uint32_t tag = pat->ctor->tag;
                        if (tag < 256 && emitted_tags[tag]) continue;
                        if (tag < 256) emitted_tags[tag] = true;
                    }

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->is_var) {
                        buf_puts(body, "default: {\n");
                        has_default = true;
                    } else {
                        buf_printf(body, "case %u: {\n", pat->ctor->tag);
                    }
                    ctx->indent += 4;

                    /* Bind field variables for constructor patterns */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        char *_mctor = mangle_field_name(pat->ctor->name);
                        for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                            Binding *fb = pat->bindings[bi];
                            const char *ctype = type_c_name(fb->type);
                            /* Use name_for_binding to get the canonical C name */
                            char *bname = name_for_binding(ctx, fb);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s %s = (%s)__scrut->as.%s._%u;\n",
                                       ctype, bname, ctype, _mctor, bi);
                            free(bname);
                        }
                        free(_mctor);
                    }

                    /* Emit body */
                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }

                    indent_buf(body, ctx->indent);
                    buf_puts(body, "break;\n");
                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                /* If no default/wildcard arm, add a default that does nothing
                 * (exhaustiveness is checked at elab time) */
                if (!has_default) {
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "default: break;\n");
                }

                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
            }

            return nil_result ? atom_nil() : tmp;
        }
        /* GF1: Generator creation -- emit struct/fns to file scope, call create fn */
        case EX_GEN: {
            const GenDef *def = e->as.gen_.def;
            emit_gen_functions(ctx, def);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = %s(", tmp, def->create_fn);
            for (uint32_t i = 0; i < def->n_captures; i++) {
                if (i > 0) buf_puts(body, ", ");
                char *cap_name = name_for_binding(ctx, def->captures[i]);
                buf_puts(body, cap_name);
                free(cap_name);
            }
            buf_puts(body, ");\n");
            return tmp;
        }
        /* GF1: Advance generator -- call _next via function pointer in struct header */
        case EX_GEN_NEXT: {
            char *gen_v = emit_value(ctx, body, e->as.gen_next_.gen_expr);
            char *tmp   = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = ((__tur_gen_hdr_t *)(%s))->__next_fn(%s);\n",
                       tmp, gen_v, gen_v);
            free(gen_v);
            return tmp;
        }
        /* GF1: Check generator exhaustion -- read __state field via common header */
        case EX_GEN_DONE: {
            char *gen_v = emit_value(ctx, body, e->as.gen_done_.gen_expr);
            char *tmp   = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "bool %s = (((__tur_gen_hdr_t *)(%s))->__state == -1);\n",
                       tmp, gen_v);
            free(gen_v);
            return tmp;
        }
        /* GF1: Yield in expression position -- forward to stmt emitter, return nil */
        case EX_YIELD: {
            emit_stmt(ctx, body, e);
            return atom_nil();
        }
        /* AR8: Build a right-folded cons list for a variadic rest parameter.
         * (f a b c d) where f takes [x y & rest :int] emits:
         *   __tur_cons_of((int64_t)(intptr_t)(c), __tur_cons_of((int64_t)(intptr_t)(d), 0LL))
         * as the last argument. Empty rest list emits 0LL (nil).
         *
         * The (int64_t)(intptr_t) wrap is the same coercion every fixed-arity
         * call boundary applies; it is a no-op for `int64_t` rvalues (the
         * common case: numeric literals, opaque handles, closure carriers)
         * and a needed coercion for function-pointer rvalues -- e.g. when
         * the rest type is a polymorphic `:A` that unifies to a bare defn
         * (`void *(*)(int64_t)` or similar). Without the cast, clang
         * rejects the function-pointer pass with -Wint-conversion. See
         * docs/upcoming/variadic-rest-closure-cast-plan.md. */
        case EX_CONS_LIST: {
            uint32_t n = e->as.cons_list_.n;
            if (n == 0) return strdup("0LL");
            /* Right-fold: start from the rightmost element and build leftward */
            char *tail = strdup("0LL");  /* nil sentinel */
            for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
                char *head = emit_value(ctx, body, e->as.cons_list_.items[i]);
                Buf cell; buf_init(&cell);
                buf_printf(&cell, "__tur_cons_of((int64_t)(intptr_t)(%s), %s)", head, tail);
                buf_putc(&cell, '\0');
                free(head);
                free(tail);
                tail = strdup(cell.data);
                buf_free(&cell);
            }
            return tail;
        }
    }
    return atom_nil();
}
