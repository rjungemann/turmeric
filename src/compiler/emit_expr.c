/* emit_expr.c -- expression-position C emission (emit_value and friends). */
#include "emit_internal.h"
#include "effect.h"     /* E2 fat-fn-value threading: EffectRow kind gate */
#include "globals.h"    /* g_dump_mono_specs, emit knobs */
#include "mono_specs.h" /* VBM3: van Laarhoven lens dispatch redirect */

/* ACB: true when kind represents a concrete aggregate type (struct, ADT, or
 * type-application) that the carrier ABI stores as a heap pointer.  Used by
 * KB-004 and KB-010 bridge insertion sites. */
/* S1/findings 16: hand the panic-hoist the return C type this builder just
 * spelled into its own call text (cast fn-ptr, thunk typedef, member fn).
 * Call as the LAST thing before returning the composed string -- nested
 * argument emissions clear the note (see EmitCtx.call_ret_note). */
static void note_call_ret(EmitCtx *ctx, const char *ct) {
    if (!ct || !*ct) return;
    snprintf(ctx->call_ret_note, sizeof ctx->call_ret_note, "%s", ct);
}

static bool type_kind_is_aggregate(TypeKind k) {
    return k == TY_STRUCT || k == TY_ADT || k == TY_APP;
}

static Type emit_fn_result_type_from_type(Type fn_type);

/* Emit a fat closure's env struct definition + its drop glue at file scope,
 * deduped through ctx->env_struct_names.  Extracted from the EX_CLOSURE
 * construction site so the CPS twin pre-pass (emit_cps_ir.c) can define the
 * struct BEFORE a `__cps` body that reads captures through it -- a CPS-emitted
 * capturing callback lands earlier in the file than its construction site.
 * `resolve_spec_params` mirrors the inner-closure-spec path (thunk param types
 * resolved under the active spec); the CPS pre-pass passes false. */
void emit_closure_env_struct_and_glue(EmitCtx *ctx, Buf *out,
                                      struct Closure *closure,
                                      const Symbol *env_name,
                                      bool resolve_spec_params) {
    bool already_emitted = false;
    if (ctx->env_struct_names) {
        for (uint8_t i = 0; i < ctx->n_env_struct_names; i++) {
            if (ctx->env_struct_names[i] == env_name) {
                already_emitted = true;
                break;
            }
        }
    }
    if (already_emitted) return;
    Type thunk_result = emit_resolve_type(ctx, emit_fn_result_type_from_type(closure->fn->binding->type));
    uint32_t thunk_arity = closure->fn->n_params > 0 ? (uint32_t)(closure->fn->n_params - 1) : 0;
    Type _rtp[MAX_FN_ARITY];
    Type *thunk_params = closure->fn->n_params > 1 ? &closure->fn->param_types[1] : NULL;
    if (thunk_params && resolve_spec_params) {
        for (uint8_t _t = 0; _t < thunk_arity; _t++)
            _rtp[_t] = emit_resolve_type(ctx, closure->fn->param_types[_t + 1]);
        thunk_params = _rtp;
    }
    char *thunk_typedef = ensure_typed_thunk_typedef(ctx, out, thunk_result, thunk_params, thunk_arity);
    /* Track this env struct */
    if (ctx->n_env_struct_names >= ctx->cap_env_struct_names) {
        ctx->cap_env_struct_names = ctx->cap_env_struct_names ? ctx->cap_env_struct_names * 2 : 8;
        ctx->env_struct_names = (const Symbol **)realloc(ctx->env_struct_names,
            ctx->cap_env_struct_names * sizeof(const Symbol *));
    }
    ctx->env_struct_names[ctx->n_env_struct_names++] = env_name;

    /* Phase HKT §5: fat closure struct -- __fn first, then captures. */
    if (thunk_typedef) {
        buf_printf(out, "struct %s { %s __fn; ", env_name->name, thunk_typedef);
    } else {
        buf_printf(out, "struct %s { int64_t __fn; ", env_name->name);
    }
    for (uint8_t i = 0; i < closure->n_captures; i++) {
        Binding *captured = closure->captures[i];
        /* Edge 1: a letrec/named-let member referenced from a nested
         * closure is captured eagerly (its globalness is unknown when
         * collect_free_vars runs), but if it turned out captureless it
         * is lifted as a directly-callable global with no env -- emit
         * no field for it (it is reached by its C symbol, not a slot).
         * Globals are never legitimately captured elsewhere, so this
         * only affects that one case. */
        if (captured && captured->is_global) continue;
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
        buf_printf(out, "%s %s; ", field_ctype, field);
        free(field);
    }
    buf_puts(out, "};\n");
    free(thunk_typedef);

    /* closure-drop-glue (Model R): per-env drop-glue so an escaping env
     * can be released generically (opaque-C teardown or scope-exit)
     * through its env[-1] header.  Releases each refcounted owning
     * capture (rc decrement, balancing the env-fill retain) then frees
     * the base.  MUST stay in lockstep with the emit_fns.c twin site
     * (that pre-pass normally wins the shared env_struct_names guard;
     * this arm is the fallback) so the rc retain/release always pairs.
     * Non-refcounted owning captures await move analysis -- see the
     * emit_fns.c site for the rationale. */
    {
        buf_printf(out,
            "static void drop_glue_%s(void *__p) {\n", env_name->name);
        buf_printf(out,
            "    struct %s *__e = (struct %s *)__p; (void)__e;\n",
            env_name->name, env_name->name);
        for (int32_t i = (int32_t)closure->n_captures - 1; i >= 0; i--) {
            Binding *cap = closure->captures[i];
            if (cap && cap->is_global) continue;
            if (cap && cap->type.kind == TY_RC) {
                char *cf = raw_name_for_binding(cap);
                buf_printf(out,
                    "    if (__e->%s) { rc_strong_decrement(__e->%s); rc_free_queue_drain(); }\n",
                    cf, cf);
                free(cf);
            } else if (cap && cap->is_fat && !closure->fat_captures_borrowed &&
                       !(cap->closure_fn_binding && closure->fn &&
                         closure->fn->binding &&
                         cap->closure_fn_binding == closure->fn->binding)) {
                /* Type-honesty (a): release an OWNED `^fat` closure
                 * handle capture (see emit_fns.c twin site). Kept in
                 * lockstep so the move/walk pairing holds either site. */
                char *cf = raw_name_for_binding(cap);
                buf_printf(out, "    TUR_CLOSURE_DROP(__e->%s);\n", cf);
                free(cf);
            } else if (closure->capture_drop_insts &&
                       closure->capture_drop_insts[i] &&
                       closure->capture_drop_insts[i]->n_method_impls > 0 &&
                       closure->capture_drop_insts[i]->method_impls[0] &&
                       closure->capture_drop_insts[i]->method_impls[0]->binding) {
                /* Model R #1b: Drop-instance release (see emit_fns.c twin). */
                char *cf = raw_name_for_binding(cap);
                char *dm = raw_name_for_binding(
                    closure->capture_drop_insts[i]->method_impls[0]->binding);
                buf_printf(out, "    %s(__e->%s);\n", dm, cf);
                free(dm); free(cf);
            }
        }
        buf_puts(out,
            "    free((void *)((char *)__p - sizeof(void *)));\n}\n");
    }
}

/* CM3 (van-laarhoven-consumer-mono-plan): peel type-erasing wrappers off a
 * consumer call's lens argument and return the named global lens it resolves to
 * (NULL for an anonymous / runtime-selected lens -- those stay on Path A). */
static const char *cm_call_lens_name(const Expr *arg) {
    while (arg) {
        if (arg->kind == EX_ASCRIBE)          arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_FN_TO_FAT)   arg = arg->as.fn_to_fat_.inner;
        else if (arg->kind == EX_POLY_TO_FAT) arg = arg->as.poly_to_fat_.inner;
        else if (arg->kind == EX_POLY_WRAP)   arg = arg->as.poly_wrap_.inner;
        else break;
    }
    if (arg && arg->kind == EX_VAR && arg->as.var.binding &&
        arg->as.var.binding->name && arg->as.var.binding->name->name)
        return arg->as.var.binding->name->name;
    return NULL;
}

/* CM3-transitive: peel wrappers off a consumer call's lens arg and return its
 * `Binding *` if it is a variable (a forwarded lens param), else NULL. */
static const void *cm_call_lens_binding(const Expr *arg) {
    while (arg) {
        if (arg->kind == EX_ASCRIBE)          arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_FN_TO_FAT)   arg = arg->as.fn_to_fat_.inner;
        else if (arg->kind == EX_POLY_TO_FAT) arg = arg->as.poly_to_fat_.inner;
        else if (arg->kind == EX_POLY_WRAP)   arg = arg->as.poly_wrap_.inner;
        else break;
    }
    if (arg && arg->kind == EX_VAR && arg->as.var.binding)
        return arg->as.var.binding;
    return NULL;
}

/* world-resize / multi-field existential payloads: true when `t` lowers to a
 * by-value C aggregate (`World__int__int__int` etc.) that is WIDER than the
 * single int64 the existential carrier (`tur_exists_t`) holds. Such a payload
 * cannot ride the scalar `(int64_t)(aggregate)` cast the carrier ABI uses for
 * single-word handles (`SizedBuf`, `SizedVec int`), so `pack` must heap-box it
 * and `open` must read it back through the pointer. Excludes:
 *   - heap structs (`:heap`): already a typed pointer `T *` -- one word, fits;
 *   - opaque newtypes / transparent int-newtypes: lower to int64 -- fit;
 *   - scalars / non-struct kinds.
 * The two emit sites (pack, open) must agree, so both consult this predicate. */
static bool exists_payload_is_byval_aggregate(Type t) {
    if (type_is_heap_struct(t)) return false;          /* typed pointer T * */
    if (type_is_heap_adt(t)) return false;             /* typed pointer T * */
    if (type_is_transparent_int_newtype(t)) return false;  /* int64 */
    /* structdef-retirement DS-C: a packed by-value aggregate payload is a
     * lowered record ADT now (the TY_STRUCT / struct-app branches and the
     * def-based aggregate tail are dead). */
    if (t.kind == TY_ADT || t.kind == TY_APP) {
        /* CONV-S2: under defstruct-as-defadt a packed by-value struct is a
         * lowered record ADT (`Wm`, `LinesR`, `(World int int)`).  It is a
         * by-value aggregate, so it must be heap-boxed into the existential's
         * int64 `value` slot rather than ride the scalar `(int64_t)(aggregate)`
         * cast (a hard cc error).  Decide by the resolved C name being a real
         * aggregate (not the int64 carrier). */
        AdtDef *adef = NULL;
        if (t.kind == TY_ADT) adef = t.as.adt_.def;
        else type_extract_adt_app(&t, &adef, (Type[16]){0}, &(uint8_t){0});
        if (!adef || adef->is_heap) return false;
        const char *acn = type_struct_value_c_name(t);
        return acn && strcmp(acn, "int64_t") != 0;
    }
    return false;
}

/* KB-021/KB-031: a type's dictionary-dispatch instance body uses the carrier
 * ABI (int64_t parameter) exactly when its value representation is the carrier
 * (see type_uses_carrier_abi in emit_core.c).  Both the value-declaration path
 * and these dispatch callsites consult the single shared predicate so they can
 * never disagree about whether an argument is already a carrier. */
static bool type_uses_carrier_in_dispatch(Type t) {
    return type_uses_carrier_abi(t);
}

/* G6 (partial): a constrained generic differentiated ONLY by its return type
 * (e.g. `(defn re-cata [B] [alg : (fn [(ReF B)] B) e : Re] : B ...)` specialized
 * at B=int / bool / cstr) interns sibling specs with identical ARGUMENT types,
 * differing only in result.  The by-args spec lookup cannot tell them apart, so a
 * call whose own result is one primitive (int) silently grabbed a sibling spec of
 * another primitive (bool) -- a wrong-typed result and a `4 == bool`
 * -Wbool-compare.  When BOTH the call's and the spec's result types are distinct
 * *primitive* kinds they are genuinely different ABIs and must not match;
 * aggregate / carrier results (TY_APP/TY_STRUCT/tyvar, disambiguated by the
 * per-Expr* recording, not here) keep the existing behaviour.  This is the
 * spec-selection half of gap G6; the closure-thunk-per-carrier half (a sub-word
 * `bool` recursive closure) remains open. */
static bool emit_prim_result_kind(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT64: case TY_BOOL: case TY_CSTR: case TY_FLOAT:
        case TY_FLOAT32: case TY_FLOAT64: case TY_NIL: case TY_PTR_VOID:
            return true;
        default: return false;
    }
}
bool emit_spec_result_mismatch(Type call_result, Type spec_result) {
    return emit_prim_result_kind(call_result.kind) &&
           emit_prim_result_kind(spec_result.kind) &&
           call_result.kind != spec_result.kind;
}

/* KB-004/KB-021: find the ABI specialization (concrete-by-value clone) that an
 * EX_CALL resolves to, or NULL.  Factored out of the EX_CALL emit path so the
 * let-binding type can discover that a call returns a concrete struct by value
 * (e.g. a `tuple2`/`ok`/`thead` clone) rather than the int64_t carrier. */
static const EmitAbiSpecialization *find_matched_abi_spec(
        EmitCtx *ctx, const Expr *e, const Binding *fn_binding) {
    if (!ctx || !e || e->kind != EX_CALL) return NULL;
    /* MB2.5 (constrained-hkt-forall-mode-b-plan): a class-method call that
     * dispatches through the runtime dict param speaks the carrier ABI (the dict
     * slot stores the carrier instance method).  It must NOT pick up an M7
     * by-value instance spec: matching one would deref the carrier args to the
     * aggregate (`*(tur_adt_Maybe__int *)x`) while the dict slot takes int64, a cc
     * type error.  The aggregate box/unbox already happens at the poly-carrier
     * boundary (the caller).  Carrier-compatible functors (Box) never mint a
     * by-value spec, so this only affects by-value aggregate functors. */
    if (emit_call_is_dict_param_dispatch(ctx, e)) return NULL;
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
    bool saw_any = false;
    for (uint32_t i = 0; i < ctx->n_specialized_calls; i++) {
        if (ctx->specialized_call_exprs[i] != e) continue;
        saw_any = true;
        if (ctx->specialized_call_outer[i] == active_outer) {
            fallback_clone = ctx->specialized_call_names[i];
            saw_call = true;
            break;
        }
        /* Cross-spec fallback (M5 Finding 7) only applies when we are emitting
         * INSIDE some spec; a carrier base / top-level emit (active_outer==NULL)
         * must not pick up a spec-scoped entry, else e.g. the int64-returning
         * carrier base routes `(ok v)` to a by-value `ok__spec` (return-type
         * mismatch).  See the M2-completion primitive-payload construct path.
         *
         * Phase 5 carrier-bridge deletion: NEVER apply the cross-spec fallback to
         * a `#{Construct}` callee.  emit_call_name disambiguates a construct only
         * by the exact Expr* recording (a by-value spec and the carrier base
         * differ ONLY in return ABI), so the same shared `(some ...)` Expr*
         * recorded under a by-value option_map spec must NOT be reported as
         * by-value when this lookup runs under the sibling int64-result spec --
         * that disagreement made the if-merge temp by-value while the branch
         * emitted the carrier `some()` (a cc type error). Keep this lookup in
         * lockstep with emit by requiring an exact outer match for constructs. */
        if (active_outer != NULL && !saw_call &&
            !(fn_binding && fn_binding->is_construct_template)) {
            fallback_clone = ctx->specialized_call_names[i];
            saw_call = true;
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
    /* Spec-scoped call recorded under a non-matching outer: at the carrier
     * base / top-level (active_outer==NULL) it must NOT fall into the by-args
     * spec match below, which cannot tell a return-only-differentiated spec
     * (e.g. by-value `ok__spec`) from the carrier (identical arg types). */
    if (saw_any && active_outer == NULL) {
        return NULL;
    }
    /* option-consumer-retype-byvalue step 2: a 0-arg call (e.g. a `(none)` /
     * `(empty)` constructor) carries no argument types, so the structural
     * arg-type match below degenerates to "first spec of this binding" and
     * would route EVERY such call -- including carrier-context ones like
     * `(some? (none))` -- to a by-value spec the moment one is interned
     * (e.g. by a pure-Turmeric `option-map` body).  The per-Expr* recording
     * above is the only sound disambiguator for 0-arg constructors; if this
     * exact call was not recorded as specialized, it stays on the carrier. */
    if (e->as.call_.n_args == 0) {
        return NULL;
    }
    /* Same disambiguation for an N-arg `#{Construct}` callee (`(ok x)` /
     * `(err e)` / `(some x)`): a by-value spec and the int64 carrier base differ
     * ONLY in return ABI, not in argument types, so the structural by-args match
     * below cannot tell them apart.  The per-Expr* recording (handled above) is
     * the sound disambiguator; an unrecorded construct call stays on the
     * carrier.  Keeps this in lockstep with emit_call_name's identical guard. */
    if (fn_binding && fn_binding->is_construct_template) {
        return NULL;
    }
    for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
        if (spec->binding != fn_binding || spec->n_args != e->as.call_.n_args) continue;
        /* G6: do not match a return-differentiated sibling spec (e.g. the bool
         * `re-cata` clone for an int-result call). */
        if (emit_spec_result_mismatch(emit_resolve_type(ctx, e->type),
                                      spec->result_type)) {
            continue;
        }
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

/* MB2 (constrained-hkt-forall-mode-b-plan): true when the tail leaf of `e` (its
 * do/let/ascribe-unwrapped last expression) is a call to a fn whose DECLARED
 * result is a bare type variable AND that call is NOT resolved to a concrete
 * by-value monomorph spec here.  In that case the call is emitted returning the
 * int64 carrier, so a function whose C return type is a concrete pointer must
 * bridge int64->pointer at the return (else -Wint-conversion).  When a concrete
 * spec IS matched (the wide-by-value monomorphization path calls
 * `run_id__spec__...Point` returning the pointer), the value is already the
 * pointer and no bridge is wanted -- so this returns false. */
bool emit_tail_call_returns_tyvar_carrier(EmitCtx *ctx, const Expr *e) {
    for (;;) {
        if (!e) return false;
        switch (e->kind) {
            case EX_ASCRIBE: e = e->as.ascribe_.inner; continue;
            case EX_DO:
                if (e->as.do_.n == 0) return false;
                e = e->as.do_.items[e->as.do_.n - 1]; continue;
            case EX_LET:
            case EX_LETREC: e = e->as.let_.body; continue;
            default: break;
        }
        break;
    }
    if (!e || e->kind != EX_CALL || !e->as.call_.fn_binding ||
        e->as.call_.fn_binding->type.kind != TY_FN ||
        e->as.call_.fn_binding->type.as.fn.result_kind != TY_TYVAR)
        return false;
    return find_matched_abi_spec(ctx, e, e->as.call_.fn_binding) == NULL;
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
/* consolidation increment 2 (bind cell): does this type mention a named
 * tyvar anywhere in its spine / fn signature?  Used to recognize a CARRIER
 * base instance entry (`__inst_*_tyvar`) by its still-generic result. */
static bool emit_repr_type_mentions_tyvar(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR) return true;
    if (t->kind == TY_APP)
        return emit_repr_type_mentions_tyvar(t->as.app.fn) ||
               emit_repr_type_mentions_tyvar(t->as.app.arg);
    if (t->kind == TY_FN) {
        if (emit_repr_type_mentions_tyvar(t->as.fn.result_full_type)) return true;
        if (t->as.fn.arg_full_types)
            for (uint32_t i = 0; i < t->as.fn.arity; i++)
                if (emit_repr_type_mentions_tyvar(t->as.fn.arg_full_types[i]))
                    return true;
    }
    return false;
}

static bool call_returns_byvalue_aggregate(EmitCtx *ctx, const Expr *call) {
    if (!call || call->kind != EX_CALL) return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN || fb->body_is_inline_c) return false;
    /* option-consumer-retype-byvalue step 2: when this call resolves to an ABI
     * spec, the spec's return type is the actual C ABI in effect -- not the
     * generic declared return.  A spec whose result element collapsed to the
     * int64 carrier (e.g. a pure-Turmeric `option-map` whose `(fn [A] B)`
     * closure left `B` unresolved, so the spec returns `int64_t`) hands back
     * the carrier handle, NOT a by-value aggregate; reporting it as by-value
     * here makes the caller spill `&temp` (taking the address of an int64 that
     * already holds the carrier pointer).  Trust the matched spec. */
    const EmitAbiSpecialization *spec = find_matched_abi_spec(ctx, call, fb);
    if (spec) {
        Type sr = emit_resolve_type(ctx, spec->result_type);
        return type_uses_carrier_abi(sr) &&
               strcmp(emit_type_c_name(ctx, sr), "int64_t") != 0;
    }
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
/* Increment 4 stage 3, chokepoint 1 (repr-decision-function-plan), shared
 * predicate: the typed-pointer C name for a CONCRETE (tyvar-free) heap
 * container / :heap struct in a declaration position, or NULL when the
 * declared type keeps the erased carrier spelling.  Consulted by the
 * let-binding decl (emit_binding_repr_c_name) and -- since the
 * mut-map-reassign seam (2026-08-16) -- by the control-form merge-temp decl
 * and its ctype mirror, so the `^mut` rebinding path spells the same
 * protocol as the let-bind position.
 *
 * Guarded on the DECLARED type being tyvar-free, exactly like the shadow
 * check: a tyvar-DECLARED binding inside a generic body keeps the erased
 * spelling by design even when the active spec resolves it concrete --
 * without this guard the hoisted arm re-spelled spec-emitted stdlib
 * bodies (`x : (Map K V)` resolved to `(Map int int)`) and churned 140
 * fixtures' codegen. */
static const char *emit_repr_concrete_heap_ptr_c_name(EmitCtx *ctx,
                                                      Type declared,
                                                      ReprPosition pos) {
    Type rbt = emit_resolve_type(ctx, declared);
    if (!((type_is_heap_adt(rbt) || type_is_heap_struct(rbt)) &&
          !emit_repr_type_mentions_tyvar(&declared) &&
          !emit_repr_type_mentions_tyvar(&rbt) &&
          /* A BARE parametric ADT base (`Map` with its args erased, standing
           * for `(Map K V)` in a generic body) is the erased container, not a
           * concrete type -- its element types are gone, so "mentions tyvar"
           * cannot see them.  Keep the erased spelling; only a genuinely
           * non-parametric def (Line, n_type_params == 0) or a concrete app
           * takes the typed pointer. */
          !(rbt.kind == TY_ADT && rbt.as.adt_.def &&
            rbt.as.adt_.def->n_type_params > 0) &&
          repr_of(&rbt, pos) == REPR_HEAP_PTR))
        return NULL;
    const char *hn = emit_type_c_name(ctx, rbt);
    /* Lens family: a :heap record whose FIELDS disqualify
     * adt_is_byvalue_product (heap-struct fields -- `Line {a: Point}`)
     * c-names to the carrier while its ctor returns the typed pointer.
     * Ask the def for the pointer spelling directly so the binding
     * matches the value the ctor actually hands it. */
    if (strcmp(hn, "int64_t") == 0 && rbt.kind == TY_ADT && rbt.as.adt_.def)
        hn = adt_heap_ptr_c_name(rbt.as.adt_.def);
    return hn;
}

static const char *emit_binding_repr_c_name(EmitCtx *ctx, Type binding_ty,
                                            const Expr *init) {
    Type rbt = emit_resolve_type(ctx, binding_ty);

    /* Increment 4 stage 3, chokepoint 1 (repr-decision-function-plan): a
     * CONCRETE (tyvar-free) heap container / :heap struct binding is its
     * typed pointer, regardless of how the INIT spells the same bits --
     * consulting repr_of's protocol instead of inheriting the initializer's
     * erased int64 spelling.  The binding-emission arms below already bridge
     * an int64 init into a pointer decl (and consumers reinterpret back), so
     * this changes the declared spelling, never the value.  The first shadow
     * sweep measured 53 sites of this class (`(Vec int)` / `(MutableMap int
     * int)` / `Line` bound as int64_t); a tyvar-elemented heap app keeps the
     * erased carrier spelling (same bits, unresolved element).  Placed BEFORE
     * the carrier-ABI early return: the Line family (a :heap record with
     * heap-struct fields) is NOT carrier-ABI, and the early return would
     * hand back type_c_name's int64 spelling for it. */
    {
        const char *hn = emit_repr_concrete_heap_ptr_c_name(
            ctx, binding_ty, REPR_POS_LET_BIND);
        if (hn) return hn;
    }

    if (!type_uses_carrier_abi(rbt))
        return emit_type_c_name(ctx, binding_ty);

    const Expr *p = init;
    /* After KB-021 an ascription emits its inner value unchanged for carrier-ABI
     * targets, so the representation is that of the inner expression. */
    while (p && p->kind == EX_ASCRIBE) p = p->as.ascribe_.inner;
    if (!p) return emit_type_c_name(ctx, binding_ty);

    /* M4 follow-up: the former "ascribe a plain TY_INT to a TY_APP that
     * resolves to a concrete struct" bridge declared the binding with the
     * struct's by-value C type.  structdef-retirement DS-D: no struct-headed
     * app resolves to a concrete by-value layout anymore (a parametric
     * aggregate is a record ADT), so that gate never fired -- removed. */

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
    } else if (p->kind == EX_IF || p->kind == EX_LET || p->kind == EX_LETREC ||
               p->kind == EX_DO) {
        /* option-lowering-mid-migration: a control-form initialiser whose tail
         * leaves are all carrier producers (some/none/ok/err/__inst_) and none
         * by-value is emitted as the int64 carrier by
         * emit_control_result_temp_decl -- declare the binding as the carrier so
         * its C initialiser type-checks (downstream uses bridge via the var's
         * emit_byvalue_carrier_abi=false flag).  The exact mirror of the temp's
         * representation decision keeps the two halves consistent. */
        if (fn_body_tail_is_carrier_producer(p) &&
            !fn_body_tail_emits_byvalue_carrier_abi(ctx, p))
            return "int64_t";
    }

    /* Everything else (struct constructor literal, concrete-spec call result,
     * by-value var, or a control-form result temp declared via emit_type_c_name)
     * keeps the by-value concrete representation. */
    return emit_type_c_name(ctx, binding_ty);
}

/* Increment 4 stage 2 (repr-decision-function-plan): shadow-check a site's
 * representation decision against repr_of's intended protocol.  Active only
 * under --emit-abi-trace; NEVER changes what the site decided -- a mismatch
 * is one stderr line, which the stage-3 migration reads as its blast-radius
 * measurement.  `chosen_cty` is the C type the site actually declared; the
 * observed ReprForm is recovered from it plus the resolved type. */
/* `own_cty` is the type's OWN C spelling (what the type alone c-names to),
 * which the caller supplies because the two shadowed TUs reach it
 * differently: emission has a spec-substituting `emit_type_c_name(ctx, t)`,
 * while the ADT-layout emitter runs before any EmitCtx exists and uses the
 * ctx-free `type_c_name`.  Everything else about the recovery is shared. */
ReprForm repr_form_from_cty(Type resolved, const char *own_cty,
                            const char *cty) {
    size_t n = strlen(cty);
    bool is_ptr = n >= 1 && cty[n - 1] == '*';
    if (strcmp(cty, "int64_t") == 0) {
        /* A transparent int newtype's int64 decl IS the payload -- the same
         * scalar-bits form repr_of predicts (SC7); without this the shadow
         * reports a spelling identity as a disagreement. */
        if (type_is_transparent_int_newtype(resolved))
            return REPR_SCALAR_BITS;
        if (resolved.kind != TY_FN && resolved.kind != TY_ADT &&
            resolved.kind != TY_APP &&
            strcmp(own_cty, "int64_t") == 0 &&
            type_has_concrete_codegen_layout(&resolved))
            return REPR_SCALAR_BITS;   /* int64_t IS this scalar's spelling */
        return REPR_CARRIER_I64;
    }
    if (is_ptr) {
        if (resolved.kind == TY_FN) return REPR_FAT_HANDLE;
        if (type_is_heap_struct(resolved) || type_is_heap_adt(resolved))
            return REPR_HEAP_PTR;
        /* A pointer that is the type's own scalar spelling (cstr, Sym,
         * ptr<T> leaves) is the value's bits; any other pointer decl is a
         * heap-object handle. */
        if (strcmp(own_cty, cty) == 0 &&
            type_has_concrete_codegen_layout(&resolved))
            return REPR_SCALAR_BITS;
        return REPR_HEAP_PTR;
    }
    if (strcmp(cty, "bool") == 0 || strcmp(cty, "double") == 0 ||
        strcmp(cty, "float") == 0 || strcmp(cty, "void") == 0 ||
        strstr(cty, "int8_t") || strstr(cty, "int16_t") ||
        strstr(cty, "int32_t") || strstr(cty, "uint"))
        return REPR_SCALAR_BITS;
    return REPR_BYVAL_AGG;             /* a bare aggregate type name */
}

/* Slot positions -- an ADT/struct field, a container element, a generic
 * carrier sink -- are ONE MACHINE WORD by construction.  Scalar bits, a heap
 * pointer, a fat-closure handle, a boxed-aggregate pointer and the erased
 * carrier all occupy that word identically, and which one a given word holds
 * is decided by the STORE, not by the declaration (increment 3 consolidated
 * the store side behind `type_is_boxed_container_elem`).  So the only
 * question a declaration-recovered shadow can honestly ask at a slot is the
 * binary one the slot actually answers: **inline aggregate, or one word?**
 *
 * This narrows what the instrument claims rather than patching the shapes it
 * mis-reports -- the CPS-graduation rule from the meta-plan.  The first
 * corpus sweep measured the cost of not doing it: all 84 adt-field lines
 * were one-word-spelled-two-ways (`cty=int64_t own=<T> *`), zero were seams.
 * Direct positions (param / result / let-bind) keep the full-resolution
 * comparison; their spelling IS the representation there, which is what let
 * chokepoint 1 migrate them.  The printed line always names the exact forms,
 * so a folded pair is still readable in a trace diff. */
static bool repr_pos_is_word_slot(ReprPosition pos) {
    return pos == REPR_POS_STRUCT_FIELD || pos == REPR_POS_CONTAINER_ELEM ||
           pos == REPR_POS_CARRIER_SINK;
}

static ReprForm repr_form_slot_class(ReprForm f, ReprPosition pos) {
    if (!repr_pos_is_word_slot(pos)) return f;
    return f == REPR_BYVAL_AGG ? REPR_BYVAL_AGG : REPR_CARRIER_I64;
}

void repr_shadow_report(const char *site, ReprPosition pos, Type resolved,
                        ReprForm want, ReprForm got, const char *cty) {
    if (repr_form_slot_class(want, pos) == repr_form_slot_class(got, pos))
        return;
    Buf tb; buf_init(&tb);
    /* `own` is the type's own C spelling.  A sweep needs it to tell a real
     * seam ("the site declared something other than what this type c-names
     * to") from a spelling identity ("both are the same word, named two
     * ways") without re-deriving it by hand per line. */
    buf_printf(&tb, "repr-shadow %s %s type=", site, repr_position_name(pos));
    type_print(&tb, resolved);
    buf_printf(&tb, " want=%s got=%s cty=%s own=%s\n", repr_form_name(want),
               repr_form_name(got), cty, type_c_name(resolved));
    buf_putc(&tb, '\0');
    repr_shadow_disagree(site, false, tb.data);
    buf_free(&tb);
}

static void repr_shadow_check(EmitCtx *ctx, const char *site,
                              ReprPosition pos, Type declared_ty,
                              const char *chosen_cty) {
    if (!repr_shadow_active() || !chosen_cty) return;
    Type resolved = emit_resolve_type(ctx, declared_ty);
    repr_shadow_report(site, pos, resolved, repr_of(&resolved, pos),
                       repr_form_from_cty(resolved,
                                          emit_type_c_name(ctx, resolved),
                                          chosen_cty),
                       chosen_cty);
}

/* Increment 0's deferred coverage gap, closed (2026-08-16).
 *
 * The trace note said: "ad-hoc spill sites NOT routed through the named
 * chokepoints do not trace -- those sites becoming chokepoint calls is
 * increments 2-4's job, and the trace will grow with them."  The audit found
 * 14 of them in this file, all one shape: an inline
 * `(int64_t)(intptr_t)(val)` reinterpreting a pointer-ish value into the
 * int64 carrier at a call boundary.
 *
 * They are CORRECT -- a pure reinterpret is what `emit_carrier_bridge` itself
 * emits for a heap value, which is why the corpus and the fuzzer are clean --
 * but each was an independent `buf_printf`, so none of them appeared in the
 * representation trace and none could be counted.  Routing them here changes
 * no emitted byte and makes the population visible.
 *
 * Deliberately NOT `emit_carrier_bridge`: that chokepoint spills, boxes, and
 * resolves spec types, and these sites have already established their value
 * is carrier-shaped.  Sending them through it would change behavior, which is
 * a migration with its own measurement, not this one. */
char *emit_carrier_reinterpret(EmitCtx *ctx, char *val, const Type *t,
                               const char *site) {
    (void)ctx;
    Buf b; buf_init(&b);
    buf_printf(&b, "(int64_t)(intptr_t)(%s)", val);
    buf_putc(&b, '\0');
    if (g_emit_abi_trace)
        fprintf(stderr, "repr-trace argcast %s %s\n", site,
                t ? repr_form_name(repr_of(t, REPR_POS_CARRIER_SINK))
                  : "unknown");
    free(val);
    char *out = strdup(b.data);
    buf_free(&b);
    return out;
}

/* KB-021: true when emitting `e` yields a *by-value* concrete carrier-ABI
 * aggregate rather than the int64_t carrier.  Such a value must be bridged to
 * the carrier before a dictionary-dispatch / carrier-ABI call; an already-
 * carrier value must not.  By-value producers are: a struct constructor literal
 * (EX_MAKE_STRUCT), a var/param declared by-value (tracked on the binding), and
 * a call resolving to a concrete-by-value ABI specialization. */
static bool emit_var_spec_arg_type(EmitCtx *ctx, const Expr *var_expr, Type *out);

/* result-block-value-double-unboxed: true when `t` is a LOWERED PARAMETRIC
 * by-value product app -- `(Result H cstr)` -> `tur_adt_Result__H__cstr`,
 * `(Box2 int cstr)` -> `tur_adt_Box2__int__cstr` -- which is held in C as a
 * concrete by-value AGGREGATE, never the int64 carrier.
 *
 * This is a different question from `type_uses_carrier_abi`, and for this
 * family the two are opposites.  `type_uses_carrier_abi` asks "does this ride
 * the int64 carrier ABI"; such an app answers NO, via the
 * `adt_app_is_byvalue_product` early-out.  A binding of that type therefore
 * also gets `emit_byvalue_carrier_abi = false`, since that flag means "a
 * CARRIER-ABI type that is, in this instance, held by-value".
 *
 * So a tail that is a bare var of such a type answered "no" to both, the
 * if-branch merge in emit_if concluded the arm was a carrier producer, and
 * bridged carrier->concrete over an already-concrete struct:
 *
 *   error: operand of type 'tur_adt_Result__H__cstr' where arithmetic or
 *          pointer type is required
 *
 * Call-shaped tails escaped this because the call predicates above answer from
 * the matched ABI spec instead; only a bare-var tail fell through.
 *
 * Restricted to TY_APP on purpose.  Extending it to a NON-parametric by-value
 * product (a plain `defstruct Pt`, lowered to `tur_adt_Pt`) regresses 10
 * fixtures -- the vec/map multiword-struct element paths and the assoc-type
 * returns -- because such a product DOES need the carrier treatment at those
 * seams, and reporting it as already-by-value suppresses a bridge it requires.
 * The same bare-var-tail bug is therefore still reachable for a non-parametric
 * product; closing it needs a predicate that distinguishes the two positions,
 * not a broader type test.  See
 * docs/reported/byvalue-product-tail-var-double-unboxed-nonparametric.md. */
static bool type_is_byvalue_product_app(EmitCtx *ctx, Type t) {
    Type rt = emit_resolve_type(ctx, t);
    if (rt.kind != TY_APP) return false;
    if (type_is_heap_adt(rt)) return false;
    return adt_app_is_byvalue_product(rt);
}

static bool expr_emits_byvalue_carrier_abi(EmitCtx *ctx, const Expr *e) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    if (!e) return false;
    /* Phase 5 carrier-bridge deletion: a control form (if/do/let) whose tail is
     * a by-value Option/Result producer is itself a by-value producer -- even
     * though its own `e->type` is collapsed to the int64 carrier (so the
     * type_uses_carrier_abi guard below would wrongly reject it).  Delegate to
     * the tail walker so a consumer of `(if b (some 1) (none))` sees the merge
     * result is already by-value and does NOT re-bridge it as a carrier handle. */
    if (e->kind == EX_IF || e->kind == EX_DO ||
        e->kind == EX_LET || e->kind == EX_LETREC)
        return fn_body_tail_emits_byvalue_carrier_abi(ctx, e);
    /* EX_CALL is checked BEFORE the e->type carrier guard: a return-only-poly
     * accessor (`ok-val`/`some`/...) has its declared result collapsed to the
     * int64 *scalar* (TY_INT) at elab, so the `type_uses_carrier_abi(e->type)`
     * guard below would wrongly reject it even when its matched by-value spec
     * resolves the result to a concrete aggregate (`Option__int`).  Consult the
     * spec directly so a generic loop's `(vec-push! acc (ok-val r))` heap-promotes
     * the by-value element instead of passing it raw into the int64 carrier slot. */
    if (e->kind == EX_CALL) {
        const EmitAbiSpecialization *spec =
            find_matched_abi_spec(ctx, e, e->as.call_.fn_binding);
        if (spec) {
            Type sr = emit_resolve_type(ctx, spec->result_type);
            return type_uses_carrier_abi(sr) &&
                   strcmp(emit_type_c_name(ctx, sr), "int64_t") != 0;
        }
        if (!type_uses_carrier_abi(e->type)) return false;
        return call_returns_byvalue_aggregate(ctx, e);
    }
    /* See type_is_byvalue_product_app: such a var answers "no" to
     * type_uses_carrier_abi (correctly -- it is not a carrier) and carries
     * emit_byvalue_carrier_abi = false (also correctly), yet is a concrete
     * aggregate in C.  It must report true here or the caller deref-unboxes
     * it.  Checked before the carrier guard, which would otherwise reject it. */
    if (e->kind == EX_VAR && e->as.var.binding &&
        type_is_byvalue_product_app(ctx, e->as.var.binding->type))
        return true;
    if (!type_uses_carrier_abi(e->type)) return false;
    if (e->kind == EX_MAKE_STRUCT) return true;
    /* G3 (instance-method by-value struct-field receiver): post-#482 a by-value
     * (non-heap) parametric struct FIELD -- e.g. an `(Option cstr)` field --
     * reads back as the embedded aggregate (`Option__cstr`), not the int64
     * carrier handle.  Passing it to a carrier-ABI dispatch needs the same
     * spill-and-address bridge a by-value local gets.  A `:heap` field (Cons,
     * Vec) is still carried as the int64 pointer, so it is excluded. */
    if (e->kind == EX_GET_FIELD) {
        Type ft = emit_resolve_type(ctx, e->type);
        return !type_is_heap_struct(ft) && type_has_concrete_codegen_layout(&ft);
    }
    if (e->kind == EX_VAR)
        return e->as.var.binding && e->as.var.binding->emit_byvalue_carrier_abi;
    return false;
}

/* The matched-spec result of a call argument, when that result is a concrete
 * by-value ADT-app (`(Option int)` -> `tur_adt_Option__int`).
 *
 * type_uses_carrier_abi answers FALSE for such a type (emit_core.c:431 -- it is
 * a concrete aggregate, not a carrier), and BOTH the shared reporters gate on
 * that predicate: expr_emits_byvalue_carrier_abi's EX_CALL arm and
 * fn_body_tail_byvalue_carrier_type_inner's.  So a spec-matched by-value-app
 * argument is invisible to the heap-container-insert bridge below, in the guard
 * and in the bridge type alike.
 *
 * Before nested-monomorph apps lowered by value this was unreachable: a
 * `(Result (Option int) cstr)` rode the int64 carrier, so `ok-val`'s spec
 * returned int64 and matched vec-push!'s int64 `val : A` slot with no bridge at
 * all.  Now the accessor returns the aggregate and the bridge is mandatory.
 *
 * Deliberately local rather than folded into either shared predicate: teaching
 * expr_emits_byvalue_carrier_abi to report these fires the escaping bridge at
 * every other carrier sink too, which breaks 7 fixtures (list/vec by-value
 * aggregate elements) by heap-promoting values that were already correct. */
static bool call_spec_result_byvalue_app(EmitCtx *ctx, const Expr *e, Type *out) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    if (!e || e->kind != EX_CALL) return false;
    /* Only the RETURN-ONLY-POLY shape, whose elab type was collapsed to the
     * int64 scalar.  An argument whose own type is already the by-value app
     * (a direct `(some 42)`) reaches the carrier slot through the CONV-S1
     * boxing block further down and must keep doing so -- routing it through
     * the escaping bridge instead breaks 7 list/vec element fixtures. */
    if (e->type.kind != TY_INT) return false;
    const EmitAbiSpecialization *spec =
        find_matched_abi_spec(ctx, e, e->as.call_.fn_binding);
    if (!spec) return false;
    Type sr = emit_resolve_type(ctx, spec->result_type);
    if (sr.kind != TY_APP || type_is_heap_adt(sr) ||
        !adt_app_is_byvalue_product(sr)) return false;
    if (out) *out = sr;
    return true;
}

/* CONV-S1/B3: true when `t` resolves to a by-value ADT (a flat aggregate, not the
 * int64 carrier).  Such a value crossing into an int64 carrier field slot (a
 * carrier ADT/GADT constructor field) must be boxed (malloc + copy) on the way in
 * and unboxed (deref) on the way out -- the byval<->carrier field crossing.  ADT
 * fields are always stored as int64 in the tagged-union typedef, so a by-value
 * child is never stored inline; the box/unbox bridge is mandatory at the seam. */
static bool emit_type_is_byvalue_adt(EmitCtx *ctx, Type t) {
    Type r = emit_resolve_type(ctx, t);
    if (r.kind == TY_ADT && r.as.adt_.def)
        /* seam 3: a :heap ADT is a typed POINTER, not a by-value aggregate, so it
         * is never boxed/unboxed across a carrier field slot -- it already IS a
         * pointer-sized carrier.  (The struct path excludes :heap structs the
         * same way via type_is_heap_struct.) */
        return !r.as.adt_.def->is_heap && adt_is_byvalue_product(r.as.adt_.def);
    /* Parametric-by-value: a concrete flat-product ADT-app value (`(Box int)`)
     * is a by-value aggregate too -- it crosses an int64 carrier field slot the
     * same way (box on store, unbox on field-bind read). */
    if (r.kind == TY_APP) {
        AdtDef *ad = type_adt_app_def(&r);
        if (ad && ad->is_heap) return false;
        return adt_app_is_byvalue_product(r);
    }
    return false;
}

/* catch-unwind-aggregate-return-miscompiled: when a catch boundary's thunk
 * returns a by-value AGGREGATE, the generic `TUR_APPLY0` call in
 * tur_catch_unwind_box is a function-pointer type mismatch -- it casts slot 0
 * to `int64_t (*)(void *)` while the thunk really returns the struct by value,
 * so the int64 that gets boxed as ok_val is whatever happened to be in the
 * return register.  The consumer then reads it as a `T *`.  Return the name of
 * a boxing trampoline for such a thunk (NULL when the generic path is right,
 * which is every scalar / pointer / :heap-ADT return). */
static const char *catch_thunk_box_shim(EmitCtx *ctx, const Expr *thunk, int *owns) {
    if (!ctx || !thunk) return NULL;
    /* The thunk reaches codegen as a fat-closure handle: the fn literal is
     * wrapped in an ascription (and possibly a fn-to-fat shim), whose own type
     * is `ptr<void>`.  Peel those to reach the TY_FN that still carries the
     * declared return type. */
    const Expr *fnexpr = thunk;
    for (int guard = 0; fnexpr && guard < 8; guard++) {
        if (fnexpr->kind == EX_ASCRIBE && fnexpr->as.ascribe_.inner)
            fnexpr = fnexpr->as.ascribe_.inner;
        else if (fnexpr->kind == EX_FN_TO_FAT && fnexpr->as.fn_to_fat_.inner)
            fnexpr = fnexpr->as.fn_to_fat_.inner;
        else break;
    }
    if (!fnexpr) return NULL;
    Type fnty = fnexpr->type;
    if (fnty.kind != TY_FN) return NULL;
    Type ret = fnty.as.fn.result_full_type
                   ? *fnty.as.fn.result_full_type
                   : emit_type_from_kind(fnty.as.fn.result_kind);
    Type rr = emit_resolve_type(ctx, ret);
    if (rr.kind == TY_FLOAT || rr.kind == TY_FLOAT32 || rr.kind == TY_FLOAT64) {
        if (owns) *owns = 0;
        return ensure_catch_bits_shim(ctx, rr);
    }
    if (!emit_type_is_byvalue_adt(ctx, ret)) return NULL;
    if (owns) *owns = 1;
    return ensure_catch_box_shim(ctx, rr);
}

/* B4 (byvalue-recursive-carrier): true when `t` resolves to a single-carrier
 * recursive ADT wrapper (Re/Expr) whose by-value representation is its int64
 * carrier.  At a fat-closure boundary such a value crosses as the raw int64
 * carrier (reinterpret), never the box/deref bridge emit_type_is_byvalue_adt
 * would otherwise drive. */
static bool emit_type_is_byval_recursive_carrier(EmitCtx *ctx, Type t) {
    Type r = emit_resolve_type(ctx, t);
    return r.kind == TY_ADT && r.as.adt_.def &&
           adt_is_byval_recursive_carrier_wrapper(r.as.adt_.def);
}

/* B4 (byvalue-recursive-carrier, slice 2): true when `t` resolves to a WIDE
 * (> 8 byte) by-value ADT.  As a parametric carrier monomorph element it is
 * stored as an int64 heap-box pointer (type_is_wide_byval_adt drives the boxing
 * in emit_registered_adt_app_rec), so a match binder reads the box POINTER raw
 * (no deref): the pointer rides the fat-closure boundary as the int64 carrier
 * and the thunk deref+copies it at entry. */
static bool emit_type_is_wide_byval_adt(EmitCtx *ctx, Type t) {
    return type_is_wide_byval_adt(emit_resolve_type(ctx, t));
}

/* B4 (byvalue-recursive-carrier): build the C expression that reconstructs the
 * by-value wrapper aggregate from its int64 carrier value -- a designated
 * compound literal `(tur_adt_X){ .as.<Ctor>._0 = (carrier) }`.  Valid only for a
 * single-carrier recursive wrapper (emit_type_is_byval_recursive_carrier);
 * returns a freshly malloc'd string the caller owns. */
static char *emit_byval_recursive_carrier_reconstruct(EmitCtx *ctx, Type t,
                                                       const char *carrier) {
    Type r = emit_resolve_type(ctx, t);
    const AdtDef *def = r.as.adt_.def;
    const char *cname = type_c_name(r);
    char *mp = adt_field_member_path(def, def->ctors[0], 0);
    Buf b; buf_init(&b);
    buf_printf(&b, "(%s){ .%s = (%s) }", cname, mp, carrier);
    buf_putc(&b, '\0');
    char *out = strdup(b.data);
    buf_free(&b);
    free(mp);
    return out;
}

/* Higher-kinded poly carrier (constrained-hkt-forall slice 3 codegen): a
 * by-value aggregate container `(f a)` (e.g. `(Option int)`) crossing the erased
 * `tur_poly_fn_t` carrier is represented as an int64 heap-box pointer -- malloc +
 * copy on the way in, deref on the way out, mirroring the EX_ANY inject/cast
 * boxing above and the B4 wide-byval convention.  Both helpers return a freshly
 * malloc'd string the caller owns. */
static char *emit_agg_box(EmitCtx *ctx, Type t, const char *val) {
    const char *cn = emit_type_c_name(ctx, emit_resolve_type(ctx, t));
    /* repr-trace: aggregate heap-boxed into an int64 slot (field store /
     * poly-carrier crossing / wide-byval element). */
    if (g_emit_abi_trace)
        fprintf(stderr, "repr-trace bridge agg-box %s\n", cn);
    Buf b; buf_init(&b);
    buf_printf(&b,
        "({ %s *__tur_pbox = (%s *)malloc(sizeof(%s)); "
        "*__tur_pbox = (%s); (int64_t)(intptr_t)__tur_pbox; })",
        cn, cn, cn, val);
    buf_putc(&b, '\0');
    char *out = strdup(b.data);
    buf_free(&b);
    return out;
}
static char *emit_agg_unbox(EmitCtx *ctx, Type t, const char *val) {
    const char *cn = emit_type_c_name(ctx, emit_resolve_type(ctx, t));
    /* repr-trace: int64 slot deref'd back to the by-value aggregate. */
    if (g_emit_abi_trace)
        fprintf(stderr, "repr-trace bridge agg-unbox %s\n", cn);
    Buf b; buf_init(&b);
    buf_printf(&b, "(*(%s *)(intptr_t)(%s))", cn, val);
    buf_putc(&b, '\0');
    char *out = strdup(b.data);
    buf_free(&b);
    return out;
}

/* G9 (carrier<->concrete): true when a field read `(.field recv)` materializes
 * a by-value (non-:heap) aggregate in C -- e.g. `(.head xs)` where xs is a
 * monomorphized `Cons__Option__int *`, so `(xs)->head` is an embedded
 * `Option__int` value, NOT the int64 carrier.  Unlike `expr_emits_byvalue_
 * carrier_abi`, this resolves the field type through the RECEIVER's concrete
 * type (consulting the active spec's arg_types for a spec-param receiver), so it
 * still fires when the field's own elaborated `e->type` was erased to the int64
 * carrier (a parametric `(Cons A)` head field).  Used to suppress a spurious
 * carrier->concrete reconstruction (the stale `tur_option_t *` cast) at a
 * dispatch arg that is already the concrete aggregate. */
static bool field_read_emits_byvalue_aggregate(EmitCtx *ctx, const Expr *e) {
    if (!ctx || !e || e->kind != EX_GET_FIELD || !e->as.get_field_.struct_expr)
        return false;
    const Expr *se = e->as.get_field_.struct_expr;
    /* Resolve the receiver-container's concrete type for the active spec: a spec
     * param keeps its erased parametric type, so prefer the spec's arg_types[]
     * (mirrors emit_reresolve_disp_type / emit_var_spec_arg_type). */
    Type rt;
    bool have_rt = false;
    if (se->kind == EX_VAR)
        have_rt = emit_spec_arg_type_for_binding(ctx, se->as.var.binding, &rt);
    if (!have_rt) rt = emit_resolve_type(ctx, se->type);

    uint32_t fidx = e->as.get_field_.field_idx;
    Type fr;
    bool fr_owned = false;
    bool have_fr = false;
    /* structdef-retirement DS-C: a get-field receiver is a lowered record ADT --
     * the TY_STRUCT and struct-app branches are dead.
     * Lowered record ADT-app receiver (`(Cons (Option int))`): the field is a
     * ctor field whose full_type is the container's type-param; substitute the
     * receiver's element args. */
    {
        AdtDef *ad = NULL; Type aargs[16]; uint8_t an = 0;
        const CtorDef *ctor = e->as.get_field_.adt_ctor;
        if (ctor && fidx < ctor->n_fields &&
            type_extract_adt_app(&rt, &ad, aargs, &an) && ad) {
            const CtorField *cf = &ctor->fields[fidx];
            if (cf->full_type && cf->full_type->kind == TY_TYVAR) {
                fr = substitute_adt_app_type_owned(cf->full_type, ad, aargs);
                fr_owned = (fr.kind == TY_APP);
                have_fr = true;
            } else if (cf->full_type) {
                fr = *cf->full_type;
                have_fr = true;
            }
        }
    }
    if (!have_fr) return false;

    bool result = !type_is_heap_struct(fr) && !type_is_heap_adt(fr) &&
                  ((type_uses_carrier_abi(fr) &&
                    type_has_concrete_codegen_layout(&fr)) ||
                   (fr.kind == TY_APP && type_app_is_concrete_adt(&fr)));
    if (fr_owned) free_struct_app_type(fr);
    return result;
}

/* instance-method-return-carrier-bridge: true when the tail leaf/leaves of `e`
 * already emit a *by-value* concrete carrier-ABI aggregate rather than the
 * int64 carrier handle.  Walks the same tail forms as
 * fn_body_tail_is_carrier_producer (ascribe/do/if/let), delegating the leaf
 * decision to expr_emits_byvalue_carrier_abi.
 *
 * Post-M2, a #{Construct} helper (ok/err/some/none) specialized at a concrete
 * call site lowers to its by-value `*__spec__*` clone, so a body whose tail is
 * `(ok (make-struct ...))` hands back the struct by value.  The carrier->concrete
 * return-deref in emit_fns.c must NOT fire for such a body -- dereferencing an
 * already-by-value aggregate as a heap pointer is a hard `cc` error (the
 * derive-decode-struct seam).  An `if` is by-value when EITHER branch is (the
 * two branches must agree in representation anyway; a mismatch is a separate
 * bug, and suppressing the deref is the safe direction). */
/* CONV-S1 seam 4 (bounded-wrapper struct return): true (writing the by-value
 * type to *out_ty) when `call` directly targets a callee that EMITS a by-value
 * concrete aggregate return -- a lowered defstruct / non-parametric record-ADT
 * (e.g. `Pos`/`Vel`), mirroring emit_fns.c's typed_byval_adt / typed_struct
 * signature decision.  A bounded typeclass wrapper's carrier base
 * (`any-get [S E] [(StorageOps S E)] ... : E`) bakes one concrete instance
 * method (`__inst_StorageOps_sop_hyget_Sparse_tyvarVel`) whose inline-C body
 * returns `tur_adt_Vel` BY VALUE, while the base's own C return is the int64
 * carrier -- so the base must spill that aggregate into the carrier on return.
 * Neither the matched-spec branch nor the carrier-ABI gate in
 * expr_emits_byvalue_carrier_abi catches it (the base carries no abi spec, and a
 * lowered defstruct is not carrier-ABI), so detect the callee's by-value
 * aggregate result directly.  Scoped to the return-bridge tail predicates so the
 * general by-value-producer classification is unchanged. */
static bool call_emits_byval_concrete_aggregate(EmitCtx *ctx, const Expr *call,
                                                Type *out_ty) {
    if (!call || call->kind != EX_CALL) return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN || !fb->type.as.fn.result_full_type)
        return false;
    /* Scope to a typeclass INSTANCE-METHOD callee (the same `__inst_` prefix
     * fn_body_tail_is_carrier_producer keys on).  A bounded wrapper's carrier
     * base bakes such a call; an ordinary recursive closure self-call (e.g. a
     * letrec `go` returning a by-value struct) must NOT trigger the carrier
     * return spill -- its enclosing return is the by-value aggregate, not the
     * int64 carrier, and broadening to all by-value-aggregate calls perturbed
     * that path. */
    if (!fb->name || !fb->name->name ||
        strncmp(fb->name->name, "__inst_", 7) != 0)
        return false;
    Type r = emit_resolve_type(ctx, *fb->type.as.fn.result_full_type);
    bool byval_aggr =
        (r.kind == TY_ADT || r.kind == TY_APP || r.kind == TY_STRUCT) &&
        !type_uses_carrier_abi(r) &&
        !type_is_heap_struct(r) && !type_is_heap_adt(r) &&
        type_has_concrete_codegen_layout(&r) &&
        strcmp(emit_type_c_name(ctx, r), "int64_t") != 0;
    if (byval_aggr && out_ty) *out_ty = r;
    return byval_aggr;
}

/* byvalue-option-if-join-function-call-arm-aggregate-cast: true (writing the
 * by-value type to *out_ty) when `call` directly targets an ORDINARY top-level
 * defn whose declared result is a concrete by-value aggregate -- e.g. `(known)`
 * with `(defn known [] : (Option int) ..)`.  Such a callee has a fixed by-value
 * C signature (`static tur_adt_Option__int known()`), so the call already yields
 * the aggregate; a carrier->concrete merge bridge (`(tur_option_t *)(intptr_t)
 * (known())`) would cast that struct rvalue to a pointer, which cc rejects as
 * "aggregate value used where an integer was expected".
 *
 * Scoped away from the two shapes the __inst_ / letrec-closure comment on
 * call_emits_byval_concrete_aggregate warns about: this requires a GLOBAL, NON-
 * generic (`!is_poly_fn && !poly_type`) callee, so a generic carrier base (whose
 * C return is the int64 carrier) and a local letrec closure self-call (whose
 * enclosing return is already the aggregate, not the carrier) are both excluded.
 * The __inst_ instance methods stay owned by call_emits_byval_concrete_aggregate
 * (they are not `is_global` defns). */
static bool call_ordinary_defn_byval_aggregate(EmitCtx *ctx, const Expr *call,
                                               Type *out_ty) {
    if (!call || call->kind != EX_CALL || call->as.call_.is_poly_call)
        return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN || !fb->type.as.fn.result_full_type)
        return false;
    if (!fb->is_global || fb->is_poly_fn || fb->poly_type) return false;
    if (fb->body_is_inline_c) return false;  /* handled by the inline-C seam */
    /* A `#{Construct}` template (some/none/ok/err) has context-dependent
     * lowering: in a carrier-returning context (e.g. inside a generic
     * `option_map` spec) it emits its bare int64-carrier base `some(..)`, not
     * the by-value monomorph -- so it is NOT unconditionally a by-value
     * aggregate producer.  Those are already classified by
     * call_construct_emits_byval_aggregate / call_spec_result_byval_aggregate,
     * which distinguish the by-value-spec case from the carrier base.  Excluding
     * them here keeps this predicate to ordinary user defns with a fixed
     * by-value C signature. */
    if (fb->is_construct_template) return false;
    /* Mirror emit_fns.c's C-return-type decision for a non-inline-C defn: the
     * signature is `emit_type_c_name(ctx, result_full_type)` (the concrete
     * aggregate), NOT the int64 carrier -- the `int64_t` fallback there is
     * reached only by inline-C bodies.  So the call yields the aggregate by
     * value whenever that c-name is a real struct name: not `int64_t`, and not a
     * `T *` pointer (a heap handle rides the int64 carrier).  The c-name test is
     * the source of truth -- `type_has_concrete_codegen_layout` reports false for
     * the abstract `(Option int)` TY_APP even where emit_type_c_name already
     * yields `tur_adt_Option__int`, so it must not gate here.  Likewise, unlike
     * call_emits_byval_concrete_aggregate, this does NOT gate on
     * `!type_uses_carrier_abi` -- a concrete `(Option int)` return is carrier-ABI
     * at the type level yet is still emitted by value from an ordinary
     * (non-generic) defn body. */
    Type r = emit_resolve_type(ctx, *fb->type.as.fn.result_full_type);
    const char *cn = emit_type_c_name(ctx, r);
    size_t cnlen = cn ? strlen(cn) : 0;
    bool byval_aggr =
        (r.kind == TY_ADT || r.kind == TY_APP || r.kind == TY_STRUCT) &&
        !type_is_heap_struct(r) && !type_is_heap_adt(r) &&
        cn && strcmp(cn, "int64_t") != 0 &&
        /* a real aggregate C name, not a pointer (`T *`) carrier handle */
        cn[cnlen - 1] != '*';
    if (byval_aggr && out_ty) *out_ty = r;
    return byval_aggr;
}

/* CONV-S1 seam 4 (none-base carrier return): true when `call` is a lowered
 * construct-template call (`(none)` -> `(Option false (default-of A))`, `(some
 * x)`, `(ok x)`) whose resolved result is a CONCRETE by-value record-ADT
 * aggregate -- exactly the case the EX_CALL ctor path (emit_expr.c:3351) emits as
 * the per-monomorph `ctor_<Name>__<args>` aggregate.  In a carrier-returning fn
 * (e.g. the generic base `none()` whose C return is int64) that aggregate tail
 * must be heap-spilled to the int64 carrier by the M5 return bridge.  Scoped to a
 * construct template with no matched by-value spec (the spec branch handles the
 * recorded case) so it is the fallback for the unrecorded carrier-base tail.
 * Inert at default: there Option/Result are structs, the ctor is the int64
 * carrier, and the resolved result's c-name is `int64_t` (excluded). */
static bool call_construct_emits_byval_aggregate(EmitCtx *ctx, const Expr *call,
                                                 Type *out_ty) {
    if (!call || call->kind != EX_CALL) return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb) return false;
    /* An ADT-constructor call -- a 0-arg ctor (fb : TY_ADT) or an N-arg ctor
     * (fb : TY_FN -> TY_ADT with no result_full_type), the same shapes the
     * EX_CALL ctor path at emit_expr.c:3283/3315 emits via `ctor_<Name>...`. */
    bool is_adt_ctor = fb->type.kind == TY_ADT ||
        (fb->type.kind == TY_FN &&
         fb->type.as.fn.result_kind == TY_ADT &&
         !fb->type.as.fn.result_full_type);
    if (!is_adt_ctor) return false;
    if (find_matched_abi_spec(ctx, call, fb) != NULL) return false;
    Type r = emit_resolve_type(ctx, call->type);
    const char *cn = emit_type_c_name(ctx, r);
    size_t cnlen = cn ? strlen(cn) : 0;
    /* Match the EX_CALL ctor suffix gate (emit_expr.c:3351): a value ADT ctor
     * emits `ctor_<Name>__<args>` by value.  Two concrete shapes qualify:
     *   - a concrete parametric app `(Option int)` (`type_app_is_concrete_adt`);
     *   - a bare non-parametric ADT `Wrap` -- a single-variant record or a
     *     fieldless enum -- which resolves to TY_ADT, not TY_APP.
     * The bare-TY_ADT case was missing, so a single-variant record ctor
     * (`(MkWrap b n)`) used as an `if`/`cond` arm whose merge temp is by-value
     * (e.g. the other arm calls a defn returning the same record) was wrongly
     * carrier-bridged -- `(*(tur_adt_Wrap *)(intptr_t)(ctor_...))` derefs the
     * by-value aggregate as an int handle: "aggregate value used where an
     * integer was expected".  In both shapes the value must not be a
     * carrier/heap value and its c-name must be a real aggregate (not
     * `int64_t`, not a `T *` handle); the def!=NULL guard rejects an open ADT
     * type variable. */
    bool byval_aggr =
        ((r.kind == TY_APP && type_app_is_concrete_adt(&r)) ||
         (r.kind == TY_ADT && r.as.adt_.def != NULL)) &&
        !type_uses_carrier_abi(r) &&
        !type_is_heap_struct(r) && !type_is_heap_adt(r) &&
        cn && strcmp(cn, "int64_t") != 0 &&
        cnlen > 0 && cn[cnlen - 1] != '*';
    if (byval_aggr && out_ty) *out_ty = r;
    return byval_aggr;
}

/* CONV-S1 seam 4 (none-base carrier return, spec sibling): true when `call` has a
 * matched ABI spec whose resolved result is a CONCRETE by-value aggregate (e.g.
 * `(:: (some x) :int)` -> `some__spec__tur_adt_Option__int_int64_t` returning
 * `tur_adt_Option__int`).  When such a call is the tail of a carrier-returning fn
 * (`opt-some [x] : int`), the aggregate must be heap-spilled to the int64 carrier
 * -- the by-value-spec analogue of call_construct_emits_byval_aggregate.  Scoped
 * to the return-tail walkers only (NOT expr_emits_byvalue_carrier_abi, whose
 * carrier-ABI gate intentionally excludes by-value specs so arg-position bridges
 * are unaffected).  Inert at default: no by-value spec exists there. */
static bool call_spec_result_byval_aggregate(EmitCtx *ctx, const Expr *call,
                                              Type *out_ty) {
    if (!call || call->kind != EX_CALL) return false;
    const EmitAbiSpecialization *spec =
        find_matched_abi_spec(ctx, call, call->as.call_.fn_binding);
    if (!spec) return false;
    Type sr = emit_resolve_type(ctx, spec->result_type);
    bool byval_aggr =
        (sr.kind == TY_APP || sr.kind == TY_ADT || sr.kind == TY_STRUCT) &&
        !type_uses_carrier_abi(sr) &&
        !type_is_heap_struct(sr) && !type_is_heap_adt(sr) &&
        strcmp(emit_type_c_name(ctx, sr), "int64_t") != 0;
    if (byval_aggr && out_ty) *out_ty = sr;
    return byval_aggr;
}

/* CONV-S1 seam 4 (closure call returning a by-value aggregate): a monadic
 * continuation call `(k x)` inside a by-value bind/ap spec (`(bind [ma k]
 * (if (some? ma) (k (.value ma)) (none)))`) is a poly (fat-closure) call whose
 * wrapper RETURNS the by-value carrier-ABI aggregate directly -- the EX_CALL
 * emit casts `k.fn` to `(R (*)(void*, A...))` and the call result IS the
 * aggregate (the `phase_f_concrete` path, ~emit_expr.c:2836).  So the call tail
 * already emits by-value and must NOT be re-bridged through the int64 carrier
 * (the `(tur_option_t *)(intptr_t)(...)` reconstruct would treat the aggregate
 * as a carrier pointer -- "aggregate value used where an integer was
 * expected").  Mirror the `phase_f_concrete` gate + a by-value-aggregate
 * result. */
static bool type_kind_is_poly_concrete(TypeKind k);
static bool closure_call_emits_byval_aggregate(EmitCtx *ctx, const Expr *e) {
    if (!e || e->kind != EX_CALL || !e->as.call_.is_poly_call) return false;
    if (e->as.call_.poly_arg_mask) return false;
    const Binding *fb = e->as.call_.fn_binding;
    bool typed_carrier = fb && fb->poly_type && fb->poly_type->kind == TY_FN;
    if (!typed_carrier && !type_kind_is_poly_concrete(e->type.kind)) return false;
    if (!typed_carrier) {
        for (uint32_t i = 0; i < e->as.call_.n_args; i++)
            if (!type_kind_is_poly_concrete(e->as.call_.args[i]->type.kind))
                return false;
    }
    Type rt = emit_resolve_type(ctx, e->type);
    return (rt.kind == TY_APP || rt.kind == TY_ADT) &&
           !type_uses_carrier_abi(rt) && !type_is_heap_adt(rt) &&
           strcmp(emit_type_c_name(ctx, rt), "int64_t") != 0;
}

bool fn_body_tail_emits_byvalue_carrier_abi(EmitCtx *ctx, const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_ASCRIBE:
            return fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.ascribe_.inner);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   fn_body_tail_emits_byvalue_carrier_abi(
                       ctx, e->as.do_.items[e->as.do_.n - 1]);
        case EX_IF:
            return e->as.if_.else_or_null &&
                   (fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.if_.then_) ||
                    fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.if_.else_or_null));
        case EX_MATCH:
            /* A match's tail value is one of its arm bodies; it emits the
             * by-value aggregate if any arm's tail does.  Without this a
             * match-valued branch (e.g. a nested `(match ... (RxIP m p2) ...)`
             * as an if/let arm) falls to the call predicates below, none match,
             * and the merge bridge deref-unboxes the already-by-value result --
             * "aggregate value used where an integer was expected". */
            for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++)
                if (fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.match_.arms[ai].body))
                    return true;
            return false;
        case EX_LET:
        case EX_LETREC:
            return fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.let_.body);
        default:
            if (call_emits_byval_concrete_aggregate(ctx, e, NULL))
                return true;
            if (call_ordinary_defn_byval_aggregate(ctx, e, NULL))
                return true;
            if (call_construct_emits_byval_aggregate(ctx, e, NULL))
                return true;
            if (call_spec_result_byval_aggregate(ctx, e, NULL))
                return true;
            if (closure_call_emits_byval_aggregate(ctx, e))
                return true;
            /* lens-composition-codegen-blockers (Blocker 2b): a fat-dispatched
             * call through a fn VALUE (`(. l1 get) s`) whose result the enclosing
             * spec resolves to a wide by-value ADT now emits that aggregate BY
             * VALUE -- the fat thunk cast return + materialization temp were
             * recovered (disp_result / fn_body_tail_byvalue_carrier_type).  Report
             * it so the control-merge bridge does NOT deref-unbox the already-by-
             * value result ("aggregate value used where an integer was expected").
             * Same gate as the type-returning sibling. */
            if (e->kind == EX_CALL) {
                const Binding *fcb = e->as.call_.fn_binding;
                if (fcb && !fcb->is_global && fcb->type.kind == TY_FN &&
                    !fcb->closure_fn_binding &&
                    (fcb->is_fat || fcb->type.as.fn.boxed) &&
                    fcb->type.as.fn.result_full_type &&
                    type_is_wide_byval_adt(
                        emit_resolve_type(ctx, *fcb->type.as.fn.result_full_type)))
                    return true;
            }
            return expr_emits_byvalue_carrier_abi(ctx, e);
    }
}

/* Phase 5 carrier-bridge deletion: the concrete by-value carrier-ABI type the
 * tail of `e` produces, or TY_UNKNOWN when the tail is not a by-value carrier
 * producer.  Mirrors fn_body_tail_emits_byvalue_carrier_abi but returns the
 * type -- recovered from the matched spec's resolved result (the source of
 * truth; the construct's own e->type is collapsed to the int64 carrier) or a
 * make-struct's type -- so a carrier-return slot can spill it concrete->carrier. */
static Type fn_body_tail_byvalue_carrier_type_inner(EmitCtx *ctx,
                                                    const Expr *e);

/* Increment 4 stage 3: the METHOD-RESULT carrier-production shadow.
 * Instrumenting the wrapper rather than each of this walker's ~20 return
 * points keeps it one chokepoint and costs no duplicated logic.
 *
 * The invariant being watched is what this function PROMISES its callers: a
 * type they can spell concretely on the far side of a carrier bridge.  The
 * first sweep corrected the spec here, which is the shadow doing its job --
 * the naive reading ("by-value carrier type" means a C aggregate) fired 5740
 * lines, 4495 of them heap containers (`(Vec int)`, `(Cons int)`, `Set`,
 * `Map`).  "By-value" in this walker's name is opposed to ERASED, not to
 * pointer: a heap container's typed pointer is a perfectly concrete far side,
 * and the crossing it feeds is the lossless pointer/carrier round trip --
 * the same distinction the struct-field sweep had to calibrate away.
 *
 * So the drift condition is narrower and sharper: the walker naming a type
 * that the protocol says is the ERASED CARRIER.  That would hand a caller an
 * int64 dressed as a concrete spelling, which is the shape increment 2 chased
 * through the `bind` cell -- the elab-side gate and the emit-side dispatch
 * pairing the wrapper ABI differently. */
Type fn_body_tail_byvalue_carrier_type(EmitCtx *ctx, const Expr *e) {
    Type t = fn_body_tail_byvalue_carrier_type_inner(ctx, e);
    if (repr_shadow_active() && t.kind != TY_UNKNOWN) {
        ReprForm got = repr_of(&t, REPR_POS_RESULT);
        if (got == REPR_CARRIER_I64) {
            Buf tb; buf_init(&tb);
            buf_puts(&tb, "repr-shadow method-result result type=");
            type_print(&tb, t);
            buf_puts(&tb, " want=concrete got=carrier-i64\n");
            buf_putc(&tb, '\0');
            repr_shadow_disagree("method-result", false, tb.data);
            buf_free(&tb);
        }
    }
    return t;
}

static Type fn_body_tail_byvalue_carrier_type_inner(EmitCtx *ctx,
                                                    const Expr *e) {
    Type unknown = type_simple(TY_UNKNOWN, CK_COPY);
    if (!e) return unknown;
    switch (e->kind) {
        case EX_ASCRIBE: {
            Type inner_t =
                fn_body_tail_byvalue_carrier_type_inner(ctx, e->as.ascribe_.inner);
            if (inner_t.kind != TY_UNKNOWN) return inner_t;
            /* consolidation increment 2 (bind cell, ascription form): the
             * inner recovery failed -- a carrier producer whose declared
             * result is still generic (`bind` at a partially-applied
             * instance head) -- but the ASCRIPTION names the concrete type:
             * `(:: (bind ...) (Result int int))` is the programmer stating
             * what the carrier holds.  Use it when the inner really is a
             * carrier producer and the ascribed type is a concrete by-value
             * carrier-ABI aggregate, so the let-init / boundary bridges can
             * re-wrap instead of assigning the raw int64 into the struct
             * (`invalid initializer`). */
            if (fn_body_tail_is_carrier_producer(e->as.ascribe_.inner)) {
                /* Not gated on type_uses_carrier_abi: post-lowering a
                 * parametric app (`(Result int int)`) is a BY-VALUE type for
                 * which that predicate reports false (the seam-4 rule); the
                 * test is "concrete non-heap aggregate with its own C
                 * layout". */
                Type at = emit_resolve_type(ctx, e->type);
                /* No type_has_concrete_codegen_layout conjunct: that switch
                 * has no TY_APP arm and fails closed (the enumerations-drift
                 * pattern); a non-int64 c-name already proves a grounded
                 * concrete monomorph layout exists. */
                if ((at.kind == TY_ADT || at.kind == TY_APP ||
                     at.kind == TY_STRUCT) &&
                    !type_is_heap_struct(at) && !type_is_heap_adt(at) &&
                    strcmp(emit_type_c_name(ctx, at), "int64_t") != 0)
                    return at;
            }
            return unknown;
        }
        case EX_DO:
            return e->as.do_.n > 0
                ? fn_body_tail_byvalue_carrier_type_inner(ctx, e->as.do_.items[e->as.do_.n - 1])
                : unknown;
        case EX_IF: {
            Type t = fn_body_tail_byvalue_carrier_type_inner(ctx, e->as.if_.then_);
            if (t.kind != TY_UNKNOWN) return t;
            return e->as.if_.else_or_null
                ? fn_body_tail_byvalue_carrier_type_inner(ctx, e->as.if_.else_or_null)
                : unknown;
        }
        case EX_MATCH: {
            /* Mirror the bool predicate: the by-value carrier type of the first
             * arm whose tail produces one. */
            for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                Type t = fn_body_tail_byvalue_carrier_type_inner(ctx, e->as.match_.arms[ai].body);
                if (t.kind != TY_UNKNOWN) return t;
            }
            return unknown;
        }
        case EX_LET:
        case EX_LETREC:
            return fn_body_tail_byvalue_carrier_type_inner(ctx, e->as.let_.body);
        default: break;
    }
    const Expr *x = e;
    while (x && x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
    if (!x) return unknown;
    if (x->kind == EX_MAKE_STRUCT && type_uses_carrier_abi(x->type))
        return emit_resolve_type(ctx, x->type);
    if (x->kind == EX_CALL) {
        const EmitAbiSpecialization *spec =
            find_matched_abi_spec(ctx, x, x->as.call_.fn_binding);
        if (spec) {
            Type sr = emit_resolve_type(ctx, spec->result_type);
            if (type_uses_carrier_abi(sr) &&
                strcmp(emit_type_c_name(ctx, sr), "int64_t") != 0)
                return sr;
        }
        /* lens-composition-codegen-blockers (Blocker 2b): a fat-dispatched call
         * through a fn VALUE -- a captured closure / call-head binding, e.g.
         * `((. l1 get) s)` desugared to `(let [ch (. l1 get)] (ch s))` -- whose
         * declared result is a bare tyvar the enclosing spec resolves to a WIDE
         * by-value aggregate.  The call's own e->type was collapsed to the int64
         * carrier at elaboration (elab_call_fn_inner), but the fat thunk / by-value
         * fatshim returns the aggregate BY VALUE, so the materialization temp must
         * be that aggregate, not int64.  Gated to a non-global fn-value callee
         * whose resolved result is a wide by-value ADT, so ordinary carrier calls
         * are untouched. */
        {
            const Binding *fcb = x->as.call_.fn_binding;
            if (fcb && !fcb->is_global && fcb->type.kind == TY_FN &&
                !fcb->closure_fn_binding &&
                (fcb->is_fat || fcb->type.as.fn.boxed) &&
                fcb->type.as.fn.result_full_type) {
                Type rr = emit_resolve_type(ctx, *fcb->type.as.fn.result_full_type);
                if (type_is_wide_byval_adt(rr))
                    return rr;
            }
        }
        /* byvalue-option-if-join-function-call-arm-aggregate-cast: an ordinary
         * top-level defn call whose declared result is a concrete by-value
         * aggregate (`(known)` : `(Option int)`) already yields that aggregate;
         * report its type so a merge temp is declared by value and its arm is not
         * carrier->concrete bridged.  Mirrors the bool predicate's
         * call_ordinary_defn_byval_aggregate. */
        {
            Type oagg = unknown;
            if (call_ordinary_defn_byval_aggregate(ctx, x, &oagg))
                return oagg;
        }
        /* CONV-S1 seam 4 (none-base carrier return): an unrecorded construct
         * template tail (`(none)`/`(some x)`) emits the by-value monomorph
         * aggregate; report that aggregate type so the carrier-return spill boxes
         * it.  Mirrors the bool predicate's call_construct_emits_byval_aggregate. */
        {
            Type cagg = unknown;
            if (call_construct_emits_byval_aggregate(ctx, x, &cagg))
                return cagg;
        }
        /* CONV-S1 seam 4 (spec sibling): a matched by-value spec tail
         * (`some__spec__...Option__int`) reports its concrete aggregate result so
         * the carrier-return spill boxes it -- the existing spec branch above only
         * returns carrier-ABI (heap) specs. */
        {
            Type sagg = unknown;
            if (call_spec_result_byval_aggregate(ctx, x, &sagg))
                return sagg;
        }
        /* CONV-S1 seam 4 (assignment-straddle): an inline-C callee whose declared
         * result is a parametric ADT app (`(Result cstr cstr)`) is lowered by
         * value (a flat aggregate, not the carrier), yet emit_fns.c still emits
         * the inline-C body with an int64 carrier C return.  A control-form merge
         * temp (if/do/let) that sources its value from such a call must be the
         * by-value aggregate so the carrier->concrete deref bridge can populate it
         * -- mirror the make-struct/spec recoveries above.  No abi spec exists for
         * a plain monomorphic inline-C call, so the spec branch misses it.  Inert
         * at default: there the lowered-by-value rep does not apply, so the result
         * type IS the carrier and its c-name is `int64_t` (excluded below). */
        const Binding *cb = x->as.call_.fn_binding;
        if (cb && cb->body_is_inline_c && cb->type.kind == TY_FN &&
            cb->type.as.fn.result_full_type) {
            Type sr = emit_resolve_type(ctx, *cb->type.as.fn.result_full_type);
            /* CONV-S1 seam 4 (inline-C generic-element result): a `: A` inline-C
             * body (e.g. `vec-get`) always returns the int64 carrier regardless
             * of the element type, and at a non-spec call site the bare tyvar `A`
             * has no binding so emit_resolve leaves it abstract.  The TYPER, by
             * contrast, already grounded the CALL's own result type to the
             * concrete element (`(Option int)` here, via the `(:: ... (Option
             * int))` read).  Fall back to that grounded type so a by-value
             * aggregate element read through the int64 carrier gets the
             * carrier->concrete deref bridge.  Gated on the declared result being
             * a bare tyvar, so concrete inline-C results are untouched. */
            bool tyvar_recovered_concrete = false;
            if (cb->type.as.fn.result_full_type->kind == TY_TYVAR &&
                (sr.kind == TY_TYVAR ||
                 strcmp(emit_type_c_name(ctx, sr), "int64_t") == 0)) {
                Type cr = emit_resolve_type(ctx, x->type);
                if (cr.kind == TY_APP || cr.kind == TY_ADT) {
                    sr = cr;
                    tyvar_recovered_concrete = true;
                }
            }
            /* multiword-element-boxing: a WIDE (> 8 byte) by-value ADT element
             * read through a generic `: A` inline-C accessor (`vec-get`, the
             * HAMT val readers) whose declared result_full_type is a BARE TYVAR
             * is EMITTED as the int64 carrier -- a heap-box pointer -- for every
             * A, because a tyvar-result inline-C body is never return-specialized
             * (vec.tur: "Inline-C bodies are not return-specialized").  The
             * concrete element type recovered from x->type (`Point`) nonetheless
             * has a concrete codegen layout, so the default carrier-producer test
             * below (which excludes concrete-layout aggregates -- they normally
             * return BY VALUE) misses it and the field access / method dispatch
             * reads `.x` straight off the int64 carrier (a hard cc error).  When
             * the recovery fired AND the element is a wide by-value ADT, report it
             * as a carrier producer so the carrier->concrete deref-unbox bridge
             * fires.  Gated on the tyvar recovery, so a genuinely by-value
             * concrete accessor result (`Pos`, result_full_type not a tyvar) is
             * untouched.
             *
             * Increment 3 (vec-byvalue-struct-element-invalid-c): the width
             * fork is gone -- a NARROW (<= 8 byte) by-value product element
             * rides the container slot boxed too (there is no working inline
             * form: the read side used to initialize `tur_adt_FzB b = <int64>`
             * straight off the carrier, a hard cc error).  The shared
             * container-element predicate keeps this read-back in lockstep
             * with the push-side boxing and the ownership probes. */
            if (tyvar_recovered_concrete &&
                !type_is_heap_struct(sr) && !type_is_heap_adt(sr) &&
                type_is_boxed_container_elem(sr))
                return sr;
            /* Gate on !type_uses_carrier_abi(sr): the by-value-temp strategy only
             * applies when the lowered aggregate flows by value.  At default the
             * same `(Result cstr cstr)` IS the carrier (type_uses_carrier_abi
             * true), where the existing carrier-temp + single M5 deref already
             * handles the merge -- firing here would declare a by-value temp and
             * double-bridge.  This keeps the extension strictly lowering-only. */
            /* Also require !type_has_concrete_codegen_layout(sr): the inline-C
             * body is EMITTED returning the int64 carrier ONLY when emit_fns'
             * typed_byval_adt does NOT apply -- i.e. a parametric ADT-app result
             * (`(Result cstr cstr)` / `(Option Device)`, no single C layout) falls
             * to the int64 default and needs the carrier->by-value bridge.  A
             * NON-parametric concrete record result (`Pos`) instead returns BY
             * VALUE (typed_byval_adt fires), so it must NOT be treated as a carrier
             * producer here -- otherwise the consume-side bridge double-derefs an
             * aggregate (the typeclass-fundep-collect regression). */
            if ((sr.kind == TY_APP || sr.kind == TY_ADT) &&
                !type_uses_carrier_abi(sr) &&
                !type_is_heap_struct(sr) && !type_is_heap_adt(sr) &&
                !type_has_concrete_codegen_layout(&sr) &&
                strcmp(emit_type_c_name(ctx, sr), "int64_t") != 0)
                return sr;
            /* hkt-inline-c-instance-body-loses-result-type: a DISPATCH call to
             * an inline-C instance method.  Its declared result is the CLASS's
             * applied `(f b)`, which resolves abstractly here (c-name int64_t),
             * so every recovery above misses it -- but the typer has already
             * grounded the CALL's own result to the concrete `(Box int)`.
             *
             * Whether the consumer must bridge is exactly "did the callee return
             * the carrier?", which is the question emit_fns.c answers when it
             * emits the signature.  Ask it through the shared predicate rather
             * than re-deriving it from the type's shape: guessing here reported
             * a carrier for inline-C functions that genuinely return BY VALUE,
             * and the consume-side bridge then deref'd a value that was never a
             * pointer (`aggregate value used where an integer was expected`, on
             * inline-c-struct-return-cstr-params and io-stdlib-roundtrip).
             *
             * The grounded type must itself have a concrete layout -- that is
             * what the consumer will declare -- which is the mirror image of the
             * branch above, where the DECLARED result deliberately has none. */
            if (cb->type.as.fn.result_full_type->kind == TY_APP &&
                !inline_c_returns_byvalue_adt(ctx, true,
                                              cb->type.as.fn.result_full_type)) {
                Type cr = emit_resolve_type(ctx, x->type);
                if (cr.kind == TY_APP &&
                    !type_uses_carrier_abi(cr) &&
                    !type_is_heap_struct(cr) && !type_is_heap_adt(cr) &&
                    type_app_is_concrete_adt(&cr) &&
                    strcmp(emit_type_c_name(ctx, cr), "int64_t") != 0)
                    return cr;
            }
        }
        /* CONV-S1 seam 4 (bounded-wrapper struct return): the body tail is a
         * direct call to a concrete instance method whose by-value aggregate
         * result (`Pos`/`Vel`) the carrier base must spill into its int64
         * return.  See call_emits_byval_concrete_aggregate. */
        Type bv;
        if (call_emits_byval_concrete_aggregate(ctx, x, &bv))
            return bv;
    }
    return unknown;
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

void emit_temp_decl(EmitCtx *ctx, Buf *body, Type type, const char *name, const char *init_or_null) {
    type = emit_resolve_type(ctx, type);
    indent_buf(body, ctx->indent);
    /* CRU B-1: a boxed TY_FN (first-class closure value) is carried as a void *
     * holding the { thunk, env... } box -- declare it via the generic path
     * (emit_type_c_name -> "void *"), not as a thin function pointer, which
     * would mistype the box.  Only a *bare* TY_FN temp is a function pointer. */
    if (type.kind == TY_FN && !type.as.fn.boxed) {
        const char *ret_c = type_c_name(emit_type_from_kind(type.as.fn.result_kind));
        /* Build the `(A0, A1, ...)` parameter list once so it can be reused for
         * both the declarator and the initializer cast. */
        Buf argbuf; buf_init(&argbuf);
        if (type.as.fn.arity == 0) {
            buf_puts(&argbuf, "void");
        } else {
            for (uint32_t i = 0; i < type.as.fn.arity; i++) {
                if (i > 0) buf_puts(&argbuf, ", ");
                buf_puts(&argbuf,
                         type_c_name(emit_type_from_kind(type.as.fn.arg_kinds[i])));
            }
        }
        buf_putc(&argbuf, '\0');
        buf_printf(body, "%s (*%s)(%s)", ret_c, name, argbuf.data);
        if (init_or_null) {
            /* parametric-defstruct-fn-field-gaps (Gap 4): when a fn-typed
             * struct field carries a non-primitive arg/result (a struct or
             * cstr), it is stored as the int64_t carrier rather than a typed
             * `tur_fnptr_..._t`.  Reading it into this typed function-pointer
             * temp would then be an int64_t -> fn-pointer init (a
             * -Wint-conversion warning / hard error under -Werror).  Bridge
             * through (intptr_t) and cast to the exact function-pointer type,
             * matching the typed-field call sites elsewhere. */
            buf_printf(body, " = (%s (*)(%s))(intptr_t)(%s)",
                       ret_c, argbuf.data, init_or_null);
        }
        buf_puts(body, ";\n");
        buf_free(&argbuf);
        return;
    }

    buf_printf(body, "%s %s", emit_type_c_name(ctx, type), name);
    if (init_or_null) buf_printf(body, " = %s", init_or_null);
    buf_puts(body, ";\n");
}

/* option-lowering-mid-migration: a control-form (if/let/do) *result temp*
 * whose static type is a carrier-ABI aggregate (Option/Result) but whose value
 * flows from carrier-int64 producers (some/none/ok/err or an __inst_ method)
 * on every tail path must be declared as the int64 carrier, NOT the by-value
 * `Option__T` / `Result__T__U` struct.
 *
 * The branch/last-expr assignments hand back the bare carrier handle (e.g.
 * `__t = some(x);`), and the downstream consumer -- typically the fn-return
 * bridge in emit_fns.c, which keys off fn_body_tail_is_carrier_producer --
 * unboxes the carrier to the concrete struct.  Declaring the temp by-value
 * here makes `__t = some(x)` an `int64_t`->struct mismatch and the consumer's
 * carrier deref a struct-as-pointer error (the two halves of the
 * mid-migration straddle).  Mirroring the exact return-side predicate keeps
 * the representation consistent end to end.
 *
 * `tail_expr` drives the carrier-producer decision (the control form whose
 * tail leaves are the producers); `type` is the temp's static type. */
/* gcc14-int-conversion (carrier-representation-tracking): true when `s` is a bare
 * C identifier (a temp / local name), so it can key the local-var type table.  A
 * cast/expression value (`(int64_t)(...)`, `x->f`, `f(...)`) is not looked up. */
bool emit_str_is_bare_ident(const char *s) {
    if (!s || !(s[0] == '_' || isalpha((unsigned char)s[0]))) return false;
    for (const char *p = s + 1; *p; p++)
        if (!(*p == '_' || isalnum((unsigned char)*p))) return false;
    return true;
}

/* let-returning-noncapturing-lambda-ices-at-merge-temp: true when a merge temp
 * of type `t` must be declared with the FAT-HANDLE spelling (`void *`, the
 * { thunk, env... } box) rather than a thin `R (*)(A...)` function pointer.
 *
 * emit_temp_decl picks thin-vs-fat off `t.as.fn.boxed`, which is a fact about
 * the TYPE.  Stage-2 tail normalization picks it off
 * fn_result_type_is_fat_normalized, which is a fact about the POSITION -- a fn
 * value reaching a result slot gets boxed into a fat pair whether or not its
 * type carries the flag.  For a captureless lambda in a `let` tail the two
 * disagreed: the tail emitted the fat box, the temp was declared thin, and the
 * assignment between them was `int64_t (*)(int64_t) = (int64_t)(intptr_t)box`
 * -- a -Wint-conversion warning, i.e. a hard error under GCC >= 14, and the
 * repr-shadow ICE that reported it.
 *
 * repr_of is the arbiter (repr-decision-function-plan): ask it in RESULT
 * position and spell the temp to match, rather than re-deriving a second
 * answer here. */
static bool merge_temp_fn_is_fat(EmitCtx *ctx, Type type) {
    Type r = emit_resolve_type(ctx, type);
    if (r.kind != TY_FN) return false;
    return repr_of(&r, REPR_POS_RESULT) == REPR_FAT_HANDLE;
}

static void emit_control_result_temp_decl(EmitCtx *ctx, Buf *body, Type type,
                                          const Expr *tail_expr, const char *name) {
    /* generic-instance-result-byvalue-struct-okarm: when the control form's tail
     * produces a *by-value* concrete carrier-ABI aggregate -- e.g. a generic
     * instance method whose tail re-wraps into `(Result (Option int) cstr)` via
     * specialized `ok`/`err` clones -- declare the temp as that by-value struct,
     * NOT the int64 carrier.  emit_if_value already takes this branch via
     * fn_body_tail_byvalue_carrier_type; a wrapping `let`/`do` whose body is that
     * `if` must agree, or its int64 carrier slot is assigned the by-value struct
     * (`incompatible types ... int64_t ... Result__Option__int__cstr`, a hard cc
     * error).  This walker recovers the type from the matched ABI spec, so it
     * fires even when the construct call's own `e->type` was collapsed to the
     * carrier (where fn_body_tail_emits_byvalue_carrier_abi bails early). */
    Type bv = fn_body_tail_byvalue_carrier_type(ctx, tail_expr);
    if (bv.kind != TY_UNKNOWN) {
        emit_temp_decl(ctx, body, bv, name, NULL);
        /* gcc14-int-conversion (carrier-representation-tracking): record the
         * temp's ACTUAL emitted C type so a later `int64_t z = <this temp>;`
         * binder init can detect the int64<->pointer straddle by the temp's real
         * representation rather than the (colliding) source type c-name. */
        emit_localvar_record_ctype(name, emit_type_c_name(ctx, bv));
        return;
    }
    /* mut-map-reassign seam (chokepoint 1, merge-temp position): a CONCRETE
     * heap container / :heap struct merge temp is its typed pointer, exactly
     * like the let-bind position -- the `^mut` rebinding / control-form path
     * was left behind by the original migration and the R3 shadow ICE'd on
     * it (want=heap-ptr got=carrier-i64).  bridge_control_result_int_ptr
     * reconciles a carrier-emitting tail into the pointer temp, mirroring
     * the binding arms' int64->pointer bridge. */
    {
        const char *hn = emit_repr_concrete_heap_ptr_c_name(
            ctx, type, REPR_POS_RESULT);
        if (hn) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s;\n", hn, name);
            emit_localvar_record_ctype(name, hn);
            return;
        }
    }
    if (type_uses_carrier_abi(emit_resolve_type(ctx, type)) &&
        fn_body_tail_is_carrier_producer(tail_expr) &&
        !fn_body_tail_emits_byvalue_carrier_abi(ctx, tail_expr)) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "int64_t %s;\n", name);
        emit_localvar_record_ctype(name, "int64_t");
        return;
    }
    /* let-returning-noncapturing-lambda-ices-at-merge-temp: a fn value that
     * reaches this slot is normalized to the fat { thunk, env... } box, so the
     * temp is that box's `void *`, not a thin function pointer. */
    if (merge_temp_fn_is_fat(ctx, type)) {
        indent_buf(body, ctx->indent);
        /* Spelled `void * name` to match the generic emit_type_c_name path,
         * which already reaches this same declaration for a type-level boxed
         * fn -- the two must agree byte for byte or snapshots fork. */
        buf_printf(body, "void * %s;\n", name);
        emit_localvar_record_ctype(name, "void *");
        return;
    }
    emit_temp_decl(ctx, body, type, name, NULL);
    emit_localvar_record_ctype(name, emit_type_c_name(ctx, type));
}

/* byvalue-product-tail-var-double-unboxed-nonparametric: true when the EMITTED
 * text `v` is a bare local whose RECORDED C type is already the by-value
 * aggregate `bv` -- so a carrier->concrete bridge over it would deref a value
 * that is not a pointer (`(*(T *)(intptr_t)(<T value>))`, a hard cc error).
 *
 * This is the position-sensitive question the type-level predicates cannot
 * answer. `expr_emits_byvalue_carrier_abi`'s EX_VAR arm was narrowed to
 * PARAMETRIC apps because widening it to non-parametric products regressed ten
 * fixtures: at the vec/map element and assoc-type-return seams a
 * non-parametric product genuinely DOES ride the carrier and needs the bridge,
 * and the type there is identical, so no test on the type can separate the two
 * positions. What separates them is the representation the value actually has
 * HERE, which the localvar side table already records at the declaration.
 *
 * Same shape as the `init_val_recorded_byval_agg` check on the let-binding
 * path; this is the emit_if merge companion, which can only ask it because the
 * arm's emitted text exists by this point. */
static bool emit_arm_is_recorded_byval_agg(EmitCtx *ctx, const char *v,
                                            Type bv) {
    if (!v || bv.kind == TY_UNKNOWN || !emit_str_is_bare_ident(v)) return false;
    const char *lv = emit_localvar_lookup_ctype(v);
    if (!lv) return false;
    const char *want = emit_type_c_name(ctx, bv);
    return want && strcmp(lv, want) == 0 &&
           strcmp(lv, "int64_t") != 0 && strchr(lv, '*') == NULL;
}

/* SR1 companion to emit_arm_is_recorded_byval_agg: the arm is a bare VARIABLE
 * that already holds the by-value aggregate.
 *
 * The recorded-local check above answers this from the local-var side table,
 * which records locals -- a PARAMETER is never in it.  That was sufficient while
 * a by-value ADT could only be a single-variant product: a sum-typed parameter
 * was always the int64 carrier, so a by-value aggregate parameter never reached
 * a control-form merge.  `(defn re-repeat-n [atom : Regex n : int] ...)`
 * returning `atom` from one `if` arm is exactly that case now, and bridging it
 * carrier->concrete derefs a value that is not a pointer.
 *
 * A pass-by-pointer parameter is excluded: it really is held as `const T *`, and
 * the caller's own `then_is_byptr_param` branch derefs it. */
static bool expr_is_pbp_param(EmitCtx *ctx, const Expr *struct_expr);
static bool emit_arm_is_byval_agg_var(EmitCtx *ctx, const Expr *arm, Type bv) {
    if (!g_sr1_sum_byvalue) return false;
    if (!arm || bv.kind == TY_UNKNOWN) return false;
    while (arm->kind == EX_ASCRIBE && arm->as.ascribe_.inner)
        arm = arm->as.ascribe_.inner;
    if (arm->kind != EX_VAR || !arm->as.var.binding) return false;
    if (expr_is_pbp_param(ctx, arm)) return false;
    Type at = emit_resolve_type(ctx, arm->as.var.binding->type);
    if (!emit_type_is_byvalue_adt(ctx, at)) return false;
    char *have = strdup(emit_type_c_name(ctx, at));
    const char *want = emit_type_c_name(ctx, bv);
    bool same = want && strcmp(have, want) == 0;
    free(have);
    return same;
}

/* CONV-S1 seam 4 (assignment-straddle): bridge the value `v` of a control-form
 * tail (`last`) into a by-value merge temp that emit_control_result_temp_decl
 * declared via its branch-1 (fn_body_tail_byvalue_carrier_type) recovery.  When
 * the tail is a carrier producer whose by-value aggregate return is nonetheless
 * EMITTED as the int64 carrier (an inline-C / #{Construct} producer under the
 * defstruct-as-defadt lowering), `emit_value` yields the carrier handle but the
 * temp is the by-value aggregate -- deref it carrier->concrete so the assign
 * type-checks.  emit_if_value applies the same bridge per arm inline; this is the
 * do/let companion.  Inert (returns `v` unchanged) when the temp is not by-value
 * or the tail already emits the by-value aggregate. */
static char *bridge_control_value_to_byvalue_temp(EmitCtx *ctx, Buf *body,
                                                   char *v, const Expr *last) {
    Type bv = fn_body_tail_byvalue_carrier_type(ctx, last);
    if (bv.kind != TY_UNKNOWN &&
        !fn_body_tail_emits_byvalue_carrier_abi(ctx, last))
        return emit_carrier_bridge(ctx, body, v, CK_CARRIER, CK_CONCRETE, bv);
    return v;
}

/* The C type `emit_control_result_temp_decl` declares the result temp with --
 * mirrors that function's three branches exactly so a caller can cast the
 * assigned value to match the temp's declared representation. */
static const char *control_result_temp_ctype(EmitCtx *ctx, Type type,
                                              const Expr *tail) {
    const char *out;
    Type bv = fn_body_tail_byvalue_carrier_type(ctx, tail);
    const char *hn = (bv.kind == TY_UNKNOWN)
        ? emit_repr_concrete_heap_ptr_c_name(ctx, type, REPR_POS_RESULT)
        : NULL;
    if (bv.kind != TY_UNKNOWN)
        out = emit_type_c_name(ctx, bv);
    else if (hn)
        /* mut-map-reassign seam: mirror emit_control_result_temp_decl's
         * concrete-heap branch exactly (chokepoint 1, merge-temp position). */
        out = hn;
    else if (type_uses_carrier_abi(emit_resolve_type(ctx, type)) &&
             fn_body_tail_is_carrier_producer(tail) &&
             !fn_body_tail_emits_byvalue_carrier_abi(ctx, tail))
        out = "int64_t";
    else if (merge_temp_fn_is_fat(ctx, type))
        /* Mirror emit_control_result_temp_decl's fat-fn branch exactly. */
        out = "void *";
    else
        out = emit_type_c_name(ctx, type);
    /* Shadow against the RECOVERED type when the walker found one: the
     * declared `type` may have been collapsed to a scalar at elab (the
     * return-only-poly accessor shape), and shadowing the collapsed type
     * reports the site's correct recovery as a false disagreement. */
    repr_shadow_check(ctx, "merge-temp", REPR_POS_RESULT,
                      bv.kind != TY_UNKNOWN ? bv : type, out);
    return out;
}

/* gcc14-int-conversion (carrier-to-typed-param): reconcile a control-form result
 * assignment `tmp = <value>` whose temp C type and value representation straddle
 * the int64<->pointer boundary.  The temp is declared by
 * emit_control_result_temp_decl (int64 carrier or a concrete `tur_adt_X *`); the
 * value flows from the body's tail, which may emit the other representation (a
 * carrier producer into a concrete-pointer temp, or a pointer value into an int64
 * temp).  `tmp = <ptr>`/`tmp = <int64>` mismatched is -Wint-conversion, a hard
 * error under GCC >= 14.  Bridge through intptr_t (value-preserving); leave a
 * matching pair, a `void *` temp, or a by-value aggregate untouched. */
static char *bridge_control_result_int_ptr(EmitCtx *ctx, char *v, Type ltype,
                                            const Expr *tail) {
    const char *tcty = control_result_temp_ctype(ctx, ltype, tail);
    if (!tcty) return v;
    size_t tL = strlen(tcty);
    bool temp_is_i64 = strcmp(tcty, "int64_t") == 0;
    bool temp_is_ptr = tL >= 1 && tcty[tL - 1] == '*' && strcmp(tcty, "void *") != 0;
    /* gcc14-int-conversion (carrier-representation-tracking): a `void *` temp
     * assigned an int64 carrier value (a closure carrier declared int64) is
     * `pointer from integer` under GCC >= 14; bridge it too.  A `void *` temp
     * assigned a POINTER value stays untouched (val_is_ptr, no branch fires). */
    bool temp_is_voidptr = strcmp(tcty, "void *") == 0;
    if (!temp_is_i64 && !temp_is_ptr && !temp_is_voidptr) return v;
    /* the value's own representation C type */
    const Expr *p = tail;
    while (p && p->kind == EX_ASCRIBE) p = p->as.ascribe_.inner;
    const char *vcty = p ? emit_binding_repr_c_name(ctx, p->type, p) : NULL;
    /* A bare local's RECORDED emitted C type is ground truth -- the source-type
     * c-name collides under the carrier duality (a closure carrier declared int64
     * whose fn type c-names to a pointer, which would hide the int64->void*
     * straddle). */
    if (emit_str_is_bare_ident(v)) {
        const char *lvty = emit_localvar_lookup_ctype(v);
        if (lvty) vcty = lvty;
    }
    if (!vcty) return v;
    size_t vL = strlen(vcty);
    bool val_is_i64 = strcmp(vcty, "int64_t") == 0;
    bool val_is_ptr = vL >= 1 && vcty[vL - 1] == '*';
    if (temp_is_i64 && val_is_ptr) {
        Buf b; buf_init(&b);
        buf_printf(&b, "(int64_t)(intptr_t)(%s)", v);
        buf_putc(&b, '\0'); free(v); v = strdup(b.data); buf_free(&b);
    } else if ((temp_is_ptr || temp_is_voidptr) && val_is_i64) {
        Buf b; buf_init(&b);
        buf_printf(&b, "(%s)(intptr_t)(%s)", tcty, v);
        buf_putc(&b, '\0'); free(v); v = strdup(b.data); buf_free(&b);
    }
    return v;
}

/* Fat-closure-env scoped free (docs/archive/history/fat-closure-env-leak.md): decide
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
    const Expr *pinit = init;
    while (pinit && pinit->kind == EX_ASCRIBE) pinit = pinit->as.ascribe_.inner;
    /* closure-drop-glue S1c: a call to a fresh-closure-returning fn (the
     * make-scaler shape) yields a freshly-malloc'd, uniquely-owned env.  The
     * scalar-capture / scalar-result safety was already checked when
     * `returns_fresh_closure` was inferred, so accept it here without the
     * TY_FN/scalar-result gate (the binding's own type is the env's ptr<void>
     * carrier, not a fn type). */
    bool fresh_call = pinit && pinit->kind == EX_CALL
                      && pinit->as.call_.fn_binding
                      && pinit->as.call_.fn_binding->returns_fresh_closure;
    /* closure-drop-glue (Model R): a PARTIAL-APPLICATION head -- e.g. the
     * `(mw-cors-opts opts)` in `((mw-cors-opts opts) base)` -- desugars to
     * `(let [<pre-applied args>] <EX_CLOSURE>)`: a freshly-malloc'd `__pap` env
     * that is applied once as the call head and then dead.  Its thunk returns the
     * UNDERLYING fn's result, never its own env, so freeing it at scope exit can
     * never alias-UAF the result.  Admit it (the escape checks below still guard
     * a __pap that leaked into a sibling/result).  Gated on the experiment. */
    bool fresh_pap = false;
    {
        const Expr *p = pinit;
        while (p && (p->kind == EX_ASCRIBE || p->kind == EX_LET))
            p = (p->kind == EX_ASCRIBE) ? p->as.ascribe_.inner : p->as.let_.body;
        fresh_pap = p && p->kind == EX_CLOSURE && init->kind == EX_LET;
    }
    if (init->kind != EX_CLOSURE && !fresh_call && !fresh_pap) return false;
    if (init->kind == EX_CLOSURE) {
        /* Scalar-result gate: a closure returning a reference/struct/pointer could
         * hand back a value derived from its env; restrict to scalar returns whose
         * result is copied out by value. */
        if (b->type.kind != TY_FN) return false;
        switch (b->type.as.fn.result_kind) {
            case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_NIL: break;
            default: return false;
        }
    }
    if (closure_binding_escapes(e->as.let_.body, b)) return false;
    for (uint32_t j = 0; j < e->as.let_.n; j++) {
        if (j == idx) continue;
        if (closure_binding_escapes(e->as.let_.bindings[j].init, b)) return false;
    }
    return true;
}

/* catch-unwind-thunk-closure-leak (Part 2): decide whether let-binding `idx`
 * holds a caught Result box (`(catch-unwind ...)` / `(catch-panic-of ...)`)
 * whose box can be `tur_result_box_free`d when the let scope exits.  Sound iff:
 *   - the initializer is an EX_CATCH_UNWIND / EX_CATCH_PANIC_OF (so the value is
 *     a box THIS scope owns), and
 *   - the bound name does not escape -- it is used only as a read-only
 *     ok?/err?/ok-val argument in the body and never escapes through a sibling
 *     binding's initializer.
 * Like let_binding_env_freeable, the conservative escape check only ever
 * greenlights a free, so a false negative merely preserves the status-quo leak;
 * it never frees a still-live box. */
static bool let_binding_box_freeable(const Expr *e, uint32_t idx) {
    const Expr *init = e->as.let_.bindings[idx].init;
    const Binding *b = e->as.let_.bindings[idx].binding;
    if (!init || !b) return false;
    if (init->kind != EX_CATCH_UNWIND && init->kind != EX_CATCH_PANIC_OF)
        return false;
    /* catch-unwind-panic-payload-leaks (Leak 2): the body may read the box only
     * through the scalar-accessor whitelist (catch_box_binding_escapes), OR
     * through a reader whose result is confined to the scope
     * (catch_box_binding_reader_confined -- e.g. the fixture's
     * `(println (panic-msg r))`, where panic-msg hands back a pointer into the
     * box-owned message that println consumes before the scope-exit free). */
    if (catch_box_binding_escapes(e->as.let_.body, b) &&
        !catch_box_binding_reader_confined(e->as.let_.body, b, e->type.kind))
        return false;
    for (uint32_t j = 0; j < e->as.let_.n; j++) {
        if (j == idx) continue;
        if (catch_box_binding_escapes(e->as.let_.bindings[j].init, b)) return false;
    }
    return true;
}

static bool expr_is_pbp_param(EmitCtx *ctx, const Expr *struct_expr);

static char *emit_let_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Phase 3/4: Check if body contains return or throw first */
    bool body_has_return_or_throw = expr_contains_return_or_throw(e->as.let_.body);
    
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    /* Phase R1: Also create temp if body has return/throw but let has non-nil type */
    if (!nil_result && !body_has_return_or_throw) {
        tmp = fresh_tmp(ctx);
        emit_control_result_temp_decl(ctx, body, e->type, e, tmp);
    } else if (!nil_result && body_has_return_or_throw) {
        /* Special case for ? operator: body may contain return but still produce a value */
        tmp = fresh_tmp(ctx);
        emit_control_result_temp_decl(ctx, body, e->type, e, tmp);
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
    /* catch-unwind-thunk-closure-leak (Part 2): C names of let-bound,
     * non-escaping caught Result boxes to tur_result_box_free at scope exit. */
    char **box_free_names = NULL;
    uint32_t n_box_free = 0;
    /* local-struct-drop (fn-field): C names + struct C types of let-bound owning
     * by-value struct locals the elaborator flagged `drops_fn_fields` -- their
     * boxed fn-field handles are freed via `drop_fnfields_<T>(&name)` at scope
     * exit (rc/ref fields are handled by the elaborator's injected defers). */
    char **fnfld_names = NULL;
    char **fnfld_types = NULL;
    uint32_t n_fnfld = 0;
    if (!body_has_return_or_throw) {
        for (uint32_t i = 0; i < e->as.let_.n; i++) {
            if (let_binding_env_freeable(e, i)) {
                env_free_names = (char **)realloc(env_free_names,
                                                  (n_env_free + 1) * sizeof(char *));
                env_free_names[n_env_free++] =
                    name_for_binding(ctx, e->as.let_.bindings[i].binding);
            } else if (let_binding_box_freeable(e, i)) {
                box_free_names = (char **)realloc(box_free_names,
                                                  (n_box_free + 1) * sizeof(char *));
                box_free_names[n_box_free++] =
                    name_for_binding(ctx, e->as.let_.bindings[i].binding);
            } else {
                const Binding *sb = e->as.let_.bindings[i].binding;
                if (!sb || !sb->drops_fn_fields || sb->type.kind != TY_ADT ||
                    !sb->type.as.adt_.def)
                    continue;
                char *mn = mangle_field_name(sb->type.as.adt_.def->name);
                size_t tl = strlen(mn) + 16;
                char *tn = (char *)malloc(tl);
                snprintf(tn, tl, "tur_adt_%s", mn);
                free(mn);
                fnfld_names = (char **)realloc(fnfld_names,
                                               (n_fnfld + 1) * sizeof(char *));
                fnfld_types = (char **)realloc(fnfld_types,
                                               (n_fnfld + 1) * sizeof(char *));
                fnfld_names[n_fnfld] = name_for_binding(ctx, sb);
                fnfld_types[n_fnfld] = tn;
                n_fnfld++;
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
            /* gcc14-int-conversion (carrier-representation-tracking): this boxed/
             * fat closure binder is the int64 carrier; record it so a downstream
             * straddle site (a `void *` letrec-result temp assigned this closure
             * carrier) resolves the real int64 representation instead of the fn
             * type's colliding pointer c-name. */
            emit_localvar_record_ctype(bn, "int64_t");
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
            emit_localvar_record_ctype(bn, "int64_t");
        } else if (b->type.kind == TY_FN) {
            /* For function pointer types, emit: <result> (*<name>)(<args...>) = <init>; */
            const char *ret_c = type_c_name(emit_type_from_kind(b->type.as.fn.result_kind));
            Buf argbuf; buf_init(&argbuf);
            for (uint32_t j = 0; j < b->type.as.fn.arity; j++) {
                if (j > 0) buf_puts(&argbuf, ", ");
                buf_puts(&argbuf,
                         type_c_name(emit_type_from_kind(b->type.as.fn.arg_kinds[j])));
            }
            buf_putc(&argbuf, '\0');
            /* parametric-defstruct-fn-field-gaps (Gap 4): the initializer may be
             * the int64_t carrier (a fn-typed struct field whose non-primitive
             * arg/result kept it off the typed `tur_fnptr_..._t` path).  Bridge
             * through (intptr_t) and cast to the exact function-pointer type so
             * the assignment is not an int64_t -> fn-pointer init
             * (-Wint-conversion / hard error under -Werror). */
            buf_printf(body, "%s (*%s)(%s) = (%s (*)(%s))(intptr_t)(%s);\n",
                       ret_c, bn, argbuf.data, ret_c, argbuf.data, iv);
            buf_free(&argbuf);
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
            /* Shadow only bindings whose DECLARED type is concrete: a
             * tyvar-declared binding inside a generic body keeps the erased
             * spelling by design even when the active spec resolves it (the
             * shadow resolves through the spec, so it would misread the
             * erasure as a disagreement -- the Line-in-lens rows of the
             * third sweep). */
            if (!emit_repr_type_mentions_tyvar(&b->type))
                repr_shadow_check(ctx, "binding", REPR_POS_LET_BIND, b->type,
                                  bind_c);
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
            /* vec-push-heap-struct-element-not-carrier-cast (read side): a
             * carrier (int64_t) binding whose initialiser emits a POINTER-
             * represented value -- e.g. `(let [c (:: x (Cons A))] ...)` inside a
             * specialized instance body, where the spec receiver `x` lowers to a
             * `Cons__Option__int *` heap pointer -- needs the pointer->carrier
             * reinterpret, else `int64_t c = x` trips -Wint-conversion.  A
             * pointer->intptr_t->int64_t cast is always valid and is a no-op for
             * a value that is already the int64 carrier (whose declared
             * carrier-ABI type may still c-name to a pointer), so this only
             * tightens codegen; a by-value aggregate init c-names without a `*`
             * and is left to the by-value binding declaration above. */
            /* The ascription node's own type is erased to the int64 carrier, so
             * peek through ascriptions to an inner spec param and resolve its
             * concrete type via the active ABI spec -- otherwise `(:: x (Cons A))`
             * resolves only to the abstract carrier (c-name int64_t) and the
             * pointer-repr check below misses it. */
            Type init_ty_r = emit_resolve_type(ctx, e->as.let_.bindings[i].init->type);
            {
                const Expr *iexpr = e->as.let_.bindings[i].init;
                while (iexpr && iexpr->kind == EX_ASCRIBE) iexpr = iexpr->as.ascribe_.inner;
                Type spec_ty;
                if (iexpr && emit_var_spec_arg_type(ctx, iexpr, &spec_ty))
                    init_ty_r = spec_ty;
            }
            const char *init_cn = emit_type_c_name(ctx, init_ty_r);
            bool init_is_ptr_repr = init_cn && strchr(init_cn, '*') != NULL;
            /* gcc14-int-conversion (carrier-representation-tracking, reverse
             * straddle): the init VALUE is a bare temp whose RECORDED emitted C
             * type is a concrete pointer, while the binder is the int64 carrier
             * (`int64_t z = __t169;` where `__t169` was declared
             * `tur_adt_Cons__Option__int *`).  The init's TYPE c-names to the
             * carrier (init_is_ptr_repr is false), so a type-based check
             * under-fires; keying on the type broadly over-fires (139-fixture
             * churn).  The local-var side table records the temp's ACTUAL emitted
             * representation, so the bridge fires only for a genuine pointer temp
             * flowing into an int64 binder. Value-preserving. */
            bool init_val_recorded_ptr = false;
            /* Whether the init value's RECORDED emitted C type is exactly `void *`
             * (an `__auto_type __ps_N` temp holding a `void *`-returning call) or
             * the `int64_t` carrier.  The concrete-pointer flag above excludes
             * `void *` (its consumers gate on `!= "void *"`), so these two carry the
             * void*<->int64 straddle directions the concrete-pointer bridge does
             * not: a `void *` temp flowing into an `int64_t` binder, and an
             * `int64_t` temp flowing into a pointer binder. */
            bool init_val_recorded_voidp = false;
            bool init_val_recorded_i64 = false;
            if (emit_str_is_bare_ident(iv)) {
                const char *lvty = emit_localvar_lookup_ctype(iv);
                size_t lL = lvty ? strlen(lvty) : 0;
                if (strcmp(bind_c, "int64_t") == 0)
                    init_val_recorded_ptr = lvty && lL >= 1 && lvty[lL - 1] == '*' &&
                                            strcmp(lvty, "void *") != 0;
                init_val_recorded_voidp = lvty && strcmp(lvty, "void *") == 0;
                init_val_recorded_i64 = lvty && strcmp(lvty, "int64_t") == 0;
            }
            /* gcc14-int-conversion (carrier-representation-tracking): the init
             * VALUE is a `void *` union-default read (`((union { int64_t s; void *
             * d; }){.s = ..}).d`, emitted for a `(:: <int> :ptr<void>)` carrier
             * relabel) while the binder is the int64 carrier -- e.g.
             * `(let [c (:: (:: 0 :ptr<void>) (SChan ...))] ...)`.  `int64_t c =
             * <void *>` is `integer from pointer` -- a hard error under GCC >= 14.
             * Detect the exact void*-member union read (unique to this emit; it
             * cannot match an int64 value) and reinterpret it to the carrier. */
            if (!init_val_recorded_ptr && strcmp(bind_c, "int64_t") == 0 && iv) {
                size_t ivL = strlen(iv);
                if (ivL >= 4 && strcmp(iv + ivL - 4, "}).d") == 0 &&
                    strstr(iv, "void * d;") != NULL)
                    init_val_recorded_ptr = true;
            }
            /* let-bind-passbyptr-struct-param-invalid-initializer: a struct
             * parameter whose fields sum to > 16 bytes arrives via the
             * by-pointer ABI (`const T *`), but the let binding is declared
             * by value (`bind_c` is the bare struct, no `*`).  A direct
             * `T g = t;` initialiser is then a pointer->struct mismatch that
             * `cc` rejects.  Dereference the pbp param (`T g = *t;`) so the
             * by-value local is initialised from the pointed-to struct -- the
             * same receiver handling EX_GET_FIELD already applies via
             * `expr_is_pbp_param`.  A pointer-represented binding (carrier /
             * :heap, `bind_c` contains `*`) keeps the bare alias. */
            bool bind_is_ptr_repr = strchr(bind_c, '*') != NULL;
            bool init_is_pbp = expr_is_pbp_param(ctx,
                                                 e->as.let_.bindings[i].init);
            /* CONV-S1 seam 4 (inline-C carrier init -> by-value binding): the
             * initializer is a call to an inline-C function whose by-value ADT-app
             * result (`(Result Device int)` under lowering) is nonetheless EMITTED
             * as the int64 carrier (emit_fns lowers an inline-C TY_APP result to
             * int64), but the binding is the by-value aggregate.  Deref the carrier
             * into the aggregate so the initialiser type-checks -- the consume-side
             * companion of the assignment-straddle merge bridge.  Gated on the
             * by-value-vs-emit disagreement, so inert when the init already yields
             * the aggregate. */
            Type init_bv = fn_body_tail_byvalue_carrier_type(
                ctx, e->as.let_.bindings[i].init);
            /* Increment 3 (double-deref guard): when the init is a control
             * form (a `let`/`do` -- e.g. the map-get macro's expansion) whose
             * merge temp was ALREADY declared by-value and bridged by
             * bridge_control_value_to_byvalue_temp, the value in hand IS the
             * aggregate -- re-deriving "carrier producer" from the tail call
             * here would deref it a second time (`(*(T *)(intptr_t)(<T
             * value>))`, a hard cc error).  The temp's ACTUAL emitted C type
             * is in the localvar side table (the init_val_recorded_* pattern
             * above); a recorded by-value aggregate type equal to the
             * binding's own suppresses the re-bridge -- consult the recorded
             * representation instead of re-deciding from the tail. */
            bool init_val_recorded_byval_agg = false;
            if (emit_str_is_bare_ident(iv)) {
                const char *lvty2 = emit_localvar_lookup_ctype(iv);
                init_val_recorded_byval_agg =
                    lvty2 && strcmp(lvty2, bind_c) == 0 &&
                    strcmp(lvty2, "int64_t") != 0 &&
                    strchr(lvty2, '*') == NULL;
            }
            bool init_carrier_to_byval = !bind_is_ptr_repr &&
                strcmp(bind_c, "int64_t") != 0 &&
                init_bv.kind != TY_UNKNOWN &&
                !init_val_recorded_byval_agg &&
                !fn_body_tail_emits_byvalue_carrier_abi(
                    ctx, e->as.let_.bindings[i].init);
            if (init_is_pbp && !bind_is_ptr_repr &&
                strcmp(bind_c, "int64_t") != 0) {
                buf_printf(body, "%s %s = *(%s);\n", bind_c, bn, iv);
            } else if (init_carrier_to_byval) {
                char *bridged = emit_carrier_bridge(ctx, body, iv,
                                    CK_CARRIER, CK_CONCRETE, init_bv);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = %s;\n", bind_c, bn, bridged);
                iv = bridged;  /* emit_carrier_bridge freed the old iv */
            } else if (strcmp(bind_c, "int64_t") == 0 &&
                (init_kind == TY_FN || init_kind == TY_PTR_VOID ||
                 init_is_ptr_repr || init_val_recorded_ptr ||
                 init_val_recorded_voidp)) {
                buf_printf(body, "%s %s = (int64_t)(intptr_t)(%s);\n", bind_c, bn, iv);
            } else if (bind_is_ptr_repr &&
                       ((init_cn && strcmp(init_cn, "int64_t") == 0) ||
                        init_val_recorded_i64)) {
                buf_printf(body, "%s %s = (%s)(intptr_t)(%s);\n", bind_c, bn, bind_c, iv);
            } else if (bind_is_ptr_repr && iv &&
                       strncmp(iv, "(int64_t)", 9) == 0) {
                /* gcc14-int-conversion (carrier-representation-tracking, Class B):
                 * the binder's declared C type is a concrete pointer, but the init
                 * VALUE is emitted as the int64 carrier -- e.g.
                 * `(:: (.tail xs) (Cons (Option int)))` emits `(int64_t)(...)->tail`
                 * while `t0` is declared `tur_adt_Cons__Option__int *`.  `init_cn`
                 * (the init's TYPE c-name) is the pointer here, so the branch above
                 * under-fires; key on the emitted value being the carrier (its
                 * `(int64_t)` prefix) and reinterpret it to the binder's pointer.
                 * Value-preserving (int64 -> intptr_t -> pointer), and only fires
                 * for a pointer binder fed an explicitly int64-cast value. */
                buf_printf(body, "%s %s = (%s)(intptr_t)(%s);\n", bind_c, bn, bind_c, iv);
            } else {
                buf_printf(body, "%s %s = %s;\n", bind_c, bn, iv);
            }
            /* gcc14-int-conversion (carrier-representation-tracking): record the
             * binder's ACTUAL declared C type so a downstream straddle site (a
             * control-result assignment reading this var) resolves the real
             * representation, not the colliding source-type c-name -- e.g. a
             * self-capturing closure carrier declared `int64_t self` whose fn type
             * c-names to a pointer, feeding a `void *` letrec-result temp. */
            emit_localvar_record_ctype(bn, bind_c);
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
            bv = bridge_control_value_to_byvalue_temp(ctx, body, bv,
                                                      e->as.let_.body);
            bv = bridge_control_result_int_ptr(ctx, bv, e->type, e->as.let_.body);
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
        bv = bridge_control_value_to_byvalue_temp(ctx, body, bv,
                                                  e->as.let_.body);
        bv = bridge_control_result_int_ptr(ctx, bv, e->type, e->as.let_.body);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", tmp, bv);
        free(bv);
    }

    /* Fat-closure-env scoped free: release non-escaping closure envs now that
     * the let body (their last use) has been emitted. */
    for (uint32_t i = 0; i < n_env_free; i++) {
        indent_buf(body, ctx->indent);
        /* closure-drop-glue (Model R): a headered env must be released through
         * its drop-glue header (which also walks owning captures), not a bare
         * `free` of the past-header pointer.  Flag-off, TUR_CLOSURE_DROP is a
         * env's drop-glue header, which also walks owning captures. */
        buf_printf(body, "TUR_CLOSURE_DROP(%s);\n", env_free_names[i]);
        free(env_free_names[i]);
    }
    free(env_free_names);

    /* catch-unwind-thunk-closure-leak (Part 2): release non-escaping caught
     * Result boxes (and, for an err box, its panic payload) at scope exit -- the
     * body was their last use.  tur_result_box_free deliberately does not free an
     * ok box's ok_val, which ok-val may have copied out and let escape. */
    for (uint32_t i = 0; i < n_box_free; i++) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "tur_result_box_free((int64_t)(intptr_t)%s);\n",
                   box_free_names[i]);
        free(box_free_names[i]);
    }
    free(box_free_names);

    /* local-struct-drop (fn-field): free the boxed fn-field handles of flagged
     * non-escaping by-value struct locals now that the body (their last use) has
     * been emitted.  Fields only -- the struct is stack-resident, so
     * drop_fnfields_<T> must not free `&name`. */
    for (uint32_t i = 0; i < n_fnfld; i++) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "drop_fnfields_%s((void *)&%s);\n",
                   fnfld_types[i], fnfld_names[i]);
        free(fnfld_names[i]);
        free(fnfld_types[i]);
    }
    free(fnfld_names);
    free(fnfld_types);

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
        emit_control_result_temp_decl(ctx, body, e->type, e, tmp);
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
            for (uint32_t j = 0; j < b->type.as.fn.arity; j++) {
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
            /* Shadow only bindings whose DECLARED type is concrete: a
             * tyvar-declared binding inside a generic body keeps the erased
             * spelling by design even when the active spec resolves it (the
             * shadow resolves through the spec, so it would misread the
             * erasure as a disagreement -- the Line-in-lens rows of the
             * third sweep). */
            if (!emit_repr_type_mentions_tyvar(&b->type))
                repr_shadow_check(ctx, "binding", REPR_POS_LET_BIND, b->type,
                                  bind_c);
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
            /* vec-push-heap-struct-element-not-carrier-cast (read side): a
             * carrier (int64_t) binding whose initialiser emits a POINTER-
             * represented value -- e.g. `(let [c (:: x (Cons A))] ...)` inside a
             * specialized instance body, where the spec receiver `x` lowers to a
             * `Cons__Option__int *` heap pointer -- needs the pointer->carrier
             * reinterpret, else `int64_t c = x` trips -Wint-conversion.  A
             * pointer->intptr_t->int64_t cast is always valid and is a no-op for
             * a value that is already the int64 carrier (whose declared
             * carrier-ABI type may still c-name to a pointer), so this only
             * tightens codegen; a by-value aggregate init c-names without a `*`
             * and is left to the by-value binding declaration above. */
            /* The ascription node's own type is erased to the int64 carrier, so
             * peek through ascriptions to an inner spec param and resolve its
             * concrete type via the active ABI spec -- otherwise `(:: x (Cons A))`
             * resolves only to the abstract carrier (c-name int64_t) and the
             * pointer-repr check below misses it. */
            Type init_ty_r = emit_resolve_type(ctx, e->as.let_.bindings[i].init->type);
            {
                const Expr *iexpr = e->as.let_.bindings[i].init;
                while (iexpr && iexpr->kind == EX_ASCRIBE) iexpr = iexpr->as.ascribe_.inner;
                Type spec_ty;
                if (iexpr && emit_var_spec_arg_type(ctx, iexpr, &spec_ty))
                    init_ty_r = spec_ty;
            }
            const char *init_cn = emit_type_c_name(ctx, init_ty_r);
            bool init_is_ptr_repr = init_cn && strchr(init_cn, '*') != NULL;
            /* gcc14-int-conversion (carrier-representation-tracking, reverse
             * straddle): the init VALUE is a bare temp whose RECORDED emitted C
             * type is a concrete pointer, while the binder is the int64 carrier
             * (`int64_t z = __t169;` where `__t169` was declared
             * `tur_adt_Cons__Option__int *`).  The init's TYPE c-names to the
             * carrier (init_is_ptr_repr is false), so a type-based check
             * under-fires; keying on the type broadly over-fires (139-fixture
             * churn).  The local-var side table records the temp's ACTUAL emitted
             * representation, so the bridge fires only for a genuine pointer temp
             * flowing into an int64 binder. Value-preserving. */
            bool init_val_recorded_ptr = false;
            /* Whether the init value's RECORDED emitted C type is exactly `void *`
             * (an `__auto_type __ps_N` temp holding a `void *`-returning call) or
             * the `int64_t` carrier.  The concrete-pointer flag above excludes
             * `void *` (its consumers gate on `!= "void *"`), so these two carry the
             * void*<->int64 straddle directions the concrete-pointer bridge does
             * not: a `void *` temp flowing into an `int64_t` binder, and an
             * `int64_t` temp flowing into a pointer binder. */
            bool init_val_recorded_voidp = false;
            bool init_val_recorded_i64 = false;
            if (emit_str_is_bare_ident(iv)) {
                const char *lvty = emit_localvar_lookup_ctype(iv);
                size_t lL = lvty ? strlen(lvty) : 0;
                if (strcmp(bind_c, "int64_t") == 0)
                    init_val_recorded_ptr = lvty && lL >= 1 && lvty[lL - 1] == '*' &&
                                            strcmp(lvty, "void *") != 0;
                init_val_recorded_voidp = lvty && strcmp(lvty, "void *") == 0;
                init_val_recorded_i64 = lvty && strcmp(lvty, "int64_t") == 0;
            }
            /* gcc14-int-conversion (carrier-representation-tracking): the init
             * VALUE is a `void *` union-default read (`((union { int64_t s; void *
             * d; }){.s = ..}).d`, emitted for a `(:: <int> :ptr<void>)` carrier
             * relabel) while the binder is the int64 carrier -- e.g.
             * `(let [c (:: (:: 0 :ptr<void>) (SChan ...))] ...)`.  `int64_t c =
             * <void *>` is `integer from pointer` -- a hard error under GCC >= 14.
             * Detect the exact void*-member union read (unique to this emit; it
             * cannot match an int64 value) and reinterpret it to the carrier. */
            if (!init_val_recorded_ptr && strcmp(bind_c, "int64_t") == 0 && iv) {
                size_t ivL = strlen(iv);
                if (ivL >= 4 && strcmp(iv + ivL - 4, "}).d") == 0 &&
                    strstr(iv, "void * d;") != NULL)
                    init_val_recorded_ptr = true;
            }
            /* let-bind-passbyptr-struct-param-invalid-initializer: a struct
             * parameter whose fields sum to > 16 bytes arrives via the
             * by-pointer ABI (`const T *`), but the let binding is declared
             * by value (`bind_c` is the bare struct, no `*`).  A direct
             * `T g = t;` initialiser is then a pointer->struct mismatch that
             * `cc` rejects.  Dereference the pbp param (`T g = *t;`) so the
             * by-value local is initialised from the pointed-to struct -- the
             * same receiver handling EX_GET_FIELD already applies via
             * `expr_is_pbp_param`.  A pointer-represented binding (carrier /
             * :heap, `bind_c` contains `*`) keeps the bare alias. */
            bool bind_is_ptr_repr = strchr(bind_c, '*') != NULL;
            bool init_is_pbp = expr_is_pbp_param(ctx,
                                                 e->as.let_.bindings[i].init);
            /* CONV-S1 seam 4 (inline-C carrier init -> by-value binding): the
             * initializer is a call to an inline-C function whose by-value ADT-app
             * result (`(Result Device int)` under lowering) is nonetheless EMITTED
             * as the int64 carrier (emit_fns lowers an inline-C TY_APP result to
             * int64), but the binding is the by-value aggregate.  Deref the carrier
             * into the aggregate so the initialiser type-checks -- the consume-side
             * companion of the assignment-straddle merge bridge.  Gated on the
             * by-value-vs-emit disagreement, so inert when the init already yields
             * the aggregate. */
            Type init_bv = fn_body_tail_byvalue_carrier_type(
                ctx, e->as.let_.bindings[i].init);
            /* Increment 3 (double-deref guard): when the init is a control
             * form (a `let`/`do` -- e.g. the map-get macro's expansion) whose
             * merge temp was ALREADY declared by-value and bridged by
             * bridge_control_value_to_byvalue_temp, the value in hand IS the
             * aggregate -- re-deriving "carrier producer" from the tail call
             * here would deref it a second time (`(*(T *)(intptr_t)(<T
             * value>))`, a hard cc error).  The temp's ACTUAL emitted C type
             * is in the localvar side table (the init_val_recorded_* pattern
             * above); a recorded by-value aggregate type equal to the
             * binding's own suppresses the re-bridge -- consult the recorded
             * representation instead of re-deciding from the tail. */
            bool init_val_recorded_byval_agg = false;
            if (emit_str_is_bare_ident(iv)) {
                const char *lvty2 = emit_localvar_lookup_ctype(iv);
                init_val_recorded_byval_agg =
                    lvty2 && strcmp(lvty2, bind_c) == 0 &&
                    strcmp(lvty2, "int64_t") != 0 &&
                    strchr(lvty2, '*') == NULL;
            }
            bool init_carrier_to_byval = !bind_is_ptr_repr &&
                strcmp(bind_c, "int64_t") != 0 &&
                init_bv.kind != TY_UNKNOWN &&
                !init_val_recorded_byval_agg &&
                !fn_body_tail_emits_byvalue_carrier_abi(
                    ctx, e->as.let_.bindings[i].init);
            if (init_is_pbp && !bind_is_ptr_repr &&
                strcmp(bind_c, "int64_t") != 0) {
                buf_printf(body, "%s %s = *(%s);\n", bind_c, bn, iv);
            } else if (init_carrier_to_byval) {
                char *bridged = emit_carrier_bridge(ctx, body, iv,
                                    CK_CARRIER, CK_CONCRETE, init_bv);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = %s;\n", bind_c, bn, bridged);
                iv = bridged;  /* emit_carrier_bridge freed the old iv */
            } else if (strcmp(bind_c, "int64_t") == 0 &&
                (init_kind == TY_FN || init_kind == TY_PTR_VOID ||
                 init_is_ptr_repr || init_val_recorded_ptr ||
                 init_val_recorded_voidp)) {
                buf_printf(body, "%s %s = (int64_t)(intptr_t)(%s);\n", bind_c, bn, iv);
            } else if (bind_is_ptr_repr &&
                       ((init_cn && strcmp(init_cn, "int64_t") == 0) ||
                        init_val_recorded_i64)) {
                buf_printf(body, "%s %s = (%s)(intptr_t)(%s);\n", bind_c, bn, bind_c, iv);
            } else if (bind_is_ptr_repr && iv &&
                       strncmp(iv, "(int64_t)", 9) == 0) {
                /* gcc14-int-conversion (carrier-representation-tracking, Class B):
                 * the binder's declared C type is a concrete pointer, but the init
                 * VALUE is emitted as the int64 carrier -- e.g.
                 * `(:: (.tail xs) (Cons (Option int)))` emits `(int64_t)(...)->tail`
                 * while `t0` is declared `tur_adt_Cons__Option__int *`.  `init_cn`
                 * (the init's TYPE c-name) is the pointer here, so the branch above
                 * under-fires; key on the emitted value being the carrier (its
                 * `(int64_t)` prefix) and reinterpret it to the binder's pointer.
                 * Value-preserving (int64 -> intptr_t -> pointer), and only fires
                 * for a pointer binder fed an explicitly int64-cast value. */
                buf_printf(body, "%s %s = (%s)(intptr_t)(%s);\n", bind_c, bn, bind_c, iv);
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
            bv = bridge_control_value_to_byvalue_temp(ctx, body, bv,
                                                      e->as.let_.body);
            bv = bridge_control_result_int_ptr(ctx, bv, e->type, e->as.let_.body);
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

static bool expr_is_pbp_param(EmitCtx *ctx, const Expr *struct_expr);

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
    /* panic-in-value-if-branch: mirror of the Phase R1 case -- only the
     * else-branch diverges (return/throw/panic) and the then-branch yields the
     * value, e.g. `(if cond value (panic ...))`.  Without this the temp is not
     * created, the value-producing then-branch is emitted as a discarded
     * statement, and the diverging else-branch is (wrongly) emitted as a value
     * assigned into the null temp -> `(null) = ((void)0);`. */
    bool then_no_return = !then_has_return_or_throw;
    bool only_then_diverges = then_has_return_or_throw && else_no_return;
    bool only_else_diverges = else_has_return_or_throw && then_no_return;
    /* Phase 5 carrier-bridge deletion: if either arm is a by-value Option/Result
     * producer (a monomorphized #{Construct} spec), the merge temp must be that
     * by-value struct -- the if's own `e->type` is collapsed to the int64
     * carrier, so the default temp decl would type it int64 and the by-value arm
     * would `cc`-mismatch.  Declare the temp by-value and bridge each
     * carrier-producing arm (carrier->concrete; a deref, never a dangling
     * stack spill). */
    Type if_bv = type_simple(TY_UNKNOWN, CK_COPY);
    if (!nil_result && ( !any_has_return_or_throw || only_then_diverges || only_else_diverges)) {
        tmp = fresh_tmp(ctx);
        if_bv = fn_body_tail_byvalue_carrier_type(ctx, e);
        if (if_bv.kind != TY_UNKNOWN)
            emit_temp_decl(ctx, body, if_bv, tmp, NULL);
        else
            emit_control_result_temp_decl(ctx, body, e->type, e, tmp);
    }
    char *cond = emit_value(ctx, body, e->as.if_.cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) {\n", cond);
    free(cond);
    ctx->indent += 4;
    /* Emit the then-branch as a discarded statement only when it itself
     * diverges (return/throw/panic) or the whole `if` is nil-typed.  When only
     * the *else*-branch diverges the then-branch still yields the merged value
     * and must be assigned into the temp -- see panic-in-value-if-branch. */
    if (nil_result || then_has_return_or_throw) {
        emit_stmt(ctx, body, e->as.if_.then_);
    } else {
        char *t = emit_value(ctx, body, e->as.if_.then_);
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
        /* RSP1: returning a pass-by-ptr struct *parameter* as an if-branch
         * result.  The param is held in C as `const T *`, but the by-value
         * merge temp is a `T`; assigning the pointer to the value is a hard cc
         * mismatch ('T' from 'const T *').  Dereference the param to copy it
         * out.  (A `make-struct` arm already produces a by-value aggregate, so
         * only the bare-param arm needs the deref.) */
        bool then_is_byptr_param = expr_is_pbp_param(ctx, th) && !temp_is_ptr;
        if (if_bv.kind != TY_UNKNOWN &&
            !fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.if_.then_) &&
            !emit_arm_is_recorded_byval_agg(ctx, t, if_bv) &&
            !emit_arm_is_byval_agg_var(ctx, e->as.if_.then_, if_bv)) {
            /* by-value merge temp, carrier-producing arm: bridge to concrete */
            t = emit_carrier_bridge(ctx, body, t, CK_CARRIER, CK_CONCRETE, if_bv);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, t);
        } else if (then_is_fat_var && temp_is_ptr) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = (void *)(intptr_t)(%s);\n", tmp, t);
        } else if (then_is_byptr_param) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = *(%s);\n", tmp, t);
        } else {
            indent_buf(body, ctx->indent);
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
        if (nil_result || else_has_return_or_throw) {
            emit_stmt(ctx, body, e->as.if_.else_or_null);
        } else {
            char *el = emit_value(ctx, body, e->as.if_.else_or_null);
            /* See the then-branch comment above. */
            const Expr *eb = e->as.if_.else_or_null;
            while (eb && eb->kind == EX_ASCRIBE) eb = eb->as.ascribe_.inner;
            bool else_is_fat_var = eb && eb->kind == EX_VAR && eb->as.var.binding
                                    && eb->as.var.binding->is_fat;
            const char *temp_c2 = type_c_name(e->type);
            size_t tlen2 = strlen(temp_c2);
            bool temp_is_ptr2 = tlen2 > 0 && temp_c2[tlen2 - 1] == '*';
            /* RSP1: see the then-branch comment -- deref a pass-by-ptr struct
             * parameter returned as the else-branch result. */
            bool else_is_byptr_param = expr_is_pbp_param(ctx, eb) && !temp_is_ptr2;
            if (if_bv.kind != TY_UNKNOWN &&
                !fn_body_tail_emits_byvalue_carrier_abi(ctx, e->as.if_.else_or_null) &&
                !emit_arm_is_recorded_byval_agg(ctx, el, if_bv) &&
                !emit_arm_is_byval_agg_var(ctx, e->as.if_.else_or_null, if_bv)) {
                el = emit_carrier_bridge(ctx, body, el, CK_CARRIER, CK_CONCRETE, if_bv);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", tmp, el);
            } else if (else_is_fat_var && temp_is_ptr2) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = (void *)(intptr_t)(%s);\n", tmp, el);
            } else if (else_is_byptr_param) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = *(%s);\n", tmp, el);
            } else {
                indent_buf(body, ctx->indent);
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
    /* tmp is NULL when no merge temp was created: a nil-typed `if`, or a
     * non-nil `if` in which both branches diverge (return/throw/panic) so no
     * value ever flows out.  Yield the nil placeholder rather than a bare NULL
     * (which would print as `(null)`). */
    return tmp ? tmp : atom_nil();
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
            /* Phase 3/4: If last item is return or panic, emit as statement only */
            if (last->kind == EX_RETURN || last->kind == EX_PANIC ||
                last->kind == EX_PANIC_WITH) {
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
                    emit_control_result_temp_decl(ctx, body, last->type, last, result);
                }
                char *v = emit_value(ctx, body, last);
                v = bridge_control_value_to_byvalue_temp(ctx, body, v, last);
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
            emit_control_result_temp_decl(ctx, body, last->type, last, result);
            char *v = emit_value(ctx, body, last);
            v = bridge_control_value_to_byvalue_temp(ctx, body, v, last);
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
    for (uint32_t _i = 0; _i < ctx->n_pbp_params; _i++) {
        if (ctx->pbp_param_ptrs[_i] == b) return true;
    }
    return false;
}

/* end-to-end-monomorphization: resolve the concrete monomorphized type of an
 * EX_VAR that is a parameter of the active ABI spec.  `emit_resolve_type` does
 * not substitute the spec's element bindings into a parametric param type
 * (e.g. an instance-method receiver `x : (Vec A)` stays `(Vec A)` even though
 * the spec is `(Vec int)`), so consult `current_abi_specialization->arg_types[]`
 * by matching the binding against `fn->params[]` -- the same pattern Path A
 * uses for `.field` access (emit_expr.c §4249).  Returns true and writes the
 * concrete arg type into *out when the var is such a spec param; false
 * otherwise (caller falls back to emit_resolve_type). */
bool emit_spec_arg_type_for_binding(EmitCtx *ctx, const struct Binding *b,
                                    Type *out) {
    if (!ctx || !b) return false;
    const EmitAbiSpecialization *aspec = ctx->current_abi_specialization;
    if (!aspec || !aspec->fn) return false;
    FnDef *fd = aspec->fn;
    for (uint32_t pi = 0; pi < fd->n_params && pi < aspec->n_args; pi++) {
        if (fd->params[pi] == b) {
            *out = aspec->arg_types[pi];
            /* R3 gate: a spec's monomorphized arg type must be concrete.  A
             * leftover parametric param here means the value crossing would fall
             * back to the int64 carrier where a concrete representation is
             * required -- the routing hole this asserts away. */
            emit_abi_assert_routed_concrete(ctx, out,
                                            "emit_spec_arg_type_for_binding", false);
            return true;
        }
    }
    return false;
}

static bool emit_var_spec_arg_type(EmitCtx *ctx, const Expr *var_expr,
                                   Type *out) {
    if (!ctx || !var_expr || var_expr->kind != EX_VAR) return false;
    return emit_spec_arg_type_for_binding(ctx, var_expr->as.var.binding, out);
}

/* M7 by-value HKT (gap G6): inside a per-(f, A) by-value instance-method spec
 * (`__inst_Functor_fmap_T__spec__*`), an ADT constructor call in the body --
 * `(AltF (g x) (g y))`, `(EmptyF)` -- has its element ERASED to a bare TY_ADT at
 * instance elaboration, so the normal `type_adt_app_ctor_suffix(e->type)` yields
 * no suffix and the carrier `ctor_AltF` is emitted (int64 fields) where the
 * consumer reads the by-value `tur_adt_ReF__bool` (bool fields).  Recover the
 * monomorphized suffix from the ACTIVE spec's result family: a Functor/Bifunctor
 * instance body constructs exactly the family the method returns, so the spec's
 * concrete result element (`(ReF bool)`) names the right ctor variant.  Returns a
 * malloc'd suffix (caller frees) or NULL when not applicable. */
static char *emit_hkt_spec_ctor_suffix(EmitCtx *ctx, const Expr *e) {
    if (!ctx || !ctx->current_abi_specialization || !e ||
        e->kind != EX_CALL || !e->as.call_.ctor || !e->as.call_.ctor->adt)
        return NULL;
    Type sret = emit_resolve_type(ctx, ctx->current_abi_specialization->result_type);
    if (sret.kind != TY_APP) return NULL;
    AdtDef *sdef = type_adt_app_def(&sret);
    if (!sdef || sdef != e->as.call_.ctor->adt) return NULL;
    if (!type_app_is_concrete_adt(&sret)) return NULL;
    return type_adt_app_ctor_suffix(sret);
}

/* Emit the propagation check that returns a zero of the enclosing function's C
 * return type when a panic signal is pending (panic-return-signal, always-on). */
static void emit_panic_signal_return(EmitCtx *ctx, Buf *body) {
    const char *rt = ctx->current_fn_ret_ctype;
    /* BR3b: inside the stackless trampoline a fallible reader call routes its
     * panic to the driver's `for(;;)` unwind loop with `break`, never a `return`
     * that would leave the driver mid-flight (leaking the __k chain). */
    if (ctx->panic_signal_is_break) {
        indent_buf(body, ctx->indent);
        buf_puts(body, "if (tur_panicking) break;\n");
        return;
    }
    indent_buf(body, ctx->indent);
    if (rt && strcmp(rt, "void") == 0) {
        buf_puts(body, "if (tur_panicking) return;\n");
    } else if (rt) {
        /* A zero of the exact declared return type propagates the signal.
         * S1: `((T)0)` for scalars -- c2mir rejects a scalar compound literal,
         * and this is the highest-volume `(T){0}` site in the emitter. */
        char *rzero = emit_c_zero_of(rt);
        buf_printf(body, "if (tur_panicking) return %s;\n", rzero ? rzero : "0");
        free(rzero);
    } else {
        /* D1a prototype limit: the enclosing function's C return type is not
         * recorded here (some carrier/dict-clone closure shapes), so we cannot
         * synthesize a well-typed propagation return.  Emit nothing rather than
         * an ill-typed `return;` -- a panic raised through such a function does
         * not yet propagate under this prototype (see the plan's Scope note). */
        buf_puts(body, "/* panic-return-signal: ret ctype unknown; no propagation here */\n");
    }
}

/* catch-unwind-thunk-closure-leak: is a catch-unwind/-panic-of thunk a fresh
 * closure literal whose fat box THIS call site owns?  A closure literal
 * (EX_CLOSURE), an auto-shimmed bare fn (EX_FN_TO_FAT), or a boxed poly method
 * (EX_POLY_TO_FAT) each materialize a freshly malloc'd fat box that is consumed
 * synchronously by tur_catch_unwind_box (invoked, never stored in the result),
 * so the box can be freed right after the catch.  A bare variable is NOT owned
 * here -- `(catch-unwind my-shared-thunk)` would free a closure the caller
 * still holds -- so it is deliberately excluded. */
static bool catch_thunk_owns_fat_box(const Expr *thunk) {
    if (!thunk) return false;
    switch (thunk->kind) {
        case EX_CLOSURE:
        case EX_FN_TO_FAT:
        case EX_POLY_TO_FAT:
            return true;
        default:
            return false;
    }
}

static char *emit_value_dispatch(EmitCtx *ctx, Buf *body, const Expr *e);

/* A-normalize a panic-capable call (always-on since panic-return-signal
 * graduated).  Every direct call is hoisted into a temporary so a tur_panicking
 * check can follow it; a pending panic then propagates by an early return of a
 * zero of the current function's return type.  void/never calls carry
 * no usable value and are checked where they appear as statements
 * (EX_PANIC / EX_PANIC_WITH). */
char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* G3 general catch-unwind splitter: a registered hole emits its C temp name
     * verbatim (the suspended sub-expression's already-delivered value). */
    for (uint8_t i = 0; i < ctx->n_sub_holes; i++)
        if (ctx->sub_holes[i] == e) return strdup(ctx->sub_names[i]);
    char *v = emit_value_dispatch(ctx, body, e);
    /* S1/findings 16: capture-and-clear the builder's ret-type note
     * UNCONDITIONALLY, so a note set by a void/never call (whose hoist below
     * is skipped) can never leak onto a later, unrelated call. */
    char ret_note[sizeof ctx->call_ret_note];
    memcpy(ret_note, ctx->call_ret_note, sizeof ret_note);
    ctx->call_ret_note[0] = '\0';
    if (e->kind != EX_CALL) return v;
    /* unit/void and never/diverging calls emit as C `void`, so there is no
     * value to bind into an __auto_type temp; their panic signal is handled at
     * statement position (EX_PANIC / EX_PANIC_WITH). */
    if (e->type.kind == TY_NIL || e->type.kind == TY_NEVER) return v;
    /* Hoist the call into a temp so a tur_panicking check can follow it.  Use
     * __auto_type (GNU C, supported by the gcc/clang the backend targets) so
     * the temp takes the call's EXACT emitted C representation -- carrier int64
     * vs by-value aggregate vs pointer -- without re-deriving it (the repr
     * heuristic disagrees with the emitted form for some carrier calls). */
    char tmp[64];
    snprintf(tmp, sizeof tmp, "__ps_%d", ctx->tmp_n++);
    indent_buf(body, ctx->indent);
    /* S1 (jit-engine-plan section 4): name the type when we can prove it, and
     * fall back to __auto_type when we cannot.
     *
     * The temp must take the call's EXACT emitted C representation -- carrier
     * int64 vs by-value aggregate vs pointer -- and the comment this replaces
     * was right that the repr heuristic disagrees with the emitted form for some
     * carrier calls.  So this does not re-derive anything: for a DIRECT call it
     * looks up the callee's return type as the forward-declaration pass actually
     * wrote it, which is by construction what __auto_type deduced.
     *
     * Indirect calls (through a fat-closure field, a thunk typedef, or a cast
     * function pointer) keep __auto_type; they are the residue S1 has still to
     * close, and leaving them inferred is strictly safer than guessing. */
    const char *ret_ct = NULL;
    {
        const char *q = v;
        while (*q == '(') q++;            /* tolerate a parenthesized callee */
        size_t idlen = 0;
        while (q[idlen] && (isalnum((unsigned char)q[idlen]) || q[idlen] == '_')) idlen++;
        if (idlen > 0 && q[idlen] == '(') {
            char callee[256];
            if (idlen < sizeof callee) {
                memcpy(callee, q, idlen);
                callee[idlen] = '\0';
                /* INT64_C/UINT64_C: stdint's integer-constant macros.  An
                 * exact read, not a guess -- giving the literal that type is
                 * the macro's entire purpose. */
                if (strcmp(callee, "INT64_C") == 0)       ret_ct = "int64_t";
                else if (strcmp(callee, "UINT64_C") == 0) ret_ct = "uint64_t";
                else {
                    const char *rt = emit_sig_lookup_ret_ctype(callee);
                    /* A `void` return never reaches here (filtered above), and an
                     * empty record is no better than inference. */
                    if (rt && *rt && strcmp(rt, "void") != 0) ret_ct = rt;
                }
            }
        }
    }
    /* Indirect calls: the builder's note carries the same ret type it spelled
     * into the call text's own cast / thunk typedef.  Name lookup wins when
     * both exist (it is the forward declaration, the strongest ground truth). */
    if (!ret_ct && ret_note[0] != '\0' && strcmp(ret_note, "void") != 0)
        ret_ct = ret_note;
    /* Last resort before __auto_type: two ANCHORED exact reads of text the
     * emitter itself just wrote (the same two rules the spike normalizer
     * carried, ported to the one place they are needed -- findings 16).  `v`
     * for an EX_CALL is composed entirely by this file and emit_core.c, never
     * by user text, so position 0 is emitter output by construction.
     *   1. `((RET (*)` -- a cast-function-pointer call head (the dict-vtable
     *      dispatch emit_call_name composes, whose note cannot survive the
     *      argument emissions that follow it).  RET is read up to the ` (*)`.
     *   2. `((T)(`     -- a cast wrap; T restricted to primitive spellings or
     *      anything ending `*`, so a parenthesized-callee call `(f)(x)` can
     *      never be misread as a cast (normalizer 11.3's lesson, kept). */
    char read_ct[256];
    if (!ret_ct) {
        const char *q = v;
        while (*q == '(') q++;
        const char *star = strstr(q, "(*)");
        if (star && star > q && star < q + sizeof read_ct) {
            size_t tlen = (size_t)(star - q);
            while (tlen > 0 && q[tlen - 1] == ' ') tlen--;
            bool ident_ok = tlen > 0;
            for (size_t i = 0; i < tlen && ident_ok; i++)
                if (!isalnum((unsigned char)q[i]) && q[i] != '_' &&
                    q[i] != ' ' && q[i] != '*') ident_ok = false;
            if (ident_ok) {
                memcpy(read_ct, q, tlen);
                read_ct[tlen] = '\0';
                if (strcmp(read_ct, "void") != 0) ret_ct = read_ct;
            }
        }
    }
    if (!ret_ct && v[0] == '(') {
        /* One leading paren, not two: the hoist's own printf adds the outer
         * pair, so a cast wrap arrives here as `(T)(expr)`. */
        const char *q = v + 1;
        while (*q == '(') q++;
        const char *close = strchr(q, ')');
        if (close && close > q && close[1] == '(' && close - q < (long)sizeof read_ct) {
            size_t tlen = (size_t)(close - q);
            static const char *prim[] = {"bool","_Bool","char","short","int","long",
                "float","double","unsigned","signed","int8_t","int16_t","int32_t",
                "int64_t","uint8_t","uint16_t","uint32_t","uint64_t","intptr_t",
                "uintptr_t","size_t","ssize_t","ptrdiff_t"};
            memcpy(read_ct, q, tlen);
            read_ct[tlen] = '\0';
            bool ok = tlen > 0 && read_ct[tlen - 1] == '*';
            for (size_t i = 0; !ok && i < sizeof prim / sizeof prim[0]; i++)
                ok = strcmp(read_ct, prim[i]) == 0;
            if (ok) ret_ct = read_ct;
        }
    }
    if (ret_ct)
        buf_printf(body, "%s %s = (%s);\n", ret_ct, tmp, v);
    else
        buf_printf(body, "__auto_type %s = (%s);\n", tmp, v);
    /* A constructor call (`ctor_X(...)`) always returns the concrete heap pointer
     * `tur_adt_X *`, even though its `(X ..)` type c-names to the int64 carrier
     * under emit_binding_repr_c_name.  Capture that before freeing v so the temp
     * is recorded with its ACTUAL pointer representation. */
    bool v_is_ctor = strncmp(v, "ctor_", 5) == 0 || strncmp(v, "(ctor_", 6) == 0;
    free(v);
    emit_panic_signal_return(ctx, body);
    /* gcc14-int-conversion (carrier-representation-tracking): record this call
     * temp's representation C type when it is a concrete pointer, so a later
     * `int64_t z = __ps_N;` straddle site (binder init, tail-backedge arg) can
     * detect the reverse int64<-pointer straddle.  The __auto_type temp takes the
     * call's real emitted type, which the source-type c-name can collide with the
     * carrier; only a concrete pointer is recorded, and a mis-recorded carrier
     * value would only add a value-preserving no-op cast, never a wrong one. */
    {
        const char *rcty = emit_binding_repr_c_name(ctx, e->type, e);
        /* repr-typed-pointer straddle (increment 4 follow-up): when the temp's
         * DECLARED C type is known (`ret_ct` -- read from the callee's own
         * forward declaration, i.e. by construction what __auto_type would have
         * deduced), that is ground truth and the side table must record it.
         * Re-deriving from the source TYPE disagrees with the emitted form for
         * a concrete heap ADT whose callee returns the int64 carrier: since
         * increment 4 stage 3, `(HBox int)` c-names to `tur_adt_HBox__int *`,
         * so the temp was DECLARED `int64_t` but RECORDED as a pointer.  Every
         * downstream straddle bridge keys on the recorded representation, so
         * all of them read "pointer -> pointer, nothing to do" and emitted
         * `tur_adt_HBox__int * m = __ps_N;` -- an int64->pointer initialisation
         * that Apple clang rejects outright (-Wint-conversion is an error by
         * default there, a warning under the older gcc/clang on CI).
         * Recording the declared type restores the agreement the bridges
         * assume; it is value-preserving and only ever adds a reinterpret. */
        if (ret_ct && *ret_ct) rcty = ret_ct;
        /* hkt-fmap-result-is-not-droppable: a carrier-dispatched typeclass method
         * whose call-node result type was REFINED to a pointer-family handle.
         *
         * `__inst_Functor_fmap_T` returns the int64 carrier, but the call node's
         * type is now the grounded `rc<int>` (elab_typeclasses.c collapses an
         * applied HKT result `(f b)` over a pointer-family head so the result can
         * be `rc/drop`ped).  `rc<int>` c-names to `RcControlBlock *`, so recording
         * the temp as a concrete pointer hides the int64->pointer straddle from the
         * binder-init bridge below and `RcControlBlock *m = __ps_N;` becomes a
         * -Wint-conversion in the user's own build.
         *
         * Scoped to exactly that shape: the call node's type is a pointer-family
         * handle AND the callee's own declared result is an applied HKT `(f b)`
         * (TY_APP), which is what identifies the refinement.  Keying only on
         * "callee c-names to int64_t" is far too broad -- a BY-VALUE SPECIALIZED
         * callee (`vec_new__spec__tur_adt_Vec__int__`) really does return the
         * concrete pointer while its generic signature still c-names to the
         * carrier, and overriding there adds a redundant cast to every such site
         * (140 fixtures of churn). */
        if (rcty && e->kind == EX_CALL && e->as.call_.fn_binding &&
            e->as.call_.fn_binding->type.kind == TY_FN &&
            (e->type.kind == TY_RC || e->type.kind == TY_WEAK ||
             e->type.kind == TY_REF || e->type.kind == TY_LREF)) {
            const Binding *cb = e->as.call_.fn_binding;
            const Type *cret = cb->type.as.fn.result_full_type;
            if (cret && cret->kind == TY_APP) rcty = "int64_t";
        }
        size_t rL = rcty ? strlen(rcty) : 0;
        bool recorded = false;
        if (rcty && rL >= 1 && rcty[rL - 1] == '*' && strcmp(rcty, "void *") != 0) {
            emit_localvar_record_ctype(tmp, rcty);
            recorded = true;
        } else if (rcty && strcmp(rcty, "void *") == 0) {
            /* clang int-conversion (reverse straddle): a `void *`-returning call
             * (an `extern-c ... :ptr` ascribed to an opaque carrier like String)
             * whose __auto_type panic temp keeps the `void *` type.  Record it so
             * a later `int64_t z = __ps_N;` / `return __ps_N;` straddle site (the
             * carrier binder / int64-carrier return) can bridge through intptr_t.
             * The existing concrete-pointer consumers all skip a `void *` entry
             * (they gate on `!= "void *"`), so this only enables the new
             * void*->int64 carrier bridges and is otherwise inert. */
            emit_localvar_record_ctype(tmp, rcty);
            recorded = true;
        }
        /* Ctor result: a parametric heap-ADT ctor (`ctor_Line`) returns the
         * concrete `tur_adt_Line *` even though its `(Line ..)` type c-names to the
         * int64 carrier (both type_c_name and emit_type_c_name collapse it).
         * Reconstruct the concrete pointer from the ctor's own name so
         * `int64_t z = <ctor temp>` at the binder init bridges. */
        if (!recorded && v_is_ctor && e->kind == EX_CALL &&
            e->as.call_.fn_binding && e->as.call_.fn_binding->name &&
            e->as.call_.fn_binding->name->name &&
            (type_is_heap_adt(emit_resolve_type(ctx, e->type)) ||
             type_is_heap_struct(emit_resolve_type(ctx, e->type)))) {
            char *mn = mangle_field_name(e->as.call_.fn_binding->name->name);
            if (mn) {
                Buf _cb; buf_init(&_cb);
                buf_printf(&_cb, "tur_adt_%s *", mn);
                buf_putc(&_cb, '\0');
                emit_localvar_record_ctype(tmp, _cb.data);
                buf_free(&_cb);
                free(mn);
            }
        }
        /* Also record a genuinely int64-carrier call temp, so a
         * pointer-returning function whose tail is such a bare temp
         * (`return __ps_224;` with the fn returning `tur_adt_Point *`) can detect
         * the forward int64->pointer return straddle.  A ctor temp was already
         * reclassified to its concrete pointer above, so this does not mislabel
         * one; the binder-init / tail-backedge bridges key on a POINTER entry, so
         * an int64 entry never triggers them. */
        if (!recorded && rcty && strcmp(rcty, "int64_t") == 0 && !v_is_ctor)
            emit_localvar_record_ctype(tmp, "int64_t");
    }
    return strdup(tmp);
}

static char *emit_value_dispatch(EmitCtx *ctx, Buf *body, const Expr *e) {
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
            /* S1: scalars come back as `((T)0)`; aggregates keep the compound
             * literal, which is the only spelling that zeroes every field. */
            char *result = emit_c_zero_of(cname);
            return result ? result : strdup("0");
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
                        if (ctx->specialized_call_exprs[si] != inner_call) continue;
                        /* multi-param-struct-annotation-degenerate-tyapp: a
                         * specialized clone returns the concrete type only when
                         * its spec result_type matches the reinterpret target.
                         * An inline-C accessor spec (e.g. map-get-eq-o : V) keeps
                         * the int64 carrier as its result_type -- forced by the
                         * carrier-forcing block in emit_module.c -- so a
                         * `(:: ... :float)` / `:cstr` read MUST still apply the
                         * carrier reinterpret.  Mirror the first loop's type_eq
                         * guard: look the spec up by clone name and only skip the
                         * reinterpret when it genuinely returns e->type.  When no
                         * spec is found, keep the legacy skip (the clone already
                         * returned the concrete type). */
                        const char *clone = ctx->specialized_call_names[si];
                        bool returns_concrete = true;
                        if (clone) {
                            for (uint32_t sj = 0; sj < ctx->n_abi_specializations; sj++) {
                                const EmitAbiSpecialization *sp = &ctx->abi_specializations[sj];
                                if (sp->clone_name && strcmp(sp->clone_name, clone) == 0) {
                                    returns_concrete = type_eq(sp->result_type, e->type);
                                    break;
                                }
                            }
                        }
                        if (returns_concrete)
                            return emit_value(ctx, body, inner_call);
                        break;  /* carrier-returning spec: fall through to reinterpret */
                    }
                }
            }
            TypeKind src_kind = e->as.reinterpret_.source_kind;
            TypeKind dst_kind = e->as.reinterpret_.target_kind;
            /* collections-cannot-hold-rc-values item 2: an rc<T> crossing the
             * int64 element carrier.  The bits are just the control-block
             * pointer, so the crossing itself is a cast -- what matters is the
             * count.  elab (own_carry_for_arg / own_carry_for_result) decided
             * whether this crossing mints a reference or moves one; here we
             * only stamp the increment it asked for.  The Vec releases its own
             * count via bit 1 of the `owned` flag threaded by stdlib/vec.tur. */
            if (src_kind == TY_RC || dst_kind == TY_RC) {
                char *rc_inner = emit_value(ctx, body, e->as.reinterpret_.expr);
                char *rc_tmp = fresh_tmp(ctx);
                buf_printf(body, "RcControlBlock *%s = (RcControlBlock *)(intptr_t)(%s);\n",
                           rc_tmp, rc_inner);
                free(rc_inner);
                if (e->as.reinterpret_.retain)
                    buf_printf(body, "if (%s) { rc_strong_increment(%s); }\n", rc_tmp, rc_tmp);
                Buf rc_out; buf_init(&rc_out);
                if (dst_kind == TY_RC) buf_printf(&rc_out, "%s", rc_tmp);
                else                   buf_printf(&rc_out, "(int64_t)(intptr_t)%s", rc_tmp);
                free(rc_tmp);
                buf_putc(&rc_out, '\0');
                return rc_out.data;
            }
            int src_size = reinterpret_kind_size_bytes(src_kind);
            int dst_size = reinterpret_kind_size_bytes(dst_kind);
            if (!reinterpret_kind_is_scalar(src_kind) || !reinterpret_kind_is_scalar(dst_kind) ||
                src_size <= 0 || dst_size <= 0) {
                /* collections-cannot-hold-rc-values: the common way to reach
                 * here is storing an owning value in a collection --
                 * `(vec-of (rc/clone a))`, `(hamt-of :k some-rc)`.  Elements go
                 * through an int64 carrier and rc/weak/ref have no
                 * reinterpretation to it.  The bare "invalid EX_REINTERPRET
                 * rc -> int" gave no hint that an ordinary program, not a
                 * compiler invariant, was at fault.
                 *
                 * This is still an abort with no span: the emit layer has no
                 * diagnostic channel (no diag_emit call exists in it), so a
                 * proper span'd error has to be raised earlier.  vec-of is a
                 * macro rather than a variadic defn, so the rest-arg check in
                 * elab_call.c is not the right hook either -- finding the right
                 * one is the open half of the report. */
                bool owning_src = src_kind == TY_RC || src_kind == TY_WEAK ||
                                  src_kind == TY_REF || src_kind == TY_LREF;
                bool owning_dst = dst_kind == TY_RC || dst_kind == TY_WEAK ||
                                  dst_kind == TY_REF || dst_kind == TY_LREF;
                if (owning_src || owning_dst) {
                    fprintf(stderr,
                            "tur: cannot store an owning value (%s) through the "
                            "int64 element carrier -- collections (vec, map/hamt) "
                            "cannot hold %s today.\n"
                            "  Store a plain handle instead, or keep the %s "
                            "outside the collection.\n"
                            "  See docs/archive/history/collections-cannot-hold-rc-values.md\n",
                            typekind_to_string(owning_src ? src_kind : dst_kind),
                            typekind_to_string(owning_src ? src_kind : dst_kind),
                            typekind_to_string(owning_src ? src_kind : dst_kind));
                } else {
                    fprintf(stderr,
                            "tur: emit: invalid EX_REINTERPRET %s -> %s\n",
                            typekind_to_string(src_kind), typekind_to_string(dst_kind));
                }
                abort();
            }

            char *inner = emit_value(ctx, body, e->as.reinterpret_.expr);
            Type src = type_simple(src_kind, CK_COPY);
            Type dst = type_simple(dst_kind, CK_COPY);
            Buf out; buf_init(&out);
            if (src_size == dst_size) {
                buf_printf(&out, "((union { %s s; %s d; }){.s = %s}).d",
                           type_c_name(src), type_c_name(dst), inner);
            } else if (src_kind == TY_FLOAT32 || dst_kind == TY_FLOAT32 ||
                       src_kind == TY_FLOAT || dst_kind == TY_FLOAT ||
                       src_kind == TY_FLOAT64 || dst_kind == TY_FLOAT64) {
                /* float32-generic-call-result-printed-as-carrier: a mixed-size
                 * pair involving a float is the int64 carrier holding float32
                 * bits (elab admits exactly that pair).  A C value cast here
                 * would CONVERT -- `(float)(int64_t)` of the bit pattern is
                 * garbage -- so use the union overlay, whose smaller member
                 * reads the carrier's low bytes: the same idiom
                 * emit_carrier_bridge emits for the inline-scalar crossing,
                 * which is what the producing side already used. */
                buf_printf(&out, "((union { %s s; %s d; }){.s = %s}).d",
                           type_c_name(src), type_c_name(dst), inner);
            } else {
                /* Integral narrowing/widening across the int64 carrier (elab
                 * call_wrap_reinterpret only allows size mismatch when both
                 * kinds are integral). A plain C cast does the right thing. */
                buf_printf(&out, "(%s)(%s)", type_c_name(dst), inner);
            }
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
            int64_t tag = e->as.union_inject_.tag_idx;
            /* type-of-cast-kind-granularity: an `any` box carries a
             * per-monomorph id for a struct/ADT payload so two struct types are
             * distinguishable.  A TY_UNION inject's tag_idx is a MEMBER INDEX,
             * not a TypeKind, so it is left alone. */
            if (e->type.kind == TY_ANY)
                tag = emit_any_type_id(ctx, e->as.union_inject_.value->type);
            /* structdef-retirement DS-C: box_struct (a StructDef*) is always NULL
             * now -- a by-value struct is a record ADT, heap-boxed by the
             * byvalue-ADT branch below; the StructDef heap-box arm is removed. */
            if (tag == (int64_t)TY_FLOAT) {
                /* TY2.2: a double does not survive an integer cast -- store its
                 * IEEE-754 bit pattern in the payload via a union reinterpret. */
                buf_printf(&out,
                    "TUR_TAG(%lld, ((union { double d; int64_t i; }){.d = (%s)}).i)",
                    (long long)tag, inner);
            } else if (emit_type_is_byvalue_adt(ctx,
                           e->as.union_inject_.value->type)) {
                /* CONV-S1 seam 4: under the defstruct->defadt lowering a by-value
                 * struct is a record ADT, so box_struct (a StructDef*) is NULL --
                 * but the aggregate still cannot ride the int64 carrier.  Heap-box
                 * it exactly as the struct path does, using the ADT monomorph C
                 * name; EX_ANY_CAST unboxes via the same predicate on the target. */
                const char *cn = emit_type_c_name(ctx,
                    emit_resolve_type(ctx, e->as.union_inject_.value->type));
                buf_printf(&out,
                    "({ %s *__tur_box = (%s *)malloc(sizeof(%s)); "
                    "*__tur_box = (%s); TUR_TAG(%lld, (int64_t)(intptr_t)__tur_box); })",
                    cn, cn, cn, inner, (long long)tag);
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
            /* type-of-cast-kind-granularity: the same per-monomorph id the
             * inject site allocated, when the target was a named type. */
            int64_t test_tag = e->as.any_is_.test_type.kind != TY_UNKNOWN
                                   ? emit_any_type_id(ctx, e->as.any_is_.test_type)
                                   : e->as.any_is_.test_tag;
            buf_printf(&out, "(TUR_GETTAG(%s) == %lld)",
                       inner, (long long)test_tag);
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
            /* type-of-cast-kind-granularity: `e->type` IS the named target
             * type, so the cast checks per-monomorph identity -- casting an
             * `any` holding a Point to OtherStruct now panics instead of
             * reinterpreting the payload. */
            int64_t target_tag = emit_any_type_id(ctx, e->type);
            Buf out; buf_init(&out);
            /* structdef-retirement DS-C: target_struct (a StructDef*) is always
             * NULL now -- a struct target is a record ADT, unboxed by the
             * byvalue-ADT branch below; the StructDef deref arm is removed. */
            if (target_tag == (int64_t)TY_FLOAT) {
                /* TY2.2: reverse the float bit-reinterpret stored on inject. */
                buf_printf(&out,
                    "({ tur_tagged_t __tur_c = (%s); "
                    "__tur_any_cast_check(TUR_GETTAG(__tur_c), %lld); "
                    "((union { int64_t i; double d; }){.i = TUR_UNTAG(__tur_c)}).d; })",
                    inner, (long long)target_tag);
            } else if (emit_type_is_byvalue_adt(ctx, e->type)) {
                /* CONV-S1 seam 4: by-value record-ADT target (lowered defstruct).
                 * target_struct is NULL, but the payload was heap-boxed on inject,
                 * so unbox by dereferencing the pointer -- the ADT analogue of the
                 * struct deref above. */
                const char *cn = emit_type_c_name(ctx, emit_resolve_type(ctx, e->type));
                buf_printf(&out,
                    "({ tur_tagged_t __tur_c = (%s); "
                    "__tur_any_cast_check(TUR_GETTAG(__tur_c), %lld); "
                    "*(%s *)(intptr_t)TUR_UNTAG(__tur_c); })",
                    inner, (long long)target_tag, cn);
            } else {
                Type target = type_simple(e->as.any_cast_.target_kind, CK_COPY);
                buf_printf(&out,
                    "({ tur_tagged_t __tur_c = (%s); "
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
            /* van-laarhoven-lens-composition (Gap B): an AMBIENT dict value
             * forwards the enclosing constrained rank-2 fn's dict.  Inside a
             * dict-clone body that dict is the clone's own dict PARAMETER; lower
             * to it directly.  Outside a clone (the plain carrier base) fall
             * through to the representative instance's singleton. */
            if (e->as.dict_.is_ambient && ctx->dict_dispatch_param_cname) {
                Buf ab; buf_init(&ab);
                buf_printf(&ab, "(int64_t)(intptr_t)%s",
                           ctx->dict_dispatch_param_cname);
                buf_putc(&ab, '\0');
                char *ar = strdup(ab.data);
                buf_free(&ab);
                return ar;
            }
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
                /* tur_panic returns (no longjmp), so the panicking frame must
                 * propagate the signal by returning now. */
                emit_panic_signal_return(ctx, body);
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
                /* tur_panic_with returns; propagate now. */
                emit_panic_signal_return(ctx, body);
            }
            free(payload_val);
            return atom_nil();
        }
        case EX_CATCH_UNWIND: {
            /* (catch-unwind thunk) - run thunk behind a panic boundary; yields a
             * result box (ok = thunk value, err = opaque Panic handle). */
            const Expr *thunk = e->as.catch_unwind_.thunk;
            char *thunk_val = emit_value(ctx, body, thunk);
            int box_owns = 1;
            const char *box_shim = catch_thunk_box_shim(ctx, thunk, &box_owns);
            char result_var[64];
            snprintf(result_var, sizeof(result_var), "__catch_result_%d", ctx->tmp_n++);
            indent_buf(body, ctx->indent);
            if (box_shim) {
                buf_printf(body,
                    "int64_t %s = tur_catch_unwind_box_via(%s, (int64_t)(intptr_t)%s, %d);\n",
                    result_var, box_shim, thunk_val, box_owns);
            } else
            buf_printf(body, "int64_t %s = tur_catch_unwind_box((int64_t)(intptr_t)%s);\n",
                       result_var, thunk_val);
            /* catch-unwind-thunk-closure-leak: reclaim the thunk fat box once the
             * catch has consumed it, when it is a closure literal we own here (a
             * bare-variable thunk is left alone -- it may alias a shared closure). */
            if (catch_thunk_owns_fat_box(thunk)) {
                indent_buf(body, ctx->indent);
                /* closure-drop-glue: flag-on the thunk fat box is headered
                 * (env[-1] drop-glue; the fat pointer is PAST it), so a bare free
                 * of the past-header pointer is an interior free.  Release it via
                 * TUR_CLOSURE_DROP (recovers the header, walks any owning captures,
                 * frees the base).  Flag-off, keep the exact plain free -- byte
                 * captures, frees the base). */
                buf_printf(body, "TUR_CLOSURE_DROP(%s);\n", thunk_val);
            }
            free(thunk_val);
            return strdup(result_var);
        }
        case EX_CATCH_PANIC_OF: {
            /* (catch-panic-of Type thunk) - typed catch boundary; re-raises panics
             * whose payload type does not match Type. */
            const Expr *thunk = e->as.catch_panic_of_.thunk;
            TypeKind type_kind = e->as.catch_panic_of_.type_kind;
            char *thunk_val = emit_value(ctx, body, thunk);
            int box_owns = 1;
            const char *box_shim = catch_thunk_box_shim(ctx, thunk, &box_owns);
            char result_var[64];
            snprintf(result_var, sizeof(result_var), "__catch_panic_of_result_%d", ctx->tmp_n++);
            indent_buf(body, ctx->indent);
            if (box_shim) {
                buf_printf(body,
                    "int64_t %s = tur_catch_panic_of_box_via(%d, %s, (int64_t)(intptr_t)%s, %d);\n",
                    result_var, (int)type_kind, box_shim, thunk_val, box_owns);
            } else
            buf_printf(body, "int64_t %s = tur_catch_panic_of_box(%d, (int64_t)(intptr_t)%s);\n",
                       result_var, (int)type_kind, thunk_val);
            /* catch-unwind-thunk-closure-leak: reclaim the owned thunk fat box
             * before the re-raise check, so it is freed on both the caught and
             * the propagate paths. */
            if (catch_thunk_owns_fat_box(thunk)) {
                indent_buf(body, ctx->indent);
                /* closure-drop-glue: header-aware release of the headered thunk
                 * box (see EX_CATCH_UNWIND above). */
                buf_printf(body, "TUR_CLOSURE_DROP(%s);\n", thunk_val);
            }
            free(thunk_val);
            /* A type-mismatched catch-panic-of leaves the signal set to
             * re-raise; propagate it to the next outer boundary. */
            emit_panic_signal_return(ctx, body);
            return strdup(result_var);
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
        case EX_CALLCC:
            /* cps-backend-direct-lowering-removal D3: the CPS lowering of
             * call/cc (emit_cps_callcc) is deleted. EX_CALLCC is a coloring
             * seed (cps_directly_uses_control), so any function using call/cc is
             * colored and emitted natively by the CT-IR backend, never reaching
             * this direct-style dispatch. Reaching it is an invariant violation. */
            fprintf(stderr, "tur: emit: EX_CALLCC reached the direct emitter "
                            "(should be handled by the CT-IR backend)\n");
            abort();
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

            /* jit-ffi-c2mir-plan F3: `(call-ptr p [sig] args...)` -- an
             * indirect call through a raw address with an explicit C
             * signature.  Pure codegen: cast the pointer to the stated
             * function type and call it, casting each argument to its
             * declared C parameter type.  A :void return is wrapped in a
             * comma expression so the node stays valid in value position. */
            /* jit-ffi-c2mir-plan F5: `(callback-ptr f [sig])` -- the reverse
             * direction.  A C library needs a plain function pointer with the
             * signature IT declares, which is rarely the C signature the
             * Turmeric function emits as (an :int32 parameter arrives as
             * int32_t but the callee takes int64_t, a :bool return leaves as
             * bool but the slot wants int).  So codegen emits a per-site
             * adapter with the C signature on the outside and the callee's
             * own carrier types on the inside, and hands out its address.
             *
             * Elaboration has already restricted `f` to a named top-level
             * function, so this is a direct call by C name -- no environment
             * to recover and no closure representation to guess at.
             *
             * Emitted into pending_handler_fns (the buffer generator and
             * handler helpers use) so it lands at file scope ahead of the
             * enclosing function. */
            if (e->as.call_.ptr_sig && e->as.call_.ptr_sig->is_callback) {
                const CallPtrSig *ps = e->as.call_.ptr_sig;
                const Expr *fx = e->as.call_.fn_expr;
                uint32_t id = (uint32_t)ctx->tmp_n++;
                Buf *out = ctx->pending_handler_fns ? ctx->pending_handler_fns
                                                    : ctx->file;
                bool ret_void = (ps->return_type.kind == TY_NIL);
                const char *rc = ret_void
                                     ? "void"
                                     : emit_type_c_name(ctx, ps->return_type);
                TypeKind trk = fx->type.as.fn.result_kind;
                char *callee = raw_name_for_binding(fx->as.var.binding);

                buf_printf(out, "static %s __tur_cb_%u(", rc, id);
                if (ps->n_params == 0) {
                    buf_puts(out, "void");
                } else {
                    for (uint32_t i = 0; i < ps->n_params; i++)
                        buf_printf(out, "%s%s a%u", i ? ", " : "",
                                   emit_type_c_name(ctx, ps->param_types[i]),
                                   i);
                }
                buf_puts(out, ") {\n    ");
                if (!ret_void && trk != TY_NIL) {
                    /* F4 follow-on: C has no cast to a struct type -- an
                     * aggregate result returns uncast (the callee already
                     * produces the exact record type the slot declares). */
                    if (ps->return_type.kind == TY_ADT)
                        buf_puts(out, "return ");
                    else
                        buf_printf(out, "return (%s)", rc);
                }
                buf_printf(out, "%s(", callee);
                for (uint32_t i = 0; i < ps->n_params; i++) {
                    TypeKind ak = (TypeKind)fx->type.as.fn.arg_kinds[i];
                    /* F4 follow-on: an aggregate parameter passes through
                     * uncast -- the adapter's parameter and the callee's
                     * are the same C struct type, and C forbids struct
                     * casts anyway. */
                    if (ps->param_types[i].kind == TY_ADT) {
                        buf_printf(out, "%sa%u", i ? ", " : "", i);
                        continue;
                    }
                    const char *ac =
                        emit_type_c_name(ctx, emit_type_from_kind(ak));
                    /* Pointer-carrying scalars round-trip through intptr_t,
                     * matching the outbound direction in call-ptr. */
                    if (ak == TY_CSTR || ak == TY_PTR_VOID)
                        buf_printf(out, "%s(%s)(intptr_t)a%u",
                                   i ? ", " : "", ac, i);
                    else
                        buf_printf(out, "%s(%s)a%u", i ? ", " : "", ac, i);
                }
                buf_puts(out, ");\n}\n");
                free(callee);

                Buf cb;
                buf_init(&cb);
                buf_printf(&cb, "((void *)(intptr_t)&__tur_cb_%u)", id);
                buf_putc(&cb, '\0');
                char *r = strdup(cb.data);
                buf_free(&cb);
                note_call_ret(ctx, "void *");
                return r;
            }

            if (e->as.call_.ptr_sig) {
                const CallPtrSig *ps = e->as.call_.ptr_sig;
                char *pv = emit_value(ctx, body, e->as.call_.fn_expr);
                Buf cb;
                buf_init(&cb);
                bool is_void = (ps->return_type.kind == TY_NIL);
                if (is_void) buf_putc(&cb, '(');
                buf_printf(&cb, "((%s (*)(",
                           is_void ? "void"
                                   : emit_type_c_name(ctx, ps->return_type));
                if (ps->n_params == 0) {
                    buf_puts(&cb, "void");
                } else {
                    for (uint32_t i = 0; i < ps->n_params; i++)
                        buf_printf(&cb, "%s%s", i ? ", " : "",
                                   emit_type_c_name(ctx, ps->param_types[i]));
                }
                buf_printf(&cb, "))(intptr_t)(%s))(", pv);
                free(pv);
                for (uint32_t i = 0; i < ps->n_params; i++) {
                    char *av = emit_value(ctx, body, e->as.call_.args[i]);
                    const char *pc = emit_type_c_name(ctx, ps->param_types[i]);
                    TypeKind pk = ps->param_types[i].kind;
                    /* Pointer-carrying scalars round-trip through intptr_t
                     * (the args ride the int64 carrier); numerics cast
                     * directly. */
                    if (pk == TY_CSTR || pk == TY_PTR_VOID)
                        buf_printf(&cb, "%s(%s)(intptr_t)(%s)",
                                   i ? ", " : "", pc, av);
                    /* F4: an aggregate argument is passed AS IS -- a defstruct
                     * already emits as the exact by-value C struct the
                     * signature names, and C has no cast to a struct type
                     * anyway. */
                    else if (pk == TY_ADT)
                        buf_printf(&cb, "%s(%s)", i ? ", " : "", av);
                    else
                        buf_printf(&cb, "%s(%s)(%s)", i ? ", " : "", pc, av);
                    free(av);
                }
                buf_putc(&cb, ')');
                if (is_void) buf_puts(&cb, ", INT64_C(0))");
                buf_putc(&cb, '\0');
                char *r = strdup(cb.data);
                buf_free(&cb);
                note_call_ret(ctx, is_void ? "int64_t"
                                           : emit_type_c_name(ctx,
                                                              ps->return_type));
                return r;
            }

            /* multiword-value boxing: `(tur-wide-byval? x)` is an EMIT-TIME type
             * query -- it folds to the int literal 1 when the argument's
             * monomorphized type is a wide (> 8 byte) by-value ADT (the same
             * predicate that decides value boxing), else 0.  map-assoc threads it
             * as bit 1 of the `owned` flag so the map RELEASES its boxed values.
             * Evaluated per monomorphization (concrete arg type), so it stays in
             * lockstep with the boxing decision; the pure-Turmeric fallback body
             * (returns 0) covers the interpreter, where values are never C-boxed. */
            if (fn_binding && fn_binding->name && fn_binding->name->name &&
                strcmp(fn_binding->name->name, "tur-wide-byval?") == 0 &&
                e->as.call_.n_args == 1) {
                /* map-assoc-move-typed-value: the query is a pure TYPE probe --
                 * its body discards the argument -- so callers hand it a BORROW
                 * `(& v)` rather than the value, which keeps a move-typed value
                 * usable at its real argument position.  Peel the borrow at the
                 * EXPRESSION level, not the type level: TY_REF_IMMUT carries
                 * only the target's TypeKind, which would lose the ADT def that
                 * type_is_wide_byval_adt needs. */
                const Expr *probe = e->as.call_.args[0];
                if (probe && probe->kind == EX_BORROW_IMMUT &&
                    probe->as.borrow_immut_.expr)
                    probe = probe->as.borrow_immut_.expr;
                Type at = emit_resolve_type(ctx, probe->type);
                /* Increment 3: the fold follows the CONTAINER-ELEMENT boxing
                 * predicate (any-width by-value product), not the wide-only
                 * one, so the map's release decision cannot drift from the
                 * insert-side boxing decision for narrow elements. */
                bool wide = !type_is_heap_struct(at) && !type_is_heap_adt(at) &&
                            type_is_boxed_container_elem(at);
                return strdup(wide ? "INT64_C(1)" : "INT64_C(0)");
            }

            /* collections-cannot-hold-rc-values (map side, step c):
             * `(tur-rc-value? x)` is the refcount-side twin of
             * `tur-wide-byval?` -- it folds to 1 when the argument's
             * monomorphized type is an rc<T>.  map-assoc threads it as bit 2 of
             * the `owned` flag so the map takes a strong reference on insert and
             * releases it when the entry dies.  Like tur-wide-byval? it is a
             * pure TYPE probe taking a BORROW, so a move-typed rc value stays
             * usable at its real argument position; the same expression-level
             * peel applies, since TY_REF_IMMUT would erase the rc-ness we are
             * asking about.  The pure-Turmeric fallback body (returns 0) covers
             * the interpreter, where rc values are not carrier-erased. */
            if (fn_binding && fn_binding->name && fn_binding->name->name &&
                strcmp(fn_binding->name->name, "tur-rc-value?") == 0 &&
                e->as.call_.n_args == 1) {
                const Expr *probe = e->as.call_.args[0];
                if (probe && probe->kind == EX_BORROW_IMMUT &&
                    probe->as.borrow_immut_.expr)
                    probe = probe->as.borrow_immut_.expr;
                bool is_rc = emit_resolve_type(ctx, probe->type).kind == TY_RC;
                return strdup(is_rc ? "INT64_C(1)" : "INT64_C(0)");
            }

            /* multiword-element boxing (Vec): `(tur-vec-elem-wide? v)` is the
             * ELEMENT-side twin of `tur-wide-byval?` -- it folds to 1 when the
             * argument's monomorphized `(Vec A)` element type A is a wide (> 8
             * byte) by-value ADT (the same predicate that decides element
             * boxing in emit_carrier_bridge_escaping), else 0.  vec-free threads
             * it to vec-free-o so a Vec[Point]-style buffer FREES each element
             * box before its data buffer.  Unlike tur-wide-byval? the value in
             * hand is the container, not an element, so there is no A-typed
             * witness to inspect -- we peel A out of the `(Vec A)` TY_APP spine.
             * The pure-Turmeric fallback body (returns 0) covers the
             * interpreter, where elements ride as TuriStruct pointers, never C
             * boxes. */
            if (fn_binding && fn_binding->name && fn_binding->name->name &&
                strcmp(fn_binding->name->name, "tur-vec-elem-wide?") == 0 &&
                e->as.call_.n_args == 1) {
                Type vt = emit_resolve_type(ctx, e->as.call_.args[0]->type);
                bool wide = false;
                if (vt.kind == TY_APP && vt.as.app.arg) {
                    Type at = emit_resolve_type(ctx, *vt.as.app.arg);
                    /* Increment 3: follow the container-element boxing
                     * predicate (any-width), in lockstep with the push-side
                     * boxing, so vec-free frees narrow element boxes too. */
                    wide = !type_is_heap_struct(at) && !type_is_heap_adt(at) &&
                           type_is_boxed_container_elem(at);
                }
                return strdup(wide ? "INT64_C(1)" : "INT64_C(0)");
            }

            /* collections-cannot-hold-rc-values item 2: `(tur-vec-elem-rc? v)`
             * is the refcount-side twin of tur-vec-elem-wide? -- it folds to 1
             * when the monomorphized `(Vec A)` element type A is an rc<T>,
             * whose slots hold a strong reference the Vec took at push time
             * (own_carry_for_arg in elab_call.c) and must release on
             * free/overwrite/removal.  Threaded as bit 1 of the `owned` flag,
             * mirroring how map-assoc threads tur-wide-byval?.  The
             * pure-Turmeric fallback body (returns 0) covers the interpreter,
             * where rc values are not carrier-erased. */
            if (fn_binding && fn_binding->name && fn_binding->name->name &&
                strcmp(fn_binding->name->name, "tur-vec-elem-rc?") == 0 &&
                e->as.call_.n_args == 1) {
                Type vt = emit_resolve_type(ctx, e->as.call_.args[0]->type);
                bool is_rc = false;
                if (vt.kind == TY_APP && vt.as.app.arg)
                    is_rc = emit_resolve_type(ctx, *vt.as.app.arg).kind == TY_RC;
                return strdup(is_rc ? "INT64_C(1)" : "INT64_C(0)");
            }

            /* CM3 (van-laarhoven-consumer-mono-plan): rewrite a call to an
             * ambiguous consumer -- `(consumer concrete-lens args...)` -- to the
             * box-free clone `<consumer>__lens_<hash>(args...)`, dropping the lens
             * arg.  Fires only when THIS site's lens arg is a statically-known
             * concrete lens in the consumer's resolved set; a runtime-selected
             * lens has no clone and falls through to the Path A carrier call.  The
             * clone's ABI matches the carrier, so each retained arg is emitted and
             * cast to its own concrete C type (which is the clone's param type). */
            if (fn_binding && fn_binding->name &&
                fn_binding->name->name) {
                int lens_idx = -1;
                const void *lb = mono_spec_consumer_call_lookup(
                    fn_binding->name->name, &lens_idx);
                if (lb && lens_idx >= 0 &&
                    (uint32_t)lens_idx < e->as.call_.n_args) {
                    const Expr *larg = e->as.call_.args[lens_idx];
                    const char *lname = cm_call_lens_name(larg);
                    /* CM3-transitive: inside a forwarding consumer's clone the
                     * lens arg IS that clone's bound lens param -- resolve it to
                     * the clone's concrete lens so `(inner l ...)` rewrites to the
                     * inner consumer's matching clone (the "singleton lens set"). */
                    const EmitAbiSpecialization *cur =
                        ctx->current_abi_specialization;
                    if (cur && cur->is_consumer_mono &&
                        cur->consumer_lens_binding &&
                        cm_call_lens_binding(larg) == cur->consumer_lens_binding)
                        lname = cur->consumer_lens_name;
                    unsigned long long lh =
                        lname ? mono_spec_lens_clone_hash(lb, lname) : 0;
                    if (lh) {
                        Buf callb; buf_init(&callb);
                        emit_vl_consumer_mono_name(&callb,
                                                   fn_binding->name->name, lh);
                        buf_putc(&callb, '(');
                        bool first = true;
                        for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                            if ((int)i == lens_idx) continue;
                            char *av = emit_value(ctx, body,
                                                  e->as.call_.args[i]);
                            if (!first) buf_puts(&callb, ", ");
                            first = false;
                            Type at = e->as.call_.args[i]->type;
                            if (type_kind_is_aggregate(at.kind))
                                buf_printf(&callb, "(%s)(intptr_t)(%s)",
                                           emit_type_c_name(ctx, at), av);
                            else
                                buf_puts(&callb, av);
                            free(av);
                        }
                        buf_putc(&callb, ')');
                        buf_putc(&callb, '\0');
                        char *r = strdup(callb.data);
                        buf_free(&callb);
                        /* findings 16.2: the consumer clone's record does not
                         * exist yet (its spec is minted after the main emit
                         * loop), so hand the hoist the ret type directly.
                         * Verified corpus-wide against the emitted forward
                         * declarations by tools/jit-spike/verify-temp-types.py. */
                        note_call_ret(ctx, emit_type_c_name(ctx, e->type));
                        return r;
                    }
                }
            }

            /* CB2 (van-laarhoven-composed-byvalue-plan): inside a by-value
             * monomorphized COMPOSED lens body (is_vl_wide_mono), a DIRECT
             * application of a NESTED lens -- `(point-x g p)` inside the adapter,
             * `(line-a adapter s)` at the tail -- is lowered through the lens's
             * MB1 dict-clone `<lens>__dict_N` and would otherwise dispatch through
             * the runtime carrier dict, reintroducing the box the whole path
             * removes.  Redirect it to the nested lens's own `<lens>__mono_<hash>`
             * body (registered by CB1), passing the trailing two args (`g`/adapter
             * and the whole `s`) by value and dropping the leading carrier dict.
             * Keyed off the concrete lens name (dict-suffix stripped) + the
             * concrete registry; a non-lens call has no mono and is untouched. */
            if (fn_binding && fn_binding->name && fn_binding->name->name &&
                ctx->current_abi_specialization &&
                ctx->current_abi_specialization->is_vl_wide_mono &&
                e->as.call_.n_args >= 2) {
                const char *nm = fn_binding->name->name;
                const char *cut = strstr(nm, "__dict_");
                size_t blen = cut ? (size_t)(cut - nm) : strlen(nm);
                char base[128];
                if (blen > 0 && blen < sizeof base) {
                    memcpy(base, nm, blen);
                    base[blen] = '\0';
                    unsigned long long h =
                        mono_spec_mono_hash_for_lens(base, NULL);
                    if (h) {
                        uint32_t na = e->as.call_.n_args;
                        char *g_str =
                            emit_value(ctx, body, e->as.call_.args[na - 2]);
                        char *s_str =
                            emit_value(ctx, body, e->as.call_.args[na - 1]);
                        Buf mo; buf_init(&mo);
                        emit_vl_mono_name(&mo, base, h);
                        buf_printf(&mo,
                                   "((int64_t)(intptr_t)(%s), "
                                   "(int64_t)(intptr_t)(%s))",
                                   g_str, s_str);
                        buf_putc(&mo, '\0');
                        char *result = strdup(mo.data);
                        buf_free(&mo);
                        free(g_str); free(s_str);
                        /* findings 16.2: same late-minted-spec gap as the
                         * consumer redirect above. */
                        note_call_ret(ctx, emit_type_c_name(ctx, e->type));
                        return result;
                    }
                }
            }

            /* Phase 16 v2: indirect capability field call — fn_expr is EX_GET_FIELD.
             * Emit: ((ret_t (*)(arg_t, ...))(intptr_t)(struct_val).field_name)(args...)
             * Effect-row annotation is advisory (erased to a plain function pointer).
             * Phase E: when the field is a typed fn ptr (concrete), call directly
             * without the intptr_t round-trip. */
            if (e->as.call_.fn_expr) {
                Expr *gf = e->as.call_.fn_expr;
                char *fn_ptr_val = emit_value(ctx, body, gf);
                const char *ret_c = type_c_name(e->type);

                /* capturing-closure-in-struct-field-segv: a BOXED fn-field (the
                 * fat representation, so a stored capturing closure works) is
                 * invoked through the fat `{thunk, env}` protocol -- read slot 0
                 * as the thunk and call it with the handle as env -- NOT the thin
                 * function-pointer cast below, which would execute the env block
                 * as code.  Emit the TUR_APPLY<N>_T macro (arg types come from the
                 * field's declared signature; the handle is both fn and env). */
                if (gf->type.kind == TY_FN && gf->type.as.fn.boxed
                    && e->as.call_.n_args <= 4) {
                    uint32_t n = e->as.call_.n_args;
                    /* Collect the declared C arg types first: whether any of
                     * them is an aggregate decides how this call is spelled. */
                    const char *arg_ct[4] = {NULL, NULL, NULL, NULL};
                    bool any_aggregate = false;
                    for (uint32_t i = 0; i < n && i < 4; i++) {
                        Type at = (gf->type.as.fn.arg_full_types &&
                                   gf->type.as.fn.arg_full_types[i])
                                      ? *gf->type.as.fn.arg_full_types[i]
                                      : emit_type_from_kind(gf->type.as.fn.arg_kinds[i]);
                        arg_ct[i] = emit_type_c_name(ctx, at);
                        if (!emit_c_type_is_scalar(arg_ct[i])) any_aggregate = true;
                    }
                    Buf out; buf_init(&out);
                    if (!any_aggregate) {
                        buf_printf(&out, "TUR_APPLY%u_T(%s", n, ret_c);
                        for (uint32_t i = 0; i < n; i++)
                            buf_printf(&out, ", %s", arg_ct[i]);
                        buf_printf(&out, ", %s", fn_ptr_val);
                        for (uint32_t i = 0; i < n; i++) {
                            char *av = emit_value(ctx, body, e->as.call_.args[i]);
                            buf_printf(&out, ", %s", av);
                            free(av);
                        }
                        buf_puts(&out, ")");
                    } else {
                        /* jit-tur-apply-casts-to-aggregate-param-type.md:
                         * TUR_APPLY<N>_T coerces every argument with `(Ai)(a)`,
                         * which is only valid while Ai is scalar -- C forbids a
                         * cast to a struct type (C11 6.5.4p2).  gcc and clang
                         * accept it as a no-op when the argument already has
                         * that type, which is exactly this case, so the cast was
                         * never doing work here; c2mir rejects it outright.
                         *
                         * The cast IS load-bearing for scalars (the int64
                         * carrier <-> pointer direction is a constraint
                         * violation without it, an error on gcc 14), so it is
                         * kept per-argument rather than dropped wholesale, and
                         * the macro stays exactly as it is for the all-scalar
                         * case -- which is every hand-written inline-C user in
                         * stdlib and the overwhelming majority of call sites.
                         *
                         * This is the macro's own expansion with that one
                         * change; keep the two in sync (emit_module.c, search
                         * TUR_APPLY0_T). */
                        buf_printf(&out, "(((%s (*)(void *", ret_c);
                        for (uint32_t i = 0; i < n; i++)
                            buf_printf(&out, ", %s", arg_ct[i]);
                        buf_printf(&out,
                                   "))(intptr_t)((int64_t *)(intptr_t)(%s))[0])"
                                   "((void *)(intptr_t)(%s)",
                                   fn_ptr_val, fn_ptr_val);
                        for (uint32_t i = 0; i < n; i++) {
                            char *av = emit_value(ctx, body, e->as.call_.args[i]);
                            if (emit_c_type_is_scalar(arg_ct[i]))
                                buf_printf(&out, ", (%s)(%s)", arg_ct[i], av);
                            else
                                buf_printf(&out, ", (%s)", av);
                            free(av);
                        }
                        buf_puts(&out, "))");
                    }
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(fn_ptr_val);
                    note_call_ret(ctx, ret_c);   /* findings 16 */
                    return result;
                }

                /* Phase E: detect concrete typed fn-ptr field.
                 * structdef-retirement: the former StructDef path stored a typed
                 * `fn` field as a directly-callable concrete function pointer.
                 * A record-ADT field (the only shape now) stores every `fn` --
                 * typed or bare -- as the int64 carrier, so it is NOT directly
                 * callable; leave is_typed_fn_field false so the intptr_t-cast
                 * path below specialises the pointer to the call's arg/result C
                 * types, exactly as for a bare `fn` field. */
                bool is_typed_fn_field = false;
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
                /* poly-hof-constrained-arg-baked-carrier: in an ABI spec body the
                 * argument of an indirect fn-pointer call (`(f a)` where `a : A`)
                 * carries its GENERIC type (the int64 carrier for A), but the spec
                 * has monomorphized it to a concrete by-value type (e.g. Box).
                 * Resolve each arg through the active spec's arg_types[] so both the
                 * carrier-bridge decision and the cast signature use the concrete
                 * type -- otherwise the cast says `(int64_t)` while the value is a
                 * `Box`, a -Wint-conversion hard error. */
                Type *resolved_arg_ty = (Type *)malloc(e->as.call_.n_args * sizeof(Type));
                if (!resolved_arg_ty && e->as.call_.n_args) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    Type rt = e->as.call_.args[i]->type;
                    Type spec_rt;
                    if (emit_var_spec_arg_type(ctx, e->as.call_.args[i], &spec_rt))
                        rt = spec_rt;
                    resolved_arg_ty[i] = rt;
                }
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
                        type_uses_carrier_in_dispatch(resolved_arg_ty[i]);
                    arg_cast[i] = needs_fn_cast || needs_carrier_bridge;
                    if (needs_fn_cast) {
                        raw = emit_carrier_reinterpret(ctx, raw, NULL, "generic-call-arg");
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
                                buf_puts(&out, type_c_name(resolved_arg_ty[i]));
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
                free(resolved_arg_ty);
                free(fn_ptr_val);
                note_call_ret(ctx, ret_c);   /* findings 16 */
                return result;
            }

            /* Phase HRT1: poly call through rank-2 fn param: emit fn_name.fn(fn_name.env, arg0, ...) */
            if (e->as.call_.is_poly_call) {
                /* VBM3: redirect a van Laarhoven lens invocation `(l g s)` --
                 * where `l` is a consumer's abstract lens param that uniquely
                 * resolves to a concrete wide-by-value-functor lens with a
                 * `<lens>__mono_<hash>` body -- straight to that by-value mono
                 * body, dropping the carrier dict-dispatch and its `(f S)` box.
                 * The mono body takes the ordinary Path A `g` (carrier) and the
                 * whole `s`; the dict arg (slot 0) is dropped.  Only fires on a
                 * unique, non-ambiguous resolution (see mono_specs). */
                if (fn_binding &&
                    e->as.call_.n_args >= 3) {
                    const char *rlens = NULL; unsigned long long rhash = 0;
                    bool redirect = false;
                    /* CM2: inside a consumer clone the lens param `l` is bound to
                     * ONE concrete lens; resolve `(l g s)` straight to that lens's
                     * mono body (the same target the |set|==1 VBM3 redirect picks,
                     * just chosen per-clone instead of from the global set).  The
                     * clone's `g` is already emitted by value via the linked twin
                     * (thunk_sym_override), so no box toggle is needed here. */
                    const EmitAbiSpecialization *cur =
                        ctx->current_abi_specialization;
                    if (cur && cur->is_consumer_mono &&
                        cur->consumer_lens_binding == fn_binding) {
                        rlens = cur->consumer_lens_name;
                        rhash = cur->consumer_lens_hash;
                        redirect = true;
                    } else if (mono_spec_redirect_for_binding(fn_binding, &rlens,
                                                              &rhash)) {
                        /* CM4: a GENERIC consumer's `(l g s)` redirects ONLY in
                         * its concrete ABI-spec body (where the lens-result
                         * consumer -- run-id / get-const -- is by-value).  Its
                         * generic carrier BASE body (no active spec) keeps the
                         * carrier dispatch, so a by-value point_x__mono is not
                         * baked into the polymorphic body (which would feed the
                         * carrier `run_hyid` a by-value aggregate). */
                        if (cur || !mono_spec_consumer_is_generic(fn_binding))
                            redirect = true;
                    }
                    if (redirect) {
                        uint32_t na = e->as.call_.n_args;
                        /* args = [dict, g, s]: g and s are the trailing two. */
                        char *g_str = emit_value(ctx, body, e->as.call_.args[na - 2]);
                        char *s_str = emit_value(ctx, body, e->as.call_.args[na - 1]);
                        Buf mo; buf_init(&mo);
                        emit_vl_mono_name(&mo, rlens, rhash);
                        buf_printf(&mo, "((int64_t)(intptr_t)(%s), (int64_t)(intptr_t)(%s))",
                                   g_str, s_str);
                        buf_putc(&mo, '\0');
                        char *result = strdup(mo.data);
                        buf_free(&mo);
                        free(g_str); free(s_str);
                        /* findings 16.2: same late-minted-spec gap as the
                         * consumer redirect above. */
                        note_call_ret(ctx, emit_type_c_name(ctx, e->type));
                        return result;
                    }
                }
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
                 * path emits.  See docs/archive/history/fn-first-class-float-carrier-gap.md. */
                bool typed_carrier = fn_binding && fn_binding->poly_type &&
                                     fn_binding->poly_type->kind == TY_FN;
                bool phase_f_concrete = !e->as.call_.poly_arg_mask &&
                                        (typed_carrier ||
                                         type_kind_is_poly_concrete(e->type.kind));
                for (uint32_t i = 0; i < n && phase_f_concrete && !typed_carrier; i++) {
                    if (!type_kind_is_poly_concrete(e->as.call_.args[i]->type.kind))
                        phase_f_concrete = false;
                }

                /* gcc14-int-conversion (re-dispatched method to a concrete-pointer
                 * param): when this call re-resolves to a concrete instance clone
                 * (`show` -> `__inst_Show_show_cstr`, param `const char *`), the
                 * generic carrier arg cast below would emit `(int64_t)(intptr_t)arg`
                 * into a pointer param -- a straddle (macOS clang hard error).  The
                 * re-resolved FnDef carries the CONCRETE `param_types[i]`; bridge to
                 * it when it is a pointer.  NULL for an ordinary generic call, so the
                 * int64-carrier cast is unchanged there. */
                FnDef *ba_reresolved = emit_reresolve_method_fndef(ctx, e);
                char **arg_strs = n ? (char **)malloc(n * sizeof(char *)) : NULL;
                if (n && !arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < n; i++) {
                    char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                    if (!phase_f_concrete) {
                        /* Generic carrier path: apply poly_arg_mask / needs_cast wrapping */
                        if (e->as.call_.poly_arg_mask & ARG_IDX_BIT(i)) {
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
                                const char *tgt = "int64_t";
                                if (ba_reresolved && i < ba_reresolved->n_params &&
                                    ba_reresolved->param_types) {
                                    const char *pc = emit_type_c_name(ctx,
                                        emit_resolve_type(ctx, ba_reresolved->param_types[i]));
                                    size_t pL = pc ? strlen(pc) : 0;
                                    if (pc && pL >= 1 && pc[pL - 1] == '*')
                                        tgt = pc;
                                }
                                Buf cast; buf_init(&cast);
                                buf_printf(&cast, "(%s)(intptr_t)(%s)", tgt, raw);
                                buf_putc(&cast, '\0');
                                free(raw);
                                raw = strdup(cast.data);
                                buf_free(&cast);
                            }
                        }
                    }
                    /* Slice 3 (constrained-hkt-forall codegen): a by-value
                     * aggregate container arg (`(Option int)`, a flat product)
                     * cannot ride the int64 carrier by value -- heap-box it and
                     * pass the pointer as int64.  The wrapper thunk derefs it
                     * (make_poly_wrapper sets poly_agg_arg_mask).  Carrier-
                     * compatible containers (parametric opaque / :heap ADT) are
                     * NOT byvalue aggregates, so they pass through untouched. */
                    if (!phase_f_concrete &&
                        emit_type_is_byvalue_adt(ctx, e->as.call_.args[i]->type)) {
                        char *boxed = emit_agg_box(ctx, e->as.call_.args[i]->type, raw);
                        free(raw);
                        raw = boxed;
                    }
                    /* B4 (byvalue-recursive-carrier): a single-carrier recursive
                     * wrapper (Re/Expr) arg whose VALUE is the erased int64
                     * carrier (a match binding / spec param emitted as int64_t)
                     * must be reconstructed into the by-value aggregate the thunk
                     * param expects -- the fat-closure cast keeps the aggregate
                     * param type, so the carrier ABI agrees by reinterpret.  A
                     * concrete-aggregate arg (raw c_name already the aggregate)
                     * passes through untouched. */
                    if (phase_f_concrete &&
                        emit_type_is_byval_recursive_carrier(ctx, e->as.call_.args[i]->type)) {
                        const char *raw_cn = type_c_name(e->as.call_.args[i]->type);
                        const char *agg_cn = emit_type_c_name(ctx, e->as.call_.args[i]->type);
                        if (raw_cn && agg_cn && strcmp(raw_cn, agg_cn) != 0) {
                            char *rec = emit_byval_recursive_carrier_reconstruct(
                                ctx, e->as.call_.args[i]->type, raw);
                            free(raw);
                            raw = rec;
                        }
                    }
                    /* Phase F concrete path: args used as-is, no int64_t widening. */
                    arg_strs[i] = raw;
                }

                /* closure-result-monomorphization: a monadic continuation
                 * (bind/ap over Option/Result) is boxed into tur_poly_fn_t via
                 * make_poly_wrapper, whose wrapper RETURNS the by-value carrier-
                 * ABI aggregate directly (ensure_aggregate_spill_shim explicitly
                 * excludes carrier-ABI aggregates, so no int64-boxing shim is
                 * inserted).  The by-value bind/ap `__spec` therefore invokes the
                 * continuation through the original by-value cast below
                 * (`((Option__int (*)(void*, int64_t))k.fn)(...)`), which matches
                 * the wrapper's real return ABI and crosses no carrier boundary.
                 * (An earlier revision bridged carrier->concrete here on the
                 * assumption the thunk always returns int64; that is false post
                 * construct-monomorphization and produced a wild-pointer deref
                 * once the grounding fix made the path reachable.) */
                Buf out; buf_init(&out);
                if (phase_f_concrete) {
                    /* Phase F: cast fn.fn to the concrete signature and call directly */
                    buf_printf(&out, "((%s (*)(void*", emit_type_c_name(ctx, e->type));
                    for (uint32_t i = 0; i < n; i++) {
                        /* B4 (slice 2): a wide by-value ADT arg crosses as the
                         * int64 box-pointer carrier (it cannot fit a register
                         * pair through the uniform fat-closure slot), so the cast
                         * spells the param int64_t and the thunk deref+copies. */
                        if (emit_type_is_wide_byval_adt(ctx, e->as.call_.args[i]->type))
                            buf_puts(&out, ", int64_t");
                        else
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
                /* findings 16: the concrete branch's value type is the cast the
                 * emitter just spelled; the generic carrier branch is int64_t
                 * unless a wrap below retypes it (each wrap re-notes). */
                note_call_ret(ctx, phase_f_concrete ? emit_type_c_name(ctx, e->type)
                                                    : "int64_t");
                /* Slice 3 (constrained-hkt-forall codegen): a by-value aggregate
                 * RESULT comes back through the carrier as an int64 heap-box
                 * pointer (the wrapper's carrier-spill shim boxed it) -- deref it
                 * back to the aggregate the surrounding context expects.  Only in
                 * the generic path; phase_f_concrete never carries an aggregate
                 * result (aggregates are not poly-concrete). */
                if (!phase_f_concrete && emit_type_is_byvalue_adt(ctx, e->type) &&
                    !(ctx->current_abi_specialization &&
                      ctx->current_abi_specialization->is_vl_wide_mono)) {
                    char *unboxed = emit_agg_unbox(ctx, e->type, result);
                    free(result);
                    result = unboxed;
                    note_call_ret(ctx, emit_type_c_name(ctx, e->type));   /* findings 16 */
                }
                /* VBM2b/VBM3/residual: inside a by-value monomorphized lens body
                 * the functor is spelled by value end to end -- `g : (-> A (f A))`
                 * returns its `(f A)` aggregate BY VALUE (its box flag is cleared
                 * for redirected consumers, see mono_specs), so no unbox fires. */
                /* hrt-poly-call-nonint-return-int-conversion: the generic carrier
                 * `.fn` field is typed int64_t, so a poly call whose result is a
                 * pointer-class type (cstr, ptr<T>, rc/ref, ...) yields an
                 * int64_t-typed expression that flows into a pointer C slot and
                 * trips -Wint-conversion.  Cast the int64 carrier result back to
                 * the concrete pointer type through intptr_t, mirroring the cast
                 * the phase_f_concrete branch already applies to `.fn`.  Correct
                 * on LP64 (same width); silences the warning and is portable. */
                if (!phase_f_concrete) {
                    TypeKind rk = emit_resolve_type(ctx, e->type).kind;
                    bool ptr_class = (rk == TY_CSTR || rk == TY_PTR_VOID ||
                                      rk == TY_RC || rk == TY_REF ||
                                      rk == TY_WEAK || rk == TY_REF_IMMUT ||
                                      rk == TY_REF_MUT);
                    if (ptr_class) {
                        Buf c; buf_init(&c);
                        buf_printf(&c, "((%s)(intptr_t)(%s))",
                                   emit_type_c_name(ctx, e->type), result);
                        buf_putc(&c, '\0');
                        free(result);
                        result = strdup(c.data);
                        buf_free(&c);
                        note_call_ret(ctx, emit_type_c_name(ctx, e->type));   /* findings 16 */
                    }
                }
                for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                free(arg_strs);
                free(fn_name);
                return result;
            }

            if (fn_binding->closure_fn_binding) {
                /* This is a closure - emit call to thunk function with closure value as first arg */
                Binding *thunk_binding = fn_binding->closure_fn_binding;
                /* constrained-instance-element-dispatch-in-closures: a per-spec
                 * clone of a fold/accumulator closure (`__fn_N__spec__...`) must be
                 * targeted instead of the shared base `__fn_N` (which bakes the
                 * carrier representative element instance) at two call sites:
                 *   1. the closure's own recursive self-call, emitted while the
                 *      clone body is active (fn_name_override set, current spec
                 *      bound to this very closure binding); and
                 *   2. the ENCLOSING instance-method spec's direct invocation of
                 *      the closure (`(go 0 0)`), emitted while the outer spec is
                 *      active and links to the clone via inner_closure_spec_idx.
                 * Mirrors the EX_CLOSURE construction's thunk_sym_override. */
                char *thunk_name = NULL;
                if (ctx->fn_name_override && ctx->current_abi_specialization &&
                    ctx->current_abi_specialization->binding == thunk_binding) {
                    thunk_name = strdup(ctx->fn_name_override);
                } else if (ctx->current_abi_specialization) {
                    /* struct-of-closures monomorphization: resolve across the
                     * primary + extra inner-closure links (see EX_CLOSURE emit). */
                    const EmitAbiSpecialization *_isp =
                        emit_inner_closure_spec_for_binding(
                            ctx, ctx->current_abi_specialization, thunk_binding);
                    if (_isp && _isp->clone_name)
                        thunk_name = strdup(_isp->clone_name);
                }
                /* generic-closure-return-type-app (Defect B, site 3): the
                 * invoke is OUTSIDE any spec (e.g. in main), but the closure
                 * value came from a call the emitter resolved to a spec clone
                 * of the producing generic -- `pure__spec__...` -- whose
                 * EX_CLOSURE stores the inner-body CLONE's thunk.  Direct-
                 * calling the shared base thunk here would re-enter the
                 * un-monomorphized body (whose `ctor_Cons` is never emitted:
                 * a link error past tur check).  Resolve the outer spec the
                 * head init actually called via the specialized-call registry,
                 * then follow its inner-closure link -- the same pairing rule
                 * as sites 1 and 2, keyed by the head instead of the ambient
                 * spec. */
                if (!thunk_name && fn_binding->closure_head_init) {
                    const Expr *src = fn_binding->closure_head_init;
                    while (src && src->kind == EX_ASCRIBE)
                        src = src->as.ascribe_.inner;
                    const char *outer_clone = NULL;
                    if (src && src->kind == EX_CALL) {
                        for (uint32_t si = 0; si < ctx->n_specialized_calls; si++) {
                            if (ctx->specialized_call_exprs[si] == src) {
                                outer_clone = ctx->specialized_call_names[si];
                                break;
                            }
                        }
                    }
                    if (outer_clone) {
                        for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
                            const EmitAbiSpecialization *osp =
                                &ctx->abi_specializations[si];
                            if (!osp->clone_name ||
                                strcmp(osp->clone_name, outer_clone) != 0)
                                continue;
                            const EmitAbiSpecialization *_isp =
                                emit_inner_closure_spec_for_binding(
                                    ctx, osp, thunk_binding);
                            if (_isp && _isp->clone_name)
                                thunk_name = strdup(_isp->clone_name);
                            break;
                        }
                    }
                }
                if (!thunk_name) thunk_name = raw_name_for_binding(thunk_binding);
                if (!thunk_name) { fprintf(stderr, "tur: oom\n"); abort(); }

                /* Closure value is the env struct variable.
                 *
                 * S5 (letrec self-recursive closure): when this call targets the
                 * closure whose own lifted body is currently being emitted -- i.e.
                 * a `letrec`-bound `go` calling `go` -- the closure box is NOT the
                 * outer-scope binding name (which is out of scope inside the lifted
                 * thunk), it is the current env pointer the thunk already received
                 * as its first parameter.  Detect the self-call by matching the
                 * target thunk against the closure being emitted and pass the raw
                 * env param (already a `void *`) instead. */
                char *closure_val;
                if (ctx->closure && ctx->closure->fn &&
                    ctx->closure->fn->binding == thunk_binding &&
                    ctx->closure->fn->n_params > 0) {
                    closure_val = raw_name_for_binding(ctx->closure->fn->params[0]);
                } else {
                    closure_val = name_for_binding(ctx, fn_binding);
                }

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
                    Type formal_full_type = {0};
                    bool have_formal_full = false;
                    if (thunk_binding->type.kind == TY_FN &&
                        thunk_binding->type.as.fn.arg_full_types &&
                        thunk_binding->type.as.fn.arg_full_types[i + 1]) {
                        formal_full_type = *thunk_binding->type.as.fn.arg_full_types[i + 1];
                        have_formal_full = true;
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
                         formal == TY_REF || formal == TY_WEAK ||
                         (have_formal_full && strchr(type_c_name(formal_full_type), '*') != NULL));
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
                        raw = emit_carrier_reinterpret(ctx, raw, NULL, "poly-call-arg");
                    } else if (formal_is_ptr && arg_is_int64_carrier) {
                        /* Reverse direction: formal slot is a pointer,
                         * but the arg's C value is the int64_t carrier (because
                         * the binding holds a function-typed value via the
                         * fn-carrier ABI, or because it was annotated ^fat).
                         * Bridge through intptr_t so the pointer slot accepts it. */
                        Buf cast; buf_init(&cast);
                        const char *f_cname = have_formal_full ? type_c_name(formal_full_type) : "void *";
                        buf_printf(&cast, "(%s)(intptr_t)(%s)", f_cname, raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    } else if (emit_type_is_wide_byval_adt(ctx,
                                   e->as.call_.args[i]->type)) {
                        /* B4 (letrec self-recursive carrier struct return): a WIDE
                         * by-value ADT arg (a lowered :copy struct > 8 bytes, e.g.
                         * `Box{lo hi}`) crosses into the thunk's closure param as
                         * the int64 box-pointer carrier -- emit_fns.c /
                         * emit_module.c declare such a param `int64_t
                         * __tur_b4box_<name>` and the thunk deref+copies it back.
                         * Heap-box the aggregate here so the call matches that
                         * carrier ABI; passing the raw `tur_adt_Box` into the
                         * int64_t slot is an incompatible-type cc error.  Affects
                         * both the closure's own recursive self-call and the
                         * enclosing letrec's direct invocation. */
                        raw = emit_carrier_bridge_escaping(ctx, body, raw,
                                  CK_CONCRETE, CK_CARRIER,
                                  emit_resolve_type(ctx, e->as.call_.args[i]->type));
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
                    note_call_ret(ctx, type_c_name(e->type));   /* findings 16 */
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
                    note_call_ret(ctx, ret_c);   /* findings 16 */
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
                 * fat box's address as code and jump to garbage.
                 *
                 * fn-value-fat-normalization stage 1: a NOMINAL thin TY_FN
                 * parameter joins the fat protocol -- every value flowing into
                 * one is a fat handle (the elab call-site shim guarantees it,
                 * keyed on the SAME fn_param_type_is_fat_normalized predicate),
                 * so the invoke dispatches fat uniformly.  This is what turns
                 * "capturing closure into a non-carrier fn param" from a SIGSEGV
                 * (poly-result-hof-capturing-closure-sigbus) into a working
                 * call.  Scoped to parameters; let-bound TY_FN locals keep
                 * their existing paths. */
                if (fn_binding->is_fat || fn_binding->type.as.fn.boxed ||
                    (fn_binding->is_param &&
                     fn_param_type_is_fat_normalized(&fn_binding->type))) {
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
                    /* CM4 (composition): inside a dict-clone body (MB2.5 carrier --
                     * it dispatches the functor method through the runtime dict as
                     * int64), a functor-wrapping closure `g` (`(-> A (f A))`) is
                     * passed as the carrier: the caller's adapter BOXES its wide
                     * `(f A)` result into int64.  A composed wide lens (`line-a-x`)
                     * lowers its nested `line-a` through such a dict-clone, so
                     * `(g (.a s))` resolves to the by-value aggregate under the
                     * concrete `f := Identity`, but the actual adapter returns the
                     * int64 carrier and the receiver feeds the carrier `fmap` slot.
                     * Force the fat-dispatch return to the int64 carrier so both
                     * agree.  A carrier-compatible functor result is already int64,
                     * so this only bites the by-value aggregate case. */
                    if (ctx->current_abi_specialization &&
                        ctx->current_abi_specialization->fn &&
                        ctx->current_abi_specialization->fn->n_dict_clone > 0 &&
                        type_kind_is_aggregate(disp_result.kind) &&
                        strcmp(type_c_name(disp_result), "int64_t") != 0)
                        disp_result = emit_type_from_kind(TY_INT);
                    if (type_uses_carrier_abi(disp_result) &&
                        fn_binding->type.kind == TY_FN &&
                        fn_binding->type.as.fn.result_full_type) {
                        Type rfull_resolved = emit_resolve_type(ctx,
                            *fn_binding->type.as.fn.result_full_type);
                        if (!type_uses_carrier_abi(rfull_resolved))
                            disp_result = rfull_resolved;
                    }
                    /* lens-composition-codegen-blockers (Blocker 2b): the recovery
                     * above only fires when e->type resolved to a carrier-ABI
                     * app/adt.  A WIDE by-value aggregate result -- `(. l1 get)`
                     * returning `Person` -- erased all the way to the bare int64
                     * carrier (TY_INT), which is NOT carrier-ABI per the predicate,
                     * so the fat thunk cast's return slot stayed int64 while the
                     * actual by-value fatshim returns `tur_adt_Person` by value.
                     * Recover it from the callee's resolved result_full_type when it
                     * is a concrete wide by-value ADT (matches the temp-var recovery
                     * in fn_body_tail_byvalue_carrier_type).  Skipped in a dict-clone
                     * body, where CM4 keeps the carrier. */
                    else if (disp_result.kind == TY_INT &&
                             !(ctx->current_abi_specialization &&
                               ctx->current_abi_specialization->fn &&
                               ctx->current_abi_specialization->fn->n_dict_clone > 0) &&
                             fn_binding->type.kind == TY_FN &&
                             fn_binding->type.as.fn.result_full_type) {
                        Type rfull_resolved = emit_resolve_type(ctx,
                            *fn_binding->type.as.fn.result_full_type);
                        if (type_is_wide_byval_adt(rfull_resolved))
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
                              arg_types[i].as.fn.boxed) ||
                             (arg_types[i].kind == TY_STRUCT &&
                              strchr(emit_type_c_name(ctx, arg_types[i]), '*') != NULL));
                        bool var_is_int64_carrier =
                            arg && arg->kind == EX_VAR && arg->as.var.binding &&
                            arg->as.var.binding->is_fat;
                        /* hkt-cata-function-arg: the mirror of the bridge above.
                         * When the carrier's function-typed argument slot lowers
                         * to the int64_t carrier (a non-boxed declared fn type)
                         * but the argument VALUE is a fat closure box -- a freshly
                         * auto-shimmed bare fn (EX_FN_TO_FAT), a capturing closure
                         * (EX_CLOSURE), or any :ptr<void>/boxed-fn value -- it is
                         * emitted as a `void *`.  Passing it straight into the
                         * int64_t slot trips -Wint-conversion; coerce it through
                         * (int64_t)(intptr_t) so the box pointer lands in the
                         * carrier slot cleanly. */
                        bool arg_is_fat_box =
                            arg && (arg->kind == EX_FN_TO_FAT ||
                                    arg->kind == EX_CLOSURE ||
                                    arg->type.kind == TY_PTR_VOID ||
                                    (arg->type.kind == TY_FN &&
                                     arg->type.as.fn.boxed));
                        if (slot_is_ptr && var_is_int64_carrier) {
                            buf_printf(&out, ", (%s)(intptr_t)(%s)", emit_type_c_name(ctx, arg_types[i]), arg_strs[i]);
                        } else if (!slot_is_ptr && arg_is_fat_box) {
                            buf_printf(&out, ", (int64_t)(intptr_t)(%s)", arg_strs[i]);
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
                    note_call_ret(ctx, ret_c);   /* findings 16 */
                    return result;
                }
                char *fn_ptr = name_for_binding(ctx, fn_binding);
                uint32_t n = e->as.call_.n_args;
                /* poly-hof-constrained-result-baked-carrier (symmetric to the
                 * arg-side fix below): resolve the RESULT type through the active
                 * ABI spec so a monomorphized fn-value call whose result tyvar
                 * binds to a non-int64 type (e.g. an HKT `(fn [a] b)` mapper with
                 * b -> cstr/float/struct) casts the fn pointer to the correct
                 * return type instead of the int64 carrier.  A no-op outside a
                 * spec / for already-concrete results. */
                const char *ret_c = emit_type_c_name(ctx, e->type);
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
                    note_call_ret(ctx, ret_c);   /* findings 16 */
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
                        /* poly-hof-constrained-arg-baked-carrier: resolve the arg
                         * type through the active ABI spec so a monomorphized
                         * by-value arg (`a : A` -> Box) types the cast signature
                         * concretely instead of as the int64 carrier. */
                        Type ait = e->as.call_.args[i]->type;
                        Type aspec_t;
                        if (emit_var_spec_arg_type(ctx, e->as.call_.args[i], &aspec_t))
                            ait = aspec_t;
                        buf_puts(&out, type_c_name(ait));
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
                    note_call_ret(ctx, ret_c);   /* findings 16 */
                    return result;
                }
            }

            /* Phase G0: 0-arg constructor call — emit ctor_Name() */
            if (fn_binding->type.kind == TY_ADT) {
                char *_mc = mangle_field_name(fn_binding->name->name);
                /* TS4P2: use per-instance ctor if the call result is a concrete ADT app.
                 * Resolve the construct's type through the active ABI spec first
                 * (M7 by-value HKT, gap G6): inside `__inst_Functor_fmap_T__spec__*`
                 * the result `(ReF b)` grounds to `(ReF bool)` only via the spec's
                 * element bindings -- without this the carrier `ctor_EmptyF` is
                 * emitted where the consumer reads the by-value `tur_adt_ReF__bool`. */
                Type rty = emit_resolve_type(ctx, e->type);
                char *suffix = (rty.kind == TY_APP)
                    ? type_adt_app_ctor_suffix(rty) : NULL;
                if (!suffix)
                    suffix = emit_hkt_spec_ctor_suffix(ctx, e);
                Buf out; buf_init(&out);
                if (suffix) {
                    buf_printf(&out, "ctor_%s%s()", _mc, suffix);
                    free(suffix);
                } else {
                    buf_printf(&out, "ctor_%s()", _mc);
                    /* dead-base-thunk-chain-references-undefined-ctor: same
                     * dead-chain shape as the n-arg branch below -- a
                     * suffix-less 0-arg ctor of a heap parametric ADT names a
                     * base symbol that is never defined.  Register the trap
                     * stand-in so the reference compiles and links. */
                    if (e->as.call_.ctor && e->as.call_.ctor->adt &&
                        e->as.call_.ctor->adt->n_type_params > 0 &&
                        e->as.call_.ctor->adt->is_heap) {
                        emit_note_dead_base_ctor(ctx, _mc, 0);
                    }
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
             * TS4P2: use per-instance ctor when the call result is a concrete ADT app.
             * CONV-S1 seam 4: a typeclass instance method (`__inst_*`) is a dispatch
             * function, NEVER an ADT constructor -- exclude it explicitly.  At
             * default an instance method returning a (parametric) struct has
             * result_kind TY_STRUCT, so this gate's `result_kind == TY_ADT` already
             * skipped it; under the defstruct-as-defadt lowering that struct is now
             * an ADT (`Serializable [Pair]`'s `deserialize : Bytes -> Pair`), and a
             * bare PARAMETRIC ADT return keeps result_full_type NULL (the
             * non-parametric by-value branch in elab_typeclasses.c does not fire),
             * so without this guard the call mis-emits as `ctor___inst_...` -- an
             * undefined symbol (link error). */
            bool fn_is_inst_method = fn_binding->name && fn_binding->name->name &&
                strncmp(fn_binding->name->name, "__inst_", 7) == 0;
            if (fn_binding->type.kind == TY_FN &&
                fn_binding->type.as.fn.result_kind == TY_ADT &&
                !fn_binding->type.as.fn.result_full_type &&
                !fn_is_inst_method) {
                /* TS4P2: choose per-instance ctor name if the result is a concrete ADT app.
                 * Resolve through the active ABI spec first (M7 by-value HKT, gap
                 * G6) so `(AltF (g x) (g y)) : (ReF b)` grounds to `(ReF bool)` and
                 * emits `ctor_AltF__bool`, matching the by-value layout the
                 * consumer reads instead of the int64-carrier `ctor_AltF`. */
                Type rty = emit_resolve_type(ctx, e->type);
                /* SC7 (lowered transparent int-newtype): under the
                 * defstruct->defadt lowering a phantom int-newtype
                 * (`(defstruct Schema [A] (raw :int))`) is a single-variant
                 * record ADT whose runtime representation is still its bare
                 * int64 payload (type_c_name -> "int64_t").  Its constructor is
                 * the identity on that payload -- emit the single arg cast to
                 * int64 directly, exactly as EX_MAKE_STRUCT does, so we never
                 * call (or need) the aggregate `ctor_Schema__int` and the result
                 * matches the int64 carrier every consumer expects. */
                if (e->as.call_.n_args == 1 &&
                    type_is_transparent_int_newtype(rty)) {
                    char *fv = emit_value(ctx, body, e->as.call_.args[0]);
                    Buf id; buf_init(&id);
                    buf_printf(&id, "(int64_t)(%s)", fv);
                    buf_putc(&id, '\0');
                    free(fv);
                    char *result = strdup(id.data);
                    buf_free(&id);
                    return result;
                }
                /* CONV-S1: only trust the receiver-type suffix when the app is
                 * fully concrete.  A lowered record-ADT constructor body
                 * (`none`'s `(Option false (default-of A))`) carries an erased
                 * element (`(Option <NULL-def struct>)`), for which
                 * type_adt_app_ctor_suffix yields the wrong `__struct` carrier
                 * suffix; fall back to the active spec's result family so the
                 * `none__spec__tur_adt_Option__int` body emits `ctor_Option__int`. */
                char *suffix = (rty.kind == TY_APP && type_app_is_concrete_adt(&rty))
                    ? type_adt_app_ctor_suffix(rty) : NULL;
                if (!suffix)
                    suffix = emit_hkt_spec_ctor_suffix(ctx, e);
                if (!suffix && rty.kind == TY_APP)
                    suffix = type_adt_app_ctor_suffix(rty);  /* last resort: prior behaviour */
                /* macos-int-conversion-carrier-pointer-straddles (case A):
                 * the emitted C name of the ctor being called, for the
                 * signature-table lookup at the end of the arg loop. */
                char *ctor_cname;
                {
                    char *_lmc = mangle_field_name(fn_binding->name->name);
                    Buf cnb; buf_init(&cnb);
                    buf_printf(&cnb, "ctor_%s%s", _lmc, suffix ? suffix : "");
                    buf_putc(&cnb, '\0');
                    ctor_cname = strdup(cnb.data);
                    buf_free(&cnb);
                    free(_lmc);
                }
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
                    /* CONV-S1 (seam 2): a `(default-of T)` argument to a
                     * monomorphised ctor whose field type is heterogeneous --
                     * `(ok v) : (Result int cstr)` passes `(default-of B)` for the
                     * `const char *` err field -- emits `(int64_t){0}` (the
                     * collapsed carrier default) where the by-value monomorph ctor
                     * `ctor_Result__int__cstr` expects the field's concrete C type.
                     * (The struct path elides the slot via C99 zero-init; the
                     * ctor-call path passes it explicitly.)  Recompute the default
                     * in the field's concrete type so it lands as `(const char *){0}`.
                     * Resolve the field type from the active spec's concrete result
                     * (or rty) by substituting the app args for the ctor field's
                     * tyvar name. */
                    if (suffix && arg && arg->kind == EX_DEFAULT_OF &&
                        e->as.call_.ctor && i < e->as.call_.ctor->n_fields &&
                        e->as.call_.ctor->fields[i].full_type) {
                        const CtorDef *ct = e->as.call_.ctor;
                        AdtDef *cdef = ct->adt;
                        Type concrete = rty;
                        if (ctx->current_abi_specialization) {
                            Type sr = emit_resolve_type(ctx,
                                ctx->current_abi_specialization->result_type);
                            if (type_adt_app_def(&sr) == cdef) concrete = sr;
                        }
                        if (concrete.kind == TY_APP && cdef) {
                            Type aargs[16]; uint8_t na = 0;
                            const Type *cur = &concrete; Type raw[16]; uint8_t nr = 0;
                            while (cur && cur->kind == TY_APP && nr < 16) {
                                if (cur->as.app.arg) raw[nr++] = *cur->as.app.arg;
                                cur = cur->as.app.fn; }
                            for (uint8_t k = 0; k < nr; k++) aargs[k] = raw[nr - 1 - k];
                            na = nr;
                            const Type *ft = ct->fields[i].full_type;
                            Type fty = *ft;
                            if (ft->kind == TY_TYVAR && ft->as.tyvar_.name) {
                                for (uint8_t k = 0; k < cdef->n_type_params && k < na; k++)
                                    if (cdef->type_params[k] &&
                                        strcmp(cdef->type_params[k], ft->as.tyvar_.name) == 0) {
                                        fty = aargs[k]; break; }
                            }
                            const char *fc = emit_type_c_name(ctx, fty);
                            /* CONV-S1 seam 4 (typedef ordering): a Result/Option
                             * monomorph's value-struct field is stored as a heap
                             * pointer `T *` (adt_field_c_type), so the unused-slot
                             * default for the other variant (`err`'s ok_val,
                             * `none`'s value) must be the NULL pointer `(T *){0}`,
                             * not the aggregate `(T){0}` -- the slot type is `T *`. */
                            Type rfty = emit_resolve_type(ctx, fty);
                            /* structdef-retirement slice 1 / DS-C: a by-value
                             * record-ADT field rides as `tur_adt_T *`, so the
                             * other variant's unused slot is `(T *){0}`.  The
                             * former TY_STRUCT disjunct is dead. */
                            bool ptr_slot = cdef && cdef->name &&
                                (strcmp(cdef->name, "Result") == 0 ||
                                 strcmp(cdef->name, "Option") == 0) &&
                                 (rfty.kind == TY_ADT && rfty.as.adt_.def &&
                                  !rfty.as.adt_.def->is_heap &&
                                  rfty.as.adt_.def->n_type_params == 0 &&
                                  adt_is_byvalue_product(rfty.as.adt_.def));
                            /* A function-typed element rides as the opaque
                             * `void *` carrier (the `__opaque` monomorph field),
                             * but type_c_name(TY_FN) leaks the fn's result type
                             * (`int64_t`/`double`), so the other-variant default
                             * would land an int/double literal in a `void *`
                             * slot (-Wint-conversion).  Emit the NULL pointer. */
                            if (rfty.kind == TY_FN) { fc = "void *"; }
                            /* S1/findings 16.4: a POINTER zero is scalar --
                             * `(const char *){0}` is a compound literal c2mir
                             * rejects.  Route through emit_c_zero_of, which
                             * spells scalars `((T)0)` and aggregates `(T){0}`. */
                            if (fc && strcmp(fc, "void *") == 0) {
                                free(arg_strs[i]);
                                arg_strs[i] = strdup("((void *)0)");
                            } else if (fc && strcmp(fc, "int64_t") != 0) {
                                if (ptr_slot) {
                                    Buf db; buf_init(&db);
                                    buf_printf(&db, "((%s *)0)", fc);
                                    buf_putc(&db, '\0');
                                    free(arg_strs[i]);
                                    arg_strs[i] = strdup(db.data);
                                    buf_free(&db);
                                } else {
                                    free(arg_strs[i]);
                                    arg_strs[i] = emit_c_zero_of(fc);
                                }
                            }
                        }
                    }
                    /* hkt-cata-function-carrier: an EX_FN_TO_FAT shim (a thin fn
                     * boxed into a fat closure for a parametric ADT field, see
                     * elab_call.c) emits a `void *` box.  The constructor lowers
                     * the field to the int64 carrier, so cast the box to int64_t
                     * (a pure reinterpret) -- otherwise the void* is passed to
                     * the int64_t ctor param with a -Wint-conversion warning. */
                    if (arg && arg->kind == EX_FN_TO_FAT) {
                        arg_strs[i] = emit_carrier_reinterpret(ctx, arg_strs[i], NULL, "ctor-field-fnbox");
                    }
                    /* CONV-S1 (slice 4): when the owning ctor is itself a by-value
                     * product and this field is a by-value aggregate, the field is
                     * stored INLINE -- the ctor param is the aggregate type, so the
                     * by-value arg is passed straight through with no box. */
                    bool field_inline = false;
                    bool field_is_fn = false;
                    {
                        Type rt = emit_resolve_type(ctx, e->type);
                        if (rt.kind == TY_ADT && rt.as.adt_.def &&
                            adt_is_byvalue_product(rt.as.adt_.def)) {
                            const AdtDef *od = rt.as.adt_.def;
                            /* SR1: a by-value owner may now be MULTI-variant, so
                             * the field being stored belongs to the constructor
                             * being applied -- reading `ctors[0]` would answer for
                             * the wrong arm (or index out of it).  Fall back to
                             * `ctors[0]` only for the single-variant case this was
                             * written for, where the two are the same ctor. */
                            const CtorDef *oc =
                                (e->as.call_.ctor && e->as.call_.ctor->adt == od)
                                    ? e->as.call_.ctor
                                    : (od->n_ctors == 1 ? od->ctors[0] : NULL);
                            if (oc && i < oc->n_fields) {
                                field_inline = adt_field_is_inline_byval(&oc->fields[i]);
                                field_is_fn = oc->fields[i].kind == TY_FN;
                            }
                        }
                    }
                    /* CONV-S1 (slice 6): a by-value product's ctor param for an
                     * `fn` field is the int64 carrier, but the arg is a raw
                     * function pointer -- cast it through (intptr_t) so the
                     * function-pointer -> int64 conversion is explicit (a struct
                     * literal does the same `(int64_t)(intptr_t)` on its fn field),
                     * otherwise clang emits a -Wint-conversion note. */
                    if (!suffix && field_is_fn && arg &&
                        arg->kind != EX_FN_TO_FAT) {
                        arg_strs[i] = emit_carrier_reinterpret(ctx, arg_strs[i], NULL, "ctor-field-fnptr");
                    }
                    /* EF-2: a handler-typed ctor field rides the int64 carrier slot
                     * (like an fn field -- see the CONV-S1 slice-6 cast above), but a
                     * handler VALUE is a `tur_handler_table_t *` pointer.  Cast it
                     * through intptr_t so the pointer -> int64 conversion is explicit,
                     * else clang trips -Wint-conversion at the ctor call.  Keyed on
                     * the ctor field's declared kind so it fires for both a by-value
                     * product (int64 field slot) and a carrier ctor. */
                    bool field_is_handler = e->as.call_.ctor &&
                        i < e->as.call_.ctor->n_fields &&
                        e->as.call_.ctor->fields[i].kind == TY_HANDLER;
                    if (!suffix && field_is_handler && arg) {
                        arg_strs[i] = emit_carrier_reinterpret(ctx, arg_strs[i], NULL, "ctor-field-byval");
                    }
                    /* EF-4: a session/role-typed ctor field also rides the int64
                     * carrier, but a `(Session P)` / `(Role G R)` VALUE is a
                     * `TurChannel *` / role pointer.  Cast through intptr_t so the
                     * pointer -> int64 store is explicit, mirroring the handler
                     * cast above; else clang trips -Wint-conversion at the ctor
                     * call.  Keyed on the ctor field's declared kind. */
                    bool field_is_session = e->as.call_.ctor &&
                        i < e->as.call_.ctor->n_fields &&
                        (e->as.call_.ctor->fields[i].kind == TY_SESSION ||
                         e->as.call_.ctor->fields[i].kind == TY_ROLE);
                    if (!suffix && field_is_session && arg) {
                        arg_strs[i] = emit_carrier_reinterpret(ctx, arg_strs[i], NULL, "ctor-field-carrier");
                    }
                    /* CONV-S1/B3: a by-value ADT argument flowing into a carrier
                     * constructor's int64 field slot must be boxed (heap copy ->
                     * int64) -- the field-store crossing.  Only for the plain
                     * (non-suffixed) carrier ctor; a monomorphised per-instance
                     * ctor (suffix != NULL, the M7 HKT path) takes the concrete
                     * type directly and is B4's concern.  An inline by-value field
                     * (slice 4) takes the aggregate directly, so it skips the box. */
                    if (!suffix && !field_inline && arg &&
                        emit_type_is_byvalue_adt(ctx, arg->type)) {
                        /* nested-construct-byvalue (Gap #4): when this construct is
                         * a spec body (`ok__spec__..._Option__cstr`) the boxed arg
                         * is the spec's param `x`, whose static type collapsed the
                         * element to the carrier representative (`Option__int`).
                         * Box at the spec's concrete arg type (`Option__cstr`) so
                         * the heap copy width and the `*tmp = (x)` assignment agree
                         * with the param's C type. */
                        Type box_ty = arg->type;
                        Type spec_ty;
                        if (emit_var_spec_arg_type(ctx, arg, &spec_ty))
                            box_ty = spec_ty;
                        const char *cn = type_c_name(emit_resolve_type(ctx, box_ty));
                        char *tmp = fresh_tmp(ctx);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                                   cn, tmp, cn, cn);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "*%s = (%s);\n", tmp, arg_strs[i]);
                        Buf c; buf_init(&c);
                        buf_printf(&c, "(int64_t)(intptr_t)%s", tmp);
                        buf_putc(&c, '\0');
                        free(arg_strs[i]);
                        arg_strs[i] = strdup(c.data);
                        buf_free(&c);
                        free(tmp);
                    }
                    /* CONV-S1 (slice 8): a :heap struct argument or other pointer-like/function-pointer
                     * argument is a pointer, but a constructor stores it in the int64
                     * carrier field slot.  Cast the pointer to int64 explicitly
                     * -- the same coercion the fn-field path does above, and the
                     * one a struct literal applies to its :heap field -- otherwise
                     * clang emits a -Wint-conversion warning at the ctor call.
                     * We only apply the cast for general pointers when the destination
                     * field is indeed a carrier-erased generic slot (field_is_carrier == true);
                     * concrete fields expecting typed pointers should receive them directly. */
                    bool field_is_carrier = false;
                    if (e->as.call_.ctor && i < e->as.call_.ctor->n_fields) {
                        const Type *fft = e->as.call_.ctor->fields[i].full_type;
                        field_is_carrier = fft && fft->kind == TY_TYVAR;
                        /* closure-in-defdata-field: an OPAQUE field is a carrier
                         * slot too.  An opaque def "has NO constructors and NO
                         * fields; it is a named int64_t carrier" (types.h:353) --
                         * its declared base is erased -- so a `(defopaque Th
                         * :ptr<void>)` field lowers to int64_t exactly like a
                         * tyvar one, and a pointer stored into it is the same
                         * straddle.  Without this an ADT holding a callback (the
                         * only way to store one -- a `:fn` field segfaults)
                         * emits C that warns, and tests/run.sh's cc-warning gate
                         * rejects it.  See
                         * docs/reported/closure-in-defdata-field.md. */
                        if (!field_is_carrier && fft) {
                            Type fr = emit_resolve_type(ctx, *fft);
                            field_is_carrier = fr.kind == TY_ADT &&
                                               fr.as.adt_.def &&
                                               fr.as.adt_.def->is_opaque;
                        }
                        /* A BOXED concrete `(fn ...)` field is a carrier slot
                         * too: resolve_ctor_field boxes fn fields of arity
                         * <= 4 (storage stays the int64 carrier; only the
                         * dispatch changes), and the stored value is then a
                         * fat pointer -- an env block for a capturing
                         * closure, an EX_FN_TO_FAT shim box for a thin one.
                         * Same straddle, same missing cast, same
                         * -Wint-conversion at the ctor call that the
                         * cc-warning gate rejects. */
                        if (!field_is_carrier && fft &&
                            fft->kind == TY_FN && fft->as.fn.boxed &&
                            /* The fn-field path above may have cast this arg
                             * already; a second wrap is redundant (and churns
                             * two snapshots for nothing). */
                            strncmp(arg_strs[i], "(int64_t)(intptr_t)", 19) != 0)
                            field_is_carrier = true;
                        /* A THIN fn field (bare `:fn` -- no signature, so
                         * full_type is NULL -- or an unboxed >4-arity
                         * signature) is an int64 slot receiving a bare
                         * function pointer: a captureless lambda lifts to a
                         * top-level `__fn_N` and lands here as
                         * `int64_t (*)()` into `int64_t`, the case-2 warning
                         * of closure-in-defdata-field.  (A CAPTURING closure
                         * into such a field is rejected at elaboration, so by
                         * here the value is a code pointer and the cast is
                         * honest.) */
                        if (!field_is_carrier &&
                            e->as.call_.ctor->fields[i].kind == TY_FN &&
                            (!fft || (fft->kind == TY_FN && !fft->as.fn.boxed)) &&
                            strncmp(arg_strs[i], "(int64_t)(intptr_t)", 19) != 0)
                            field_is_carrier = true;
                    }
                    Type resolved_arg_type = emit_resolve_type(ctx, arg->type);
                    TypeKind rk = resolved_arg_type.kind;
                    bool is_ptr_like = (rk == TY_PTR_VOID ||
                                        rk == TY_RC ||
                                        rk == TY_REF ||
                                        rk == TY_WEAK ||
                                        rk == TY_CSTR ||
                                        rk == TY_REF_IMMUT ||
                                        rk == TY_REF_MUT ||
                                        rk == TY_FN ||
                                        rk == TY_CONT ||
                                        rk == TY_CLONEABLE_CONT ||
                                        rk == TY_FORALL);
                    /* A closure argument is pointer-like whatever its TYPE says.
                     * Ascribed to an opaque it resolves to that opaque (a
                     * carrier), but it still LOWERS to a pointer -- a lifted
                     * `__fn_N` when it captures nothing, a `void *` env when it
                     * does -- so the straddle is real and only the expression
                     * form reveals it. */
                    if (!is_ptr_like) {
                        const Expr *ac = arg;
                        while (ac && ac->kind == EX_ASCRIBE)
                            ac = ac->as.ascribe_.inner;
                        if (ac && ac->kind == EX_CLOSURE) is_ptr_like = true;
                    }
                    if (!suffix && !field_inline && arg &&
                        (type_is_heap_struct(resolved_arg_type) ||
                         type_is_heap_adt(resolved_arg_type) ||
                         (field_is_carrier && is_ptr_like))) {
                        arg_strs[i] = emit_carrier_reinterpret(ctx, arg_strs[i], NULL, "ctor-field-heap");
                    }
                    /* gcc14-int-conversion (docs/archive/history/codegen-gcc14-permerrors.md):
                     * a CONCRETE (non-carrier) rc<T>/weak<T> field lowers to
                     * `RcControlBlock *`, but its argument is frequently the int64
                     * carrier -- an `rc<T>` function parameter's C type is int64_t,
                     * so `ctor_Own(ir, ...)` hands an int64 into the
                     * `RcControlBlock *` field slot.  That is a -Wint-conversion,
                     * promoted to a hard error under GCC >= 14.  Cast through
                     * (RcControlBlock *)(intptr_t): value-preserving in both
                     * directions -- an int64 carrier becomes the pointer it already
                     * encodes, and an already-typed `RcControlBlock *` arg
                     * round-trips to itself.  Skipped for a carrier field (handled
                     * by the pointer->int64 block just above). */
                    if (!suffix && !field_inline && arg && !field_is_carrier &&
                        e->as.call_.ctor && i < e->as.call_.ctor->n_fields &&
                        (e->as.call_.ctor->fields[i].kind == TY_RC ||
                         e->as.call_.ctor->fields[i].kind == TY_WEAK)) {
                        Buf c; buf_init(&c);
                        buf_printf(&c, "(RcControlBlock *)(intptr_t)(%s)", arg_strs[i]);
                        buf_putc(&c, '\0');
                        free(arg_strs[i]);
                        arg_strs[i] = strdup(c.data);
                        buf_free(&c);
                    }
                    /* gcc14-int-conversion: a fn-typed ctor field is stored in the
                     * int64 carrier slot -- BOTH in a carrier ctor and in a
                     * monomorph one (`ctor_Endo__int`'s param is int64_t) -- so a
                     * fn-pointer argument must be cast to int64, else it "makes
                     * integer from pointer" (a hard error under GCC >= 14).  Not
                     * gated on `suffix` (the monomorph path is exactly where this
                     * bites), and gated on the arg being an actual fn/ptr value so
                     * an already-int64 carrier arg is left untouched.  A tyvar
                     * carrier fn field is handled by the pointer->int64 block above
                     * (field_is_carrier), so restrict to concrete TY_FN fields. */
                    if (suffix && !field_inline && arg && !field_is_carrier &&
                        e->as.call_.ctor && i < e->as.call_.ctor->n_fields &&
                        e->as.call_.ctor->fields[i].kind == TY_FN) {
                        Type rat = emit_resolve_type(ctx, arg->type);
                        if (rat.kind == TY_FN || rat.kind == TY_PTR_VOID) {
                            /* The monomorph ctor's fn-field PARAM C type is
                             * adt_field_c_type(def, fld, args): a BOXED fn field
                             * lowers to `void *` (`ctor_Lens__Person__cstr(void *,
                             * void *)`), a carrier fn field to `int64_t`
                             * (`ctor_Endo__int(int64_t)`).  Casting a fn pointer to
                             * `(int64_t)(intptr_t)` and passing it into a `void *`
                             * param is a -Wint-conversion straddle (macOS clang hard
                             * error).  Bridge to the field's ACTUAL C type when that
                             * is a pointer; else keep the int64 carrier cast. */
                            Type fty = adt_field_type_for_app(
                                &rty, &e->as.call_.ctor->fields[i]);
                            /* rty is not always a clean concrete `(Lens Point int)`
                             * app at this seam (the ctor suffix may come from the
                             * active spec, not rty), so a UNKNOWN resolution falls
                             * back to the ABI spec's concrete result family -- the
                             * same receiver the EX_DEFAULT_OF block uses above. */
                            if (fty.kind == TY_UNKNOWN &&
                                ctx->current_abi_specialization &&
                                e->as.call_.ctor->adt) {
                                Type sr = emit_resolve_type(ctx,
                                    ctx->current_abi_specialization->result_type);
                                if (type_adt_app_def(&sr) == e->as.call_.ctor->adt)
                                    fty = adt_field_type_for_app(
                                        &sr, &e->as.call_.ctor->fields[i]);
                            }
                            const char *fcty = (fty.kind != TY_UNKNOWN)
                                ? type_c_name(fty) : NULL;
                            size_t fL = fcty ? strlen(fcty) : 0;
                            const char *tgt = (fcty && fL >= 1 && fcty[fL - 1] == '*')
                                ? fcty : "int64_t";
                            Buf c; buf_init(&c);
                            buf_printf(&c, "(%s)(intptr_t)(%s)", tgt, arg_strs[i]);
                            buf_putc(&c, '\0');
                            free(arg_strs[i]);
                            arg_strs[i] = strdup(c.data);
                            buf_free(&c);
                        }
                    }
                    /* gcc14-int-conversion: an existential pack argument is
                     * `tur_exists_t` == `void *`, but a ctor stores it in the int64
                     * carrier field slot (`ctor_Box(int64_t)`).  Passing the void*
                     * pack into the int64 param is "integer from pointer" (a hard
                     * error under GCC >= 14).  Cast to int64 -- fires regardless of
                     * `suffix`/`field_is_carrier` (an existential field's full_type
                     * is TY_EXISTS, not a tyvar, so the pointer->int64 block above
                     * does not cover it). */
                    if (!field_inline && arg &&
                        emit_resolve_type(ctx, arg->type).kind == TY_EXISTS) {
                        arg_strs[i] = emit_carrier_reinterpret(ctx, arg_strs[i], NULL, "ctor-field-exists");
                    }
                    /* A FLOAT argument flowing into a ctor field that is ERASED
                     * to the int64 CARRIER -- a tyvar field of a carrier-helper
                     * base ctor (`ctor_Result(bool,int64_t,int64_t)`'s `ok_val`,
                     * declared `a`) -- must be BIT-reinterpreted, not numerically
                     * converted: `ok__spec__int64_t_double`'s `ctor_Result(true,
                     * x, ...)` with `x` a double would truncate 2.5 -> 2.  Gate on
                     * the ctor FIELD's declared type being a tyvar (== the int64
                     * carrier): a CONCRETE float field (`ctor_Circle(double)`'s
                     * `radius : float`) takes the double directly and bit-packs
                     * internally, and a monomorph ctor (suffix != NULL) likewise
                     * takes the concrete double -- both must be excluded. */
                    if (!suffix && !field_inline && arg && e->as.call_.ctor &&
                        i < e->as.call_.ctor->n_fields) {
                        Type art = emit_resolve_type(ctx, arg->type);
                        /* `full_type` is non-NULL only for a tyvar-declared
                         * field (carrier-erased to int64); a concrete field
                         * (`radius : float`) leaves it NULL and is NOT a carrier. */
                        const Type *fft = e->as.call_.ctor->fields[i].full_type;
                        bool field_is_carrier = fft && fft->kind == TY_TYVAR;
                        if (field_is_carrier &&
                            (art.kind == TY_FLOAT || art.kind == TY_FLOAT32 ||
                             art.kind == TY_FLOAT64)) {
                            const char *fcn = type_c_name(art);
                            Buf c; buf_init(&c);
                            buf_printf(&c, "((union { %s s; int64_t d; }){.s = (%s)}).d",
                                       fcn, arg_strs[i]);
                            buf_putc(&c, '\0');
                            free(arg_strs[i]);
                            arg_strs[i] = strdup(c.data);
                            buf_free(&c);
                        }
                    }
                    /* CONV-S1 seam 4 (typedef ordering): a lowered `Result`/`Option`
                     * monomorph ctor stores a non-parametric value-struct field as a
                     * heap pointer `T *` (adt_field_c_type) -- matching the struct
                     * path's carrier-box layout and letting a forward decl satisfy
                     * the typedef ordering.  The ctor PARAM is therefore `T *`, but
                     * the arg here is the `T` value, so heap-box it (malloc + copy)
                     * and pass the pointer.  Inert at default (Result/Option are
                     * structs there, so this monomorph ctor never fires). */
                    if (suffix && arg && arg->kind != EX_DEFAULT_OF &&
                        e->as.call_.ctor &&
                        e->as.call_.ctor->adt && e->as.call_.ctor->adt->name &&
                        (strcmp(e->as.call_.ctor->adt->name, "Result") == 0 ||
                         strcmp(e->as.call_.ctor->adt->name, "Option") == 0)) {
                        /* An EX_DEFAULT_OF arg is the *unused* variant slot (e.g.
                         * `err`'s ok_val): the EX_DEFAULT_OF block above already
                         * materialised it in the slot's true C type -- the NULL
                         * pointer `(T *){0}` for a ros-pointer-box value-struct
                         * field, or `(T){0}` otherwise -- so it is passed
                         * directly.  Heap-boxing it here (malloc + `*tmp = (T
                         * *){0}`) would store a `T *` literal into a `T` slot, a
                         * type error.  Only a real by-value payload arg is boxed. */
                        Type at = emit_resolve_type(ctx, arg->type);
                        /* structdef-retirement slice 1 / DS-C: a non-parametric
                         * by-value record-ADT arg (a lowered defstruct -- `User`
                         * is a TY_ADT) is heap-boxed to match the `tur_adt_User *`
                         * pointer slot adt_field_c_type emits.  The former
                         * TY_STRUCT box_struct arm is dead. */
                        bool box_adt = at.kind == TY_ADT && at.as.adt_.def &&
                            !at.as.adt_.def->is_heap &&
                            at.as.adt_.def->n_type_params == 0 &&
                            adt_is_byvalue_product(at.as.adt_.def);
                        if (box_adt) {
                            const char *cn = type_c_name(at);
                            char *tmp = fresh_tmp(ctx);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                                       cn, tmp, cn, cn);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "*%s = (%s);\n", tmp, arg_strs[i]);
                            free(arg_strs[i]);
                            arg_strs[i] = strdup(tmp);
                            free(tmp);
                        }
                    }
                    /* macos-int-conversion-carrier-pointer-straddles (case A):
                     * last word on the pointer<->carrier straddle at a
                     * monomorphized ctor's field slot.  Every cast above
                     * re-derives the slot's C type from the field's Type, and
                     * for a fn-typed field that re-derivation is ambiguous --
                     * type_c_name(TY_FN) branches on the `boxed` flag, which is
                     * absent from both the TY_FN mangle and type_eq, so
                     * `ctor_Option__fn1_int__int` (slot `void *`) and
                     * `ctor_Option__fn1_float__float` (slot `int64_t`) are
                     * indistinguishable by type alone.  The signature side
                     * table records the string each prototype actually carries
                     * (types.c record_adt_app_ctor_sigs), so consult that.
                     *
                     * Fires ONLY on an int64<->pointer straddle, and only when
                     * the argument's own emitted C type is known for certain:
                     * an explicit `(T)(intptr_t)` cast this block already
                     * applied, or a bare reference to a parameter of the active
                     * ABI spec (whose C type is emit_type_c_name of the spec's
                     * arg type, by construction of the clone's signature).  A
                     * pointer->pointer mismatch is deliberately left alone: it
                     * means a mis-selected monomorph, and a cast would paper it
                     * over rather than fix it (see data-literal-nested). */
                    const char *slot_cty =
                        emit_sig_lookup_param_ctype(ctor_cname, i);
                    if (slot_cty && arg_strs[i]) {
                        const char *av = arg_strs[i];
                        const char *arg_cty = NULL;
                        if (strncmp(av, "(int64_t)(intptr_t)", 19) == 0) {
                            arg_cty = "int64_t";
                        } else if (av[0] == '(' && strstr(av, " *)(intptr_t)")) {
                            arg_cty = "*";   /* some concrete pointer */
                        } else if (arg && arg->kind == EX_VAR) {
                            Type sp;
                            if (emit_var_spec_arg_type(ctx, arg, &sp))
                                arg_cty = emit_type_c_name(ctx, sp);
                        }
                        if (arg_cty) {
                            size_t sl = strlen(slot_cty);
                            bool slot_is_ptr = sl && slot_cty[sl - 1] == '*';
                            size_t al = strlen(arg_cty);
                            bool arg_is_ptr = al && arg_cty[al - 1] == '*';
                            bool arg_is_i64 = strcmp(arg_cty, "int64_t") == 0;
                            bool slot_is_i64 = strcmp(slot_cty, "int64_t") == 0;
                            if ((slot_is_i64 && arg_is_ptr) ||
                                (slot_is_ptr && arg_is_i64)) {
                                Buf c; buf_init(&c);
                                buf_printf(&c, "(%s)(intptr_t)(%s)",
                                           slot_cty, arg_strs[i]);
                                buf_putc(&c, '\0');
                                free(arg_strs[i]);
                                arg_strs[i] = strdup(c.data);
                                buf_free(&c);
                            }
                        }
                    }
                }
                free(ctor_cname);
                char *_mc = mangle_field_name(fn_binding->name->name);
                Buf out; buf_init(&out);
                if (suffix) {
                    buf_printf(&out, "ctor_%s%s(", _mc, suffix);
                    free(suffix);
                } else {
                    buf_printf(&out, "ctor_%s(", _mc);
                    /* dead-base-thunk-chain-references-undefined-ctor: a
                     * suffix-less ctor call on a HEAP parametric ADT names the
                     * base `ctor_X`, which is never defined -- heap parametric
                     * ADTs only get per-spec ctors (`ctor_Cons__int`), unlike
                     * carrier-lowered parametric ADTs (Option/Result), whose
                     * base ctor exists.  Such a call is only reachable from
                     * the dead base generic thunk chain (every live path
                     * routes to a spec).  Register it so a static trap
                     * definition lands in the forward-decl band (before every
                     * body, in the carrier convention -- base bodies are
                     * carrier-typed): the call then compiles on clang 16+
                     * (no implicit declaration) AND a hand -O0 compile of
                     * emit-c output links clean, while a genuinely live call
                     * -- a compiler defect -- aborts loudly at runtime. */
                    if (e->as.call_.ctor && e->as.call_.ctor->adt &&
                        e->as.call_.ctor->adt->n_type_params > 0 &&
                        e->as.call_.ctor->adt->is_heap) {
                        emit_note_dead_base_ctor(ctx, _mc, e->as.call_.n_args);
                    }
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
            /* unascribed-carrier-helper-read-collapses-element-tyvar: a
             * constrained-instance element call may re-dispatch to a different
             * concrete instance than the elab-baked representative -- e.g. the
             * float spec of `Enc [Vec]` re-resolves to `enc_float(double)` even
             * though `fn_binding` is the baked `enc_cstr(const char *)`.  The
             * carrier-scalar cast below must follow that ACTUAL callee's param
             * ABI, so consult the re-resolved FnDef (NULL when the call is not
             * re-dispatched, e.g. the carrier base clone, where the baked binding
             * is what gets emitted). */
            FnDef *reresolved_callee = emit_reresolve_method_fndef(ctx, e);
            char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
            if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *arg_expr = e->as.call_.args[i];
                /* M5 residual-straddle (docs/artifacts/m5-residual-straddle-
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
                /* end-to-end-monomorphization (root 2 retirement): a `(:: x :int)`
                 * whose inner resolves to a CONCRETE `:heap` type (`Vec__int *`)
                 * must keep the ascribe so the EX_ASCRIBE emit performs the
                 * pointer->int64 carrier relabel; stripping it here would pass
                 * the raw typed pointer into a carrier int64 param
                 * (-Wint-conversion).  The carrier base (abstract `(Vec A)` ->
                 * int64) does not match this gate. */
                if (!preserve_ascribe_for_bridge && arg_expr &&
                    arg_expr->kind == EX_ASCRIBE &&
                    arg_expr->type.kind == TY_INT &&
                    arg_expr->as.ascribe_.inner) {
                    const Expr *asc_inner = arg_expr->as.ascribe_.inner;
                    Type inner_r;
                    if (!emit_var_spec_arg_type(ctx, asc_inner, &inner_r))
                        inner_r = emit_resolve_type(ctx, asc_inner->type);
                    if (type_is_heap_struct(inner_r) &&
                        type_has_concrete_codegen_layout(&inner_r)) {
                        preserve_ascribe_for_bridge = true;
                    }
                }
                /* constrained-generic-dispatch-float-element: keep an
                 * `(:: (vec-get v i) A)` ascription intact when the element
                 * tyvar `A` resolves (through the active spec) to a FLOAT width
                 * over an int64 carrier inner.  Stripping it would hand the raw
                 * int64 carrier bits to a `double` parameter -- a NUMERIC
                 * int64->double conversion (1.5 -> 4.6e18), not the bit
                 * reinterpret the concrete `(:: ... :float)` form performs.
                 * Preserving the wrapper routes it through the EX_ASCRIBE emit,
                 * which bridges carrier->concrete for the float case. */
                if (!preserve_ascribe_for_bridge && arg_expr &&
                    arg_expr->kind == EX_ASCRIBE &&
                    arg_expr->type.kind == TY_TYVAR &&
                    arg_expr->as.ascribe_.inner &&
                    arg_expr->as.ascribe_.inner->type.kind == TY_INT &&
                    ctx->current_abi_specialization) {
                    Type rtv = emit_resolve_type(ctx, arg_expr->type);
                    if (rtv.kind == TY_FLOAT || rtv.kind == TY_FLOAT32 ||
                        rtv.kind == TY_FLOAT64) {
                        preserve_ascribe_for_bridge = true;
                    }
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
                /* consolidation increment 2 (bind cell): pair the continuation
                 * wrapper's return ABI with the entry point THIS call selects.
                 * A carrier base instance (`__inst_*_tyvar` -- no by-value
                 * spec matched, result still mentions a tyvar) invokes a
                 * tur_poly_fn_t continuation through the int64 carrier cast,
                 * so an EX_POLY_WRAP argument must spill-shim a by-value
                 * aggregate result; a by-value spec callee consumes the raw
                 * aggregate and must NOT see the shim (the load-bearing
                 * Option pairing).  Scoped to the direct EX_POLY_WRAP arg so
                 * nested emissions are unaffected. */
                bool saved_pwc = ctx->poly_wrap_callee_carrier;
                if (emit_arg && emit_arg->kind == EX_POLY_WRAP &&
                    !matched_spec && fn_binding && fn_binding->name &&
                    fn_binding->name->name &&
                    strncmp(fn_binding->name->name, "__inst_", 7) == 0 &&
                    fn_binding->type.kind == TY_FN &&
                    fn_binding->type.as.fn.result_full_type &&
                    emit_repr_type_mentions_tyvar(
                        fn_binding->type.as.fn.result_full_type)) {
                    ctx->poly_wrap_callee_carrier = true;
                }
                char *raw = emit_value(ctx, body, emit_arg);
                ctx->poly_wrap_callee_carrier = saved_pwc;
                /* byvalue-result-param-ok-predicate-materialize-bad-cast: set
                 * once a pass-by-pointer struct *parameter* argument has been
                 * converted to the int64 carrier by a pointer cast (its C value
                 * is already `const T *`, i.e. a pointer to the by-value
                 * aggregate, which IS a valid carrier handle).  Suppresses the
                 * later pass-by-ptr `(*(...))` deref (which is for callees that
                 * take the struct by value), so the two do not compound into
                 * `(*((int64_t)(intptr_t)(&tmp)))`. */
                bool pbp_carrier_cast = false;
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
                 * docs/archive/history/m5-suite-residual-6-failures-2026-06-14.md. */
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
                /* unascribed-carrier-helper-read-collapses-element-tyvar: non-NULL
                 * (a concrete scalar pointer C type, e.g. "const char *") when the
                 * carrier int64 must be reinterpreted to a non-void pointer param. */
                const char *scalar_carrier_cty = NULL;
                /* Phase P3: TY_INT (int64_t) arg passed to a TY_PTR_VOID (void*) param
                 * requires (void*)(intptr_t) coercion. Occurs when persistent-map
                 * lowering passes a map handle (int64_t) to hamt/count etc. */
                if (!needs_fn_cast && fn_binding->type.kind == TY_FN) {
                    uint32_t n_fnparams = fn_binding->type.as.fn.arity;
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
                    /* unascribed-carrier-helper-read-collapses-element-tyvar: the
                     * carrier base clone of a constrained instance whose
                     * representative is a non-int scalar (e.g. `Enc [cstr]` when no
                     * `Enc [int]` exists) dispatches the element call to that
                     * representative method -- `__inst_Enc_enc_cstr(const char *)` --
                     * but the element read comes through the int64 carrier
                     * (`vec-get`'s erased `:A`).  Passing the carrier int64 to the
                     * `const char *` (TY_CSTR) param trips -Wint-conversion.  Bridge
                     * it through the param's own C type via intptr_t: the base clone
                     * is a representative placeholder (only the per-element ABI
                     * specializations are ever called with real data), so the
                     * reinterpret only needs to be valid C. */
                    else if ((_emit_is_int || _arg_is_int) && _pk == TY_CSTR &&
                             (!reresolved_callee ||
                              (param_idx < reresolved_callee->n_params &&
                               reresolved_callee->param_types &&
                               reresolved_callee->param_types[param_idx].kind == TY_CSTR))) {
                        needs_fn_cast = true;
                        scalar_carrier_cty = type_c_name(emit_type_from_kind(_pk));
                    }
                }
                if (needs_fn_cast && fn_binding->type.kind == TY_FN) {
                    uint32_t n_fnparams = fn_binding->type.as.fn.arity;
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
                               && (fn_binding->body_is_inline_c || !matched_spec ||
                                   (matched_spec &&
                                    strcmp(emit_type_c_name(ctx,
                                        matched_spec->arg_types[i]), "int64_t") == 0))) {
                        /* Polymorphic param emitted as the int64 carrier: an
                         * inline-C body keeps `val` typed int64_t regardless of A,
                         * and the generic (non-spec, !matched_spec) emit of a
                         * make-struct/carrier body likewise declares the param
                         * int64_t (e.g. `static int64_t some(int64_t)`).  A
                         * TY_FN/TY_PTR_VOID actual (a closure stashed into the
                         * carrier slot) must be bridged through (int64_t)(intptr_t)
                         * -- otherwise clang trips -Wint-conversion.  A matched
                         * spec usually resolves A to a concrete C type (no-cast),
                         * but when it resolves to the int64 carrier itself -- a
                         * by-value construct spec `some__spec__...Option__int_int64_t`
                         * whose value param is the int64 carrier, fed a `(:: (fn ..)
                         * int)` closure (`void *`) -- the cast is still needed. */
                        needs_fn_cast = true;
                        cast_to_void_ptr = false;
                    } else if ((pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS)
                               && matched_spec
                               && emit_type_c_name(ctx, matched_spec->arg_types[i])
                               && strcmp(emit_type_c_name(ctx,
                                            matched_spec->arg_types[i]),
                                         "void *") == 0) {
                        /* clang int-conversion (witness-arg straddle): a tyvar
                         * param that the matched spec grounded to `void *` -- e.g. a
                         * `void`-witness monomorph whose param is `void *witness`
                         * (vec_empty_like's phantom witness) -- fed the int64
                         * carrier.  `void *` <- int64 is `integer to pointer
                         * conversion`, a hard error under clang's default
                         * -Wint-conversion.  Bridge int64 -> void* through intptr_t
                         * (value-preserving; the witness is a phantom the callee
                         * never dereferences). */
                        needs_fn_cast = true;
                        cast_to_void_ptr = true;
                    } else if (pk == TY_CSTR && scalar_carrier_cty) {
                        /* unascribed-carrier-helper-read-collapses-element-tyvar:
                         * keep the carrier-int64 -> const char* reinterpret set
                         * above (the scalar_carrier_cty emission path), rather than
                         * falling through to the no-cast default. */
                        needs_fn_cast = true;
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
                    uint32_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i : (uint32_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    if (FN_ARG_FLAG(fn_binding->type.as.fn, param_idx, FA_FAT))
                        cast_to_void_ptr = false;
                }
                /* gcc14-int-conversion (re-dispatched method to a concrete-pointer
                 * param): the cast decision above keys on the GENERIC `fn_binding`'s
                 * param kind, which for a re-dispatched method (`show` -> the
                 * concrete `__inst_Show_show_cstr`) is the int64 carrier (TY_STRUCT
                 * / tyvar) -- so the default `else` emits `(int64_t)(intptr_t)arg`
                 * into what is actually a `const char *` / `tur_adt_X *` param, a
                 * macOS-clang hard error.  The re-resolved FnDef carries the
                 * CONCRETE `param_types[i]`; when that is a (non-void) pointer,
                 * bridge to it instead of the int64 carrier.  Only overrides the
                 * bare-int64 fallback, so every already-correct cast is unchanged. */
                const char *reresolve_ptr_cty = NULL;
                if (reresolved_callee && i < reresolved_callee->n_params &&
                    reresolved_callee->param_types) {
                    const char *pc = emit_type_c_name(ctx,
                        emit_resolve_type(ctx, reresolved_callee->param_types[i]));
                    size_t pL = pc ? strlen(pc) : 0;
                    if (pc && pL >= 1 && pc[pL - 1] == '*' && strcmp(pc, "void *") != 0)
                        reresolve_ptr_cty = pc;
                }
                if (needs_fn_cast) {
                    Buf cast; buf_init(&cast);
                    if (fn_cast_typedef) {
                        /* typed-c-abi-function-pointers: cast through the bare
                         * `R (*)(A...)` typedef (a captureless fn's C name
                         * decays to a function pointer of that exact shape). */
                        buf_printf(&cast, "(%s)(%s)", fn_cast_typedef, raw);
                    } else if (scalar_carrier_cty) {
                        /* carrier int64 -> concrete scalar pointer (e.g.
                         * `const char *`) reinterpret via intptr_t. */
                        buf_printf(&cast, "(%s)(intptr_t)(%s)", scalar_carrier_cty, raw);
                    } else if (cast_to_void_ptr) {
                        buf_printf(&cast, "(void *)(intptr_t)(%s)", raw);
                    } else if (reresolve_ptr_cty) {
                        buf_printf(&cast, "(%s)(intptr_t)(%s)", reresolve_ptr_cty, raw);
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
                if (e->as.call_.poly_arg_mask & ARG_IDX_BIT(i)) {
                    Buf cast; buf_init(&cast);
                    buf_printf(&cast, "*(tur_poly_fn_t*)(intptr_t)(%s)", raw);
                    buf_putc(&cast, '\0');
                    free(raw);
                    raw = strdup(cast.data);
                    buf_free(&cast);
                }
                /* Slice 3 (constrained-hkt-forall codegen): a poly-wrapper's
                 * inner-call arg that arrived through the carrier as an int64
                 * heap-box pointer to a by-value aggregate -- deref it back to
                 * the aggregate the callee parameter expects.  The target C type
                 * is the callee's own parameter full type. */
                if (e->as.call_.poly_agg_arg_mask & ARG_IDX_BIT(i) &&
                    fn_binding && fn_binding->type.kind == TY_FN &&
                    fn_binding->type.as.fn.arg_full_types &&
                    i < fn_binding->type.as.fn.arity &&
                    fn_binding->type.as.fn.arg_full_types[i] &&
                    /* VBM2b: in a by-value monomorphized lens body the `(f a)`
                     * arg is produced BY VALUE (the `g` poly call returns the
                     * aggregate directly), not as an int64 heap-box pointer, so
                     * the Path A deref-back must NOT fire. */
                    !(ctx->current_abi_specialization &&
                      ctx->current_abi_specialization->is_vl_wide_mono)) {
                    char *ub = emit_agg_unbox(ctx,
                        *fn_binding->type.as.fn.arg_full_types[i], raw);
                    free(raw);
                    raw = ub;
                }
                /* vec-push-heap-struct-element-not-carrier-cast: the symmetric
                 * direction of the ACB bridge below.  When the callee param is a
                 * polymorphic tyvar that lowers to the int64 carrier -- an
                 * inline-C body keeps `val` typed `int64_t` for every A
                 * (`vec-push! [A] [v : (Vec A) val : A]`), and an unspecialized
                 * generic body likewise -- but the ARGUMENT emits a CONCRETE
                 * carrier-ABI value (a by-value parametric struct such as
                 * `(:: (some 5) (Option int))` -> `Option__int`, a nested heap
                 * container `(Vec int)` -> `Vec__int *`, or any concrete
                 * `*__spec__*` result), the value must be bridged to the
                 * carrier.  emit_carrier_bridge picks the right form per
                 * representation (aggregate spill-and-address, pointer
                 * reinterpret, inline-scalar union).  The receiver `(Vec A)`
                 * already rides the carrier; without this an element value of a
                 * nested parametric type (`(Vec (Option int))`) reaches the
                 * `int64_t` slot uncast and trips an incompatible-type /
                 * -Wint-conversion cc error.  A matched concrete spec resolves A
                 * to the element's real C type and keeps its own
                 * (carrier->concrete) bridges, so it is excluded here. */
                Type spec_byval_ty = type_simple(TY_UNKNOWN, CK_COPY);
                bool spec_byval =
                    call_spec_result_byvalue_app(ctx, emit_arg, &spec_byval_ty);
                if (!needs_fn_cast && fn_binding->type.kind == TY_FN && emit_arg &&
                    (expr_emits_byvalue_carrier_abi(ctx, emit_arg) || spec_byval)) {
                    uint32_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i
                        : (uint32_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    if ((pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS) &&
                        (fn_binding->body_is_inline_c || !matched_spec)) {
                        /* vec-push-byvalue-aggregate-element-stores-dangling-
                         * stack-address: this carrier sink is a heap container
                         * insert (vec-push! / map-set! / set-add!, all inline-C
                         * `val : A` carriers) -- the stored element OUTLIVES the
                         * push expression and the producing frame.  The default
                         * bridge would carry (int64_t)(intptr_t)(&tmp), a stack
                         * address that dangles once this frame returns.  Use the
                         * escaping variant so a by-value aggregate element is
                         * heap-promoted (malloc + copy) instead of address-of'd.
                         *
                         * Bridge with the value's REAL by-value type, not
                         * `emit_arg->type`: a return-only-poly accessor (`ok-val`)
                         * has its elab type collapsed to the int64 scalar, so
                         * spilling at `emit_arg->type` would declare an `int64_t`
                         * temp and copy an `Option__int` into it.  The matched
                         * spec's resolved result is the source of truth. */
                        Type bridge_ty = fn_body_tail_byvalue_carrier_type(ctx, emit_arg);
                        if (bridge_ty.kind == TY_UNKNOWN && spec_byval)
                            bridge_ty = spec_byval_ty;
                        if (bridge_ty.kind == TY_UNKNOWN) bridge_ty = emit_arg->type;
                        raw = emit_carrier_bridge_escaping(ctx, body, raw,
                                                           CK_CONCRETE, CK_CARRIER,
                                                           bridge_ty);
                    }
                }
                /* CONV-S1: a by-value parametric ADT-app argument
                 * (`tur_adt_Option__int`) passed to a uniform-carrier (int64)
                 * parameter -- e.g. the parametric typeclass instance method
                 * `__inst_Eq_eq_qu_T(int64_t, ...)` selected for an `(Option int)`
                 * receiver, whose body reads each param back as
                 * `((tur_adt_Option *)(intptr_t)x)->...` -- must be boxed to the
                 * carrier (spill + address-of + cast).  `type_uses_carrier_abi`
                 * reports a by-value app as non-carrier, so the
                 * `expr_emits_byvalue_carrier_abi` bridges above never fire; detect
                 * the by-value app directly and bridge only when the callee param
                 * lowers to the int64 carrier (a class type-variable slot), never a
                 * concrete-aggregate param (which already takes the value by value).
                 * A matched concrete spec resolves the param to the real C type and
                 * carries its own bridges, so it is excluded. */
                if (!needs_fn_cast && !matched_spec &&
                    fn_binding->type.kind == TY_FN && emit_arg &&
                    emit_arg->type.kind == TY_APP &&
                    adt_app_is_byvalue_product(emit_arg->type)) {
                    uint32_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? (uint8_t)i
                        : (uint8_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    /* pk is the param's NOMINAL kind: a class type variable
                     * (TY_TYVAR/FORALL/EXISTS) or the bare parametric ADT it
                     * resolved to (TY_ADT -- e.g. `eq? [x : T]` with T = the bare
                     * `Option`).  A bare *parametric* ADT param cannot be emitted
                     * by value (its field is polymorphic), and an un-specialized
                     * (`!matched_spec`) generic/instance method emits the int64
                     * carrier for all of these -- so a by-value TY_APP monomorph
                     * arg must be boxed.  A non-parametric by-value ADT param
                     * (also pk==TY_ADT, but emitted by value) only ever receives a
                     * TY_ADT arg, never a TY_APP monomorph, so it never reaches
                     * here.  A generic `(Result A B)` param (pk==TY_APP whose
                     * full_type is a NON-concrete app -- a free fn like
                     * `ok? [r : (Result A B)]`) is likewise emitted on the int64
                     * carrier, so its by-value monomorph arg is boxed too; a
                     * CONCRETE app param (`(Result int cstr)`) is emitted by value
                     * and must NOT be bridged.  A generic param whose type erased
                     * all the way to the int64 carrier (pk==TY_INT -- e.g. a free
                     * `ok? [r : (Result A B)]` whose param collapses to int64) is
                     * also a carrier sink.  In every admitted case the param lowers
                     * to int64; the by-value-app guard above means a genuine int
                     * param never reaches here (a by-value monomorph could not
                     * type-check into an int slot). */
                    bool app_param_carrier = false;
                    if (pk == TY_APP && fn_binding->type.as.fn.arg_full_types &&
                        fn_binding->type.as.fn.arg_full_types[param_idx])
                        app_param_carrier = !type_app_is_concrete_adt(
                            fn_binding->type.as.fn.arg_full_types[param_idx]);
                    if (pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS ||
                        pk == TY_ADT || pk == TY_INT || app_param_carrier) {
                        /* byvalue-result-param-ok-predicate-materialize-bad-cast:
                         * a pass-by-pointer struct *parameter* (`r : (Result int
                         * cstr)`, materialized in C as `const T *`) is already a
                         * pointer to the by-value aggregate -- which is exactly a
                         * carrier handle (the non-pbp branch of emit_carrier_bridge
                         * spills the value and hands back `&tmp`).  So the crossing
                         * is a plain pointer->int64 cast; the generic bridge would
                         * instead spill `T tmp = r;` (aggregate = pointer -- a hard
                         * cc error) and then the pass-by-ptr `(*(...))` deref below
                         * would compound it.  Emit the cast directly and flag it so
                         * that deref is skipped. */
                        /* vec-push-byvalue-aggregate-escapes-frame (regression
                         * guard): when the carrier sink is an inline-C heap-container
                         * insert (`vec-push! [A] [val : A]`, map-set!, set-add!) and
                         * the element's emitted representation is a genuine by-value
                         * aggregate STRUCT (`(some 5)` -> `tur_adt_Option__int`), the
                         * stored element OUTLIVES the push expression AND the
                         * producing frame.  The default (non-escaping) bridge spills
                         * the aggregate to a stack local and carries
                         * `(int64_t)(intptr_t)(&tmp)`, a stack address that dangles
                         * once this frame returns -- the fixture reads reclaimed-stack
                         * garbage.  Block 5343 heap-promotes this for a `val : A`
                         * whose monomorph kind stays TY_TYVAR, but when `A` resolves
                         * to a by-value parametric app (`(Option int)`) the param kind
                         * here is TY_APP, so this seam handles it instead.
                         *
                         * The discriminator is the arg's emitted C type name: only a
                         * real struct (not `int64_t`, not a pointer) is spilled by the
                         * plain bridge and needs heap-promotion.  A single-scalar-field
                         * product that collapses to the int64 carrier (`(Box int)` ->
                         * `int64_t`, an inline-C method's `(box-new ...)` receiver) is
                         * already pointer-sized: the plain pointer-reinterpret bridge
                         * below is correct and a heap-promote would corrupt the
                         * receiver deref. */
                        const char *_argcn = emit_type_c_name(ctx, emit_arg->type);
                        size_t _acnl = _argcn ? strlen(_argcn) : 0;
                        bool _arg_is_byval_struct = _argcn &&
                            strcmp(_argcn, "int64_t") != 0 &&
                            !(_acnl >= 1 && _argcn[_acnl - 1] == '*');
                        /* ...and the call actually STORES the element into a
                         * persistent heap collection: a sibling argument is a heap
                         * container (`(Vec A)`/`(Map K V)`/`(Set A)`).  This is what
                         * separates a container insert (vec-push!/map-set!/set-add!,
                         * whose element outlives the frame) from a TRANSIENT inline-C
                         * consumer of a by-value aggregate (`unwrap-or`, `map` on an
                         * `(Option A)`, catch-unwind result readers), which reads the
                         * value in-place and must NOT heap-promote -- doing so both
                         * leaks the malloc'd copy and needlessly churns codegen. */
                        bool _has_heap_container_sibling = false;
                        for (uint32_t _j = 0; _j < e->as.call_.n_args; _j++) {
                            if (_j == i) continue;
                            Type _st = emit_resolve_type(ctx, e->as.call_.args[_j]->type);
                            if (type_is_heap_adt(_st) || type_is_heap_struct(_st)) {
                                _has_heap_container_sibling = true;
                                break;
                            }
                        }
                        if (expr_is_pbp_param(ctx, emit_arg)) {
                            Buf _pb; buf_init(&_pb);
                            buf_printf(&_pb, "(int64_t)(intptr_t)(%s)", raw);
                            free(raw);
                            raw = strdup(_pb.data);
                            buf_free(&_pb);
                            pbp_carrier_cast = true;
                        } else if (fn_binding->body_is_inline_c &&
                                   _arg_is_byval_struct &&
                                   _has_heap_container_sibling) {
                            /* Bridge with the value's REAL by-value type (a
                             * return-only-poly accessor collapses its elab type to the
                             * int64 scalar). */
                            Type bridge_ty =
                                fn_body_tail_byvalue_carrier_type(ctx, emit_arg);
                            if (bridge_ty.kind == TY_UNKNOWN)
                                bridge_ty = emit_arg->type;
                            raw = emit_carrier_bridge_escaping(ctx, body, raw,
                                                               CK_CONCRETE, CK_CARRIER,
                                                               bridge_ty);
                        } else {
                            raw = emit_carrier_bridge(ctx, body, raw,
                                                      CK_CONCRETE, CK_CARRIER,
                                                      emit_arg->type);
                        }
                    }
                }
                /* CONV-S1 (seam-4): the non-parametric counterpart of the TY_APP
                 * block above.  A by-value NON-parametric lowered-ADT argument (a
                 * lowered `defstruct`, e.g. `tur_adt_Pos`) passed to a generic
                 * carrier (int64) parameter -- a tyvar slot of an inline-C or
                 * unspecialised generic helper (`__set-raw [A] [v : A]` ->
                 * `_un_unset_hyraw(int64_t)`) -- must be boxed (spill + address-of +
                 * cast) the same way the TY_APP monomorph is.  emit_type_is_byvalue_
                 * adt covers TY_APP too, which the block above already handled, so
                 * resolve the arg's concrete type and restrict to the bare TY_ADT
                 * case here.  The param kind must be a genuine polymorphic carrier
                 * slot (TY_TYVAR/FORALL/EXISTS); a concrete by-value ADT param
                 * (pk==TY_ADT emitted by value) takes the aggregate directly and is
                 * excluded.  A matched concrete spec resolves the param to the real
                 * C type and carries its own bridges, so it is excluded. */
                if (!needs_fn_cast && !matched_spec &&
                    fn_binding->type.kind == TY_FN && emit_arg) {
                    Type rarg = emit_resolve_type(ctx, emit_arg->type);
                    if (rarg.kind == TY_ADT && emit_type_is_byvalue_adt(ctx, rarg)) {
                        uint32_t n_fnparams = fn_binding->type.as.fn.arity;
                        uint8_t param_idx = (i < n_fnparams) ? (uint8_t)i
                            : (uint8_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                        TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                        /* SR1: an explicitly `:int`-typed parameter is a carrier
                         * slot too, and a by-value aggregate has to be boxed into
                         * it exactly as it is into a tyvar slot.  This is the
                         * `:int`-as-type-eraser shape CLAUDE.md warns about, and
                         * `stdlib/fix.tur` is built on it -- `(defn roll [layer :
                         * int] ...)` takes any functor layer, so `(roll (Stop 0))`
                         * hands a `CountF` to an `int64_t` formal.  While every sum
                         * rode the carrier that was a no-op; now it is a hard C
                         * error ("incompatible type for argument 1 of 'roll'").
                         * The value is stored into the Fix cell and outlives the
                         * call, so it takes the ESCAPING bridge -- a stack spill
                         * would hand out an address that dangles on return.
                         * (`emit_carrier_bridge` short-circuits a transparent int
                         * newtype to the identity, so a real int-shaped value that
                         * merely wears an ADT name is not boxed.) */
                        /* Prefer the callee's EMITTED parameter type over its
                         * declared kind.  The two disagree exactly where this
                         * matters: `(defn eval-node [n : int] ...)` whose body
                         * matches `n` against `ExprNode` patterns has its
                         * parameter refined to the ADT, so the emitted signature
                         * takes the aggregate by value while `arg_kinds[]` still
                         * reads TY_INT.  Boxing into that is the mirror-image
                         * mismatch of not boxing into a real carrier slot. */
                        const char *pct = emit_sig_lookup_param_ctype(fn_name, i);
                        bool int_carrier_param =
                            (pct && *pct)
                                ? (strcmp(pct, "int64_t") == 0)
                                : (pk == TY_INT || pk == TY_INT64 ||
                                   pk == TY_UINT64 || pk == TY_PTR_VOID);
                        if (g_sr1_sum_byvalue && int_carrier_param &&
                            !matched_spec) {
                            raw = emit_carrier_bridge_escaping(
                                      ctx, body, raw, CK_CONCRETE, CK_CARRIER, rarg);
                        } else if ((pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS) &&
                            (fn_binding->body_is_inline_c || !matched_spec)) {
                            /* multiword-element-boxing: a WIDE (> 8 byte) by-value
                             * ADT stored into a heap container through an inline-C
                             * `val : A` carrier param (vec-push! / the HAMT
                             * inserters) OUTLIVES the push frame -- the element
                             * lives as long as the collection.  The default
                             * (non-escaping) bridge spills the aggregate to a stack
                             * local and hands back `(int64_t)(intptr_t)(&tmp)`, a
                             * dangling stack address once this frame returns.  Use
                             * the escaping variant so the element is heap-promoted
                             * (malloc + copy); the collection owns its boxed copy
                             * (value semantics -- mutating the source after insert
                             * does not change the stored element).  The narrow
                             * (transient, non-storing) inline-C carrier crossings
                             * keep the cheaper stack spill.
                             *
                             * Increment 3 (vec-byvalue-struct-element-invalid-c):
                             * a NARROW (<= 8 byte) by-value product is heap-boxed
                             * too when the call stores it into a heap container --
                             * the stack spill dangles exactly like the wide case,
                             * and the read side now deref-unboxes any-width
                             * elements (type_is_boxed_container_elem).  The
                             * container-store discriminator is a heap-container
                             * sibling argument (the receiver `(Vec A)` / map /
                             * set), same as the TY_APP block above; a transient
                             * consumer (`unwrap-or`-style, no container sibling)
                             * keeps the stack spill unchanged. */
                            bool _n3_container_store = false;
                            if (fn_binding->body_is_inline_c &&
                                !type_is_wide_byval_adt(rarg) &&
                                type_is_boxed_container_elem(rarg)) {
                                for (uint32_t _j = 0; _j < e->as.call_.n_args; _j++) {
                                    if (_j == i) continue;
                                    Type _st = emit_resolve_type(
                                        ctx, e->as.call_.args[_j]->type);
                                    if (type_is_heap_adt(_st) ||
                                        type_is_heap_struct(_st)) {
                                        _n3_container_store = true;
                                        break;
                                    }
                                }
                            }
                            if (fn_binding->body_is_inline_c &&
                                (type_is_wide_byval_adt(rarg) ||
                                 _n3_container_store))
                                raw = emit_carrier_bridge_escaping(
                                          ctx, body, raw,
                                          CK_CONCRETE, CK_CARRIER, rarg);
                            else
                                raw = emit_carrier_bridge(ctx, body, raw,
                                                          CK_CONCRETE, CK_CARRIER,
                                                          rarg);
                        }
                    }
                }
                /* SR1, the other direction: a CARRIER argument reaching a param
                 * the callee emits as a by-value aggregate.  A `:int`-declared
                 * parameter whose body matches it against constructors is refined
                 * to that ADT, so the emitted signature takes the aggregate --
                 * `(defn run-calc-op [op : int] ...)` becomes
                 * `run_calc_op(tur_adt_CalcOp)`.  Callers that honour the
                 * DECLARATION still hand over an int64 carrier word: here the
                 * lifted `(fn [op] (run-calc-op op))`, whose own `op` is an
                 * untyped carrier param.  Deref the box back to the aggregate.
                 * While every sum rode the carrier the refinement was invisible
                 * and both sides were int64.
                 *
                 * Keyed on the emitted signature rather than any Type, because the
                 * refined param type is not reachable from the call site -- the
                 * callee's declared TY_FN still says `:int`.  Narrow on purpose: a
                 * non-pointer `tur_adt_` C name (a by-value ADT aggregate) against
                 * an argument that is statically a carrier word. */
                if (g_sr1_sum_byvalue && !needs_fn_cast && !matched_spec &&
                    emit_arg) {
                    const char *pct = emit_sig_lookup_param_ctype(fn_name, i);
                    size_t pl = pct ? strlen(pct) : 0;
                    TypeKind ak = emit_resolve_type(ctx, emit_arg->type).kind;
                    if (pct && pl > 0 && pct[pl - 1] != '*' &&
                        strncmp(pct, "tur_adt_", 8) == 0 &&
                        (ak == TY_INT || ak == TY_INT64 || ak == TY_UINT64 ||
                         ak == TY_PTR_VOID)) {
                        Buf ub; buf_init(&ub);
                        buf_printf(&ub, "(*(%s *)(intptr_t)(%s))", pct, raw);
                        buf_putc(&ub, '\0');
                        free(raw);
                        raw = strdup(ub.data);
                        buf_free(&ub);
                    }
                }
                /* multiword-element-boxing (Map value / spec'd inline-C carrier
                 * insert): map-assoc-eq-o and the other HAMT inserters are inline-C
                 * `defn`s WITH an ABI spec, so the `!matched_spec`-gated block above
                 * skips them -- but an inline-C body takes a tyvar `val : V` param
                 * as the int64 carrier regardless of the spec (inline-C bodies are
                 * not param-specialized).  A WIDE by-value aggregate value must
                 * still be heap-boxed to the carrier: the map stores the value
                 * pointer, which outlives the insert frame, so a stack spill would
                 * dangle.  The read side (`(:: (map-get m k) Point)`) reinterprets
                 * the stored carrier as the box pointer and loads by value. */
                if (!needs_fn_cast && matched_spec && emit_arg &&
                    i < matched_spec->n_args &&
                    /* Only when THIS spec actually lowers the param to the int64
                     * carrier.  Some inline-C specs DO specialize a param to the
                     * concrete by-value aggregate (`__dense_set__...(tur_adt_Pos)`)
                     * -- boxing to the carrier there is a hard type mismatch. */
                    strcmp(emit_type_c_name(ctx, matched_spec->arg_types[i]),
                           "int64_t") == 0 &&
                    fn_binding->type.kind == TY_FN &&
                    fn_binding->body_is_inline_c &&
                    !expr_is_pbp_param(ctx, emit_arg)) {
                    Type rarg = emit_resolve_type(ctx, emit_arg->type);
                    uint8_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? (uint8_t)i
                        : (uint8_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    /* Increment 3: NARROW (<= 8 byte) by-value products box the
                     * same way wide ones do, but only when the call stores into
                     * a heap container (a heap-container sibling argument, e.g.
                     * the `(Map K V)` receiver of map-assoc-eq-o) -- a transient
                     * spec'd inline-C consumer with no container sibling keeps
                     * the plain bridge unchanged. */
                    bool _n3_hamt_store = false;
                    if ((pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS) &&
                        !type_is_heap_struct(rarg) && !type_is_heap_adt(rarg) &&
                        !type_is_wide_byval_adt(rarg) &&
                        type_is_boxed_container_elem(rarg)) {
                        for (uint32_t _j = 0; _j < e->as.call_.n_args; _j++) {
                            if (_j == i) continue;
                            Type _st = emit_resolve_type(
                                ctx, e->as.call_.args[_j]->type);
                            if (type_is_heap_adt(_st) ||
                                type_is_heap_struct(_st)) {
                                _n3_hamt_store = true;
                                break;
                            }
                        }
                    }
                    if ((pk == TY_TYVAR || pk == TY_FORALL || pk == TY_EXISTS) &&
                        !type_is_heap_struct(rarg) && !type_is_heap_adt(rarg) &&
                        (type_is_wide_byval_adt(rarg) || _n3_hamt_store)) {
                        /* Box the value via tur_hamt_box_key (a refcount+size box)
                         * rather than a plain malloc, so the map can RELEASE it on
                         * free / entry-drop: map-assoc threads the value-owned bit
                         * (bit 1 of `owned`) via (tur-wide-byval? v), and the HAMT
                         * retains/releases the box symmetrically with an owned key.
                         * The read side (`(:: (map-get m k) T)`) still just derefs
                         * the payload pointer -- box_key returns the payload past
                         * its header, so the aggregate load is unchanged. */
                        const char *cn = emit_type_c_name(ctx, rarg);
                        char *spill = fresh_tmp(ctx);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = %s;\n", cn, spill, raw);
                        Buf vb; buf_init(&vb);
                        buf_printf(&vb, "(int64_t)(intptr_t)tur_hamt_box_key("
                                        "(const void*)&%s, sizeof(%s))", spill, cn);
                        buf_putc(&vb, '\0');
                        free(raw);
                        raw = strdup(vb.data);
                        buf_free(&vb);
                        free(spill);
                    }
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
                /* A genuine pass-by-pointer struct param (`const T*`) is NOT a
                 * carrier int64_t -- it satisfies the aggregate-carrier branch
                 * only by over-match.  Excluding it here lets the dedicated pbp
                 * deref branch below emit the cleaner `(*(t))` instead of the
                 * redundant `(*(T *)(intptr_t)(t))` cast-round-trip.  The two are
                 * semantically identical (both deref a real pointer to a by-value
                 * copy), so this is a codegen-clarity tightening, not a behaviour
                 * change.  expr_is_pbp_param is side-effect-free (the comment on
                 * the pbp branch below explains why it is the safe predicate to
                 * consult here). */
                /* defopaque-struct-payload: an opaque [A] :int spec arg
                 * (e.g. `(Box Pos)`) is aggregate at the type level but
                 * lowers to plain `int64_t` at the C level -- the carrier
                 * IS the spec's parameter shape, so bridging a CK_CARRIER
                 * source to "concrete" produces a wild deref
                 * (`(*(int64_t *)(intptr_t)(b_892))`). Skip the bridge
                 * when the spec arg's C lowering is already int64_t.
                 * Sibling to the closure-env fix in emit_effects.c that
                 * resolves a generic `:A` capture's env-field type
                 * through the current spec. */
                bool spec_lowers_to_int64 = false;
                if (matched_spec) {
                    const char *spec_cname =
                        emit_type_c_name(ctx, matched_spec->arg_types[i]);
                    if (spec_cname && strcmp(spec_cname, "int64_t") == 0)
                        spec_lowers_to_int64 = true;
                }
                /* nested-construct-byvalue (Gap #4): when the argument is a call
                 * whose OWN matched ABI spec already returns the concrete by-value
                 * aggregate (`ok_val__spec__tur_adt_Box_...` returns a `tur_adt_Box`
                 * by value, not the int64 carrier), the value is ALREADY concrete --
                 * applying the carrier->concrete deref-unbox on top double-unboxes
                 * (`*(tur_adt_Box*)(intptr_t)(<aggregate>)`).  Recognize the
                 * by-value-returning arg spec and skip the bridge.  Scalar-element
                 * accessors (`ok_val__spec__int64_t_...`) still return the carrier
                 * int64 and are bridged as before. */
                bool arg_spec_returns_byvalue_aggregate = false;
                if (emit_arg && emit_arg->kind == EX_CALL &&
                    emit_arg->as.call_.fn_binding &&
                    emit_arg->as.call_.fn_binding->type.kind == TY_FN) {
                    const EmitAbiSpecialization *as = find_matched_abi_spec(
                        ctx, emit_arg, emit_arg->as.call_.fn_binding);
                    if (as) {
                        Type rt = emit_resolve_type(ctx, as->result_type);
                        const char *rc = emit_type_c_name(ctx, rt);
                        if (rc && strcmp(rc, "int64_t") != 0 &&
                            type_kind_is_aggregate(rt.kind) &&
                            !type_uses_carrier_abi(rt))
                            arg_spec_returns_byvalue_aggregate = true;
                    }
                }
                /* CONV-S1 seam 4 (carrier-held by-value receiver -> by-value spec
                 * arg): `emit_arg` is a VAR bound to a param declared `int64_t`
                 * (the uniform dispatch ABI) whose elaborated type is a by-value
                 * aggregate ADT app -- e.g. `ff : (Option (fn ...))` in the
                 * GENERIC carrier-base body of `__inst_Applicative_ap_Option`.
                 * The body reads its fields by deref-ing the int64 carrier
                 * (`emit_carrier_holds_byval`), but a consume specialized to the
                 * by-value monomorph (`some?` -> `some___spec__...Option__opaque`)
                 * wants the aggregate by value.  The arg's own type is the
                 * by-value app (so the TY_INT / carrier-abi disjuncts miss it),
                 * yet the value emitted IS the int64 carrier -- so unbox it.
                 * Suppressed when the active spec already passes this param as a
                 * concrete by-value app (mirrors the EX_GET_FIELD
                 * `recv_spec_byval_adt` guard), where `raw` is the aggregate. */
                bool emit_arg_holds_carrier_byval = false;
                if (emit_arg && emit_arg->kind == EX_VAR &&
                    emit_arg->as.var.binding &&
                    emit_arg->as.var.binding->emit_carrier_holds_byval) {
                    Type vspec;
                    bool passed_byval = emit_var_spec_arg_type(ctx, emit_arg, &vspec) &&
                        vspec.kind == TY_APP && type_app_is_concrete_adt(&vspec) &&
                        !type_is_heap_adt(vspec);
                    emit_arg_holds_carrier_byval = !passed_byval;
                }
                if (!needs_fn_cast && matched_spec &&
                    emit_arg &&
                    !expr_is_pbp_param(ctx, emit_arg) &&
                    !spec_lowers_to_int64 &&
                    !arg_spec_returns_byvalue_aggregate &&
                    type_kind_is_aggregate(matched_spec->arg_types[i].kind) &&
                    /* G9: a by-value aggregate field read (`(.head xs)` off a
                     * monomorphized cell) is ALREADY the concrete aggregate; the
                     * erased TY_INT below would otherwise reconstruct it through a
                     * stale `tur_option_t *` carrier cast. */
                    !field_read_emits_byvalue_aggregate(ctx, emit_arg) &&
                    /* WF3 (van-laarhoven-wide-functor-carrier-plan): a poly call
                     * whose `(f S)` result is a wide by-value aggregate ALREADY
                     * unboxed its carrier result to the concrete aggregate at the
                     * poly-carrier boundary (emit_expr.c:2841).  Feeding that value
                     * to a by-value spec param (a generic `run-id`/`get-const`
                     * consuming `(f S)`) must NOT deref it a second time -- the
                     * carrier-producer disjunct below would otherwise double-unbox
                     * (`*(T*)(intptr_t)(<aggregate>)`, an aggregate used as an
                     * integer).  Opaque functors are one word (no boundary unbox)
                     * and never reach this (the by-value-ADT test below excludes
                     * them), so the guard is unconditional since VBM4. */
                    !(emit_arg->kind == EX_CALL &&
                      emit_arg->as.call_.is_poly_call &&
                      emit_type_is_byvalue_adt(ctx, emit_arg->type)) &&
                    /* VBM2b: the WF3 gate above, for the by-value monomorphized
                     * lens body -- the `g : (-> A (f A))` call emits its `(f a)`
                     * aggregate BY VALUE (not the boxed carrier), so a by-value
                     * ADT arg from any call must NOT be deref'd again.  The
                     * fat-closure `g` call is not is_poly_call, so the gate above
                     * misses it; catch it on the active-spec flag instead. */
                    !(ctx->current_abi_specialization &&
                      ctx->current_abi_specialization->is_vl_wide_mono &&
                      emit_arg->kind == EX_CALL &&
                      emit_type_is_byvalue_adt(ctx, emit_arg->type)) &&
                    (emit_arg->type.kind == TY_INT ||
                     (type_kind_is_aggregate(emit_arg->type.kind) &&
                      type_uses_carrier_abi(emit_arg->type) &&
                      !expr_emits_byvalue_carrier_abi(ctx, emit_arg)) ||
                     /* CONV-S1 seam 4 (carrier-producer arg -> by-value spec
                      * param): the arg is a direct call to a carrier producer -- an
                      * inline-C / #{Construct} / `__inst_` method whose by-value
                      * ADT-app result (`(Result bool cstr)` / `(Option Device)`
                      * under lowering) is EMITTED as the int64 carrier, so its
                      * `emit_arg->type` reads by-value (type_uses_carrier_abi
                      * false) and the two checks above miss it.  Deref the carrier
                      * into the spec's by-value param -- the call-arg companion of
                      * the let-init / merge carrier->by-value bridges.  REQUIRES no
                      * matched ABI spec on the arg: a by-value spec clone of an
                      * instance method (`__inst_..__spec__Pos`) already returns the
                      * aggregate by value and must NOT be deref'd (it is also an
                      * `__inst_` carrier producer by name, so the spec check is what
                      * distinguishes the two -- the default
                      * typeclass-assoc-type-parametric-struct-element regression). */
                     (emit_arg->kind == EX_CALL &&
                      emit_arg->as.call_.fn_binding &&
                      fn_body_tail_is_carrier_producer(emit_arg) &&
                      !find_matched_abi_spec(ctx, emit_arg,
                                             emit_arg->as.call_.fn_binding) &&
                      !expr_emits_byvalue_carrier_abi(ctx, emit_arg)) ||
                     emit_arg_holds_carrier_byval)) {
                    raw = emit_carrier_bridge(ctx, body, raw,
                                             CK_CARRIER, CK_CONCRETE,
                                             matched_spec->arg_types[i]);
                }
                /* nested-construct-byvalue (float element): the spec param is an
                 * inline `double`, but the argument emits the int64 carrier --
                 * e.g. a constrained-instance accessor (`ok_hyval`) that stayed on
                 * the carrier for a float element of a decoded Option.  A plain
                 * C `int64_t -> double` conversion is NUMERIC (4.6e18 instead of
                 * 3.25); reinterpret the bits via the union bridge instead.  Only
                 * a generic (tyvar-result) carrier accessor with no by-value spec
                 * emits the int64 carrier; a concrete `:float`-returning callee
                 * already yields a `double` and is left untouched. */
                else if (!needs_fn_cast && matched_spec && emit_arg &&
                         !expr_is_pbp_param(ctx, emit_arg) &&
                         (matched_spec->arg_types[i].kind == TY_FLOAT ||
                          matched_spec->arg_types[i].kind == TY_FLOAT32 ||
                          matched_spec->arg_types[i].kind == TY_FLOAT64)) {
                    bool arg_is_carrier_int64 = false;
                    if (emit_arg->kind == EX_CALL &&
                        emit_arg->as.call_.fn_binding &&
                        emit_arg->as.call_.fn_binding->type.kind == TY_FN) {
                        const EmitAbiSpecialization *as = find_matched_abi_spec(
                            ctx, emit_arg, emit_arg->as.call_.fn_binding);
                        if (as) {
                            arg_is_carrier_int64 = strcmp(
                                emit_type_c_name(ctx, as->result_type),
                                "int64_t") == 0;
                        } else {
                            arg_is_carrier_int64 =
                                emit_arg->as.call_.fn_binding->type.as.fn.result_kind
                                    == TY_TYVAR;
                        }
                    }
                    if (arg_is_carrier_int64) {
                        raw = emit_carrier_bridge(ctx, body, raw,
                                                 CK_CARRIER, CK_CONCRETE,
                                                 matched_spec->arg_types[i]);
                    }
                }
                /* constrained-defn-monomorphize (cstr/pointer element): the spec
                 * param is a concrete pointer-carried scalar (`const char *`), but
                 * a generic tyvar-result accessor (`ok-val` of a dict-dispatched
                 * method) emitted the int64 carrier.  Passing the carrier straight
                 * to a `const char *` param trips -Wint-conversion; reinterpret via
                 * intptr_t.  Mirrors the float-element branch above; the bits are
                 * already the pointer, so this is a cast, not a conversion. */
                else if (!needs_fn_cast && matched_spec && emit_arg &&
                         !expr_is_pbp_param(ctx, emit_arg) &&
                         matched_spec->arg_types[i].kind == TY_CSTR) {
                    bool arg_is_carrier_int64 = false;
                    if (emit_arg->kind == EX_CALL &&
                        emit_arg->as.call_.fn_binding &&
                        emit_arg->as.call_.fn_binding->type.kind == TY_FN) {
                        const EmitAbiSpecialization *as = find_matched_abi_spec(
                            ctx, emit_arg, emit_arg->as.call_.fn_binding);
                        if (as) {
                            arg_is_carrier_int64 = strcmp(
                                emit_type_c_name(ctx, as->result_type),
                                "int64_t") == 0;
                        } else {
                            arg_is_carrier_int64 =
                                emit_arg->as.call_.fn_binding->type.as.fn.result_kind
                                    == TY_TYVAR;
                        }
                    }
                    if (arg_is_carrier_int64) {
                        /* The carrier int64 already holds the `const char *`
                         * pointer bits -- a plain reinterpret cast, NOT the
                         * heap-struct deref emit_carrier_bridge would emit for an
                         * aggregate (that derefs the string as a pointer-to-
                         * pointer -> -Warray-bounds + wrong read). */
                        const char *cty = emit_type_c_name(ctx, matched_spec->arg_types[i]);
                        Buf cast; buf_init(&cast);
                        buf_printf(&cast, "(%s)(intptr_t)(%s)", cty, raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    }
                }
                /* Option none-as-NULL retirement (Track A): a `#{Construct}`
                 * result (`some`/`none`/`ok`/`err`, which stay on the int64
                 * carrier base) passed straight into a PLAIN (non-spec) callee
                 * whose declared param is a concrete by-value Option/Result --
                 * e.g. `(takes-byval (some 5))` where `takes-byval`'s param is
                 * `(Option int)` -> by-value `Option__int` -- otherwise emits a
                 * hard cc type error (int64_t vs Option__int).  The spec branch
                 * above only fires for an ABI-specialized callee; a monomorphic
                 * user fn has no `matched_spec`, so bridge here using its declared
                 * param type.  The bridge is NULL-safe for Option (carrier 0 ==
                 * none reconstructs to `(Option__int){0}`, never deref'd), so a
                 * by-value Option consumer can now receive `(none)` without the
                 * historical NULL-deref segfault.
                 *
                 * Scoped TIGHTLY -- only when (a) the arg is itself a call to a
                 * `#{Construct}` template (the some/none/ok/err carrier
                 * producers, the documented gap), AND (b) the param's struct
                 * family is Option or Result (the only families the bridge's
                 * canonical `tur_option_t`/`tur_result_box_t` field-wise
                 * reconstruction handles; every other parametric struct rides
                 * the int64 carrier ABI in param position, so a generic
                 * "concrete aggregate param" gate wrongly deref'd already-correct
                 * carrier values -- the cause of the first cut's 98-fixture
                 * regression). */
                else if (!needs_fn_cast && !matched_spec && emit_arg &&
                         emit_arg->kind == EX_CALL &&
                         emit_arg->as.call_.fn_binding &&
                         emit_arg->as.call_.fn_binding->is_construct_template &&
                         !expr_is_pbp_param(ctx, emit_arg) &&
                         (emit_arg->type.kind == TY_INT ||
                          (type_kind_is_aggregate(emit_arg->type.kind) &&
                           type_uses_carrier_abi(emit_arg->type) &&
                           !expr_emits_byvalue_carrier_abi(ctx, emit_arg))) &&
                         fn_binding && fn_binding->type.kind == TY_FN &&
                         fn_binding->type.as.fn.arg_full_types &&
                         i < fn_binding->type.as.fn.arity &&
                         fn_binding->type.as.fn.arg_full_types[i]) {
                    /* structdef-retirement DS-D: the former carrier->concrete
                     * bridge for a struct-headed Option/Result param
                     * (type_extract_struct_app) never fired once Option/Result
                     * lowered to record ADTs -- no struct-headed app forms.  A
                     * lowered Option/Result construct-template arg already crosses
                     * correctly via the general carrier handling. */
                    (void)0;
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
                 * expr_is_pbp_param is checked first (side-effect-free) as the
                 * primary gate before the type_struct_pass_by_ptr aggregate
                 * check. */
                else if (!needs_fn_cast && emit_arg && !pbp_carrier_cast &&
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
                /* multiword-element-boxing (non-spec callee arg): a CONCRETE
                 * function or typeclass instance method whose declared param is a
                 * WIDE (> 8 byte) by-value ADT -- e.g. `__inst_Show_show_Point(
                 * tur_adt_Point)` -- fed a carrier-producing `(vec-get v i)` arg
                 * receives the int64 box pointer where the aggregate is expected.
                 * The matched_spec ACB bridge (above) covers a spec-call sink; this
                 * is its no-matched_spec companion (a monomorphic user fn or a
                 * concrete instance method has no ABI spec).  Deref-unbox the
                 * carrier into the by-value param.  Gated on the arg being a wide-
                 * byval carrier producer that does NOT already emit the aggregate,
                 * so an arg already materialized by value is untouched. */
                /* The element read is often ascribed to the constraint tyvar
                 * (`(show (:: (vec-get x i) A))` in vec-show-loop), so strip
                 * ascriptions to reach the carrier-producing call. */
                const Expr *mwb_arg = emit_arg;
                while (mwb_arg && mwb_arg->kind == EX_ASCRIBE)
                    mwb_arg = mwb_arg->as.ascribe_.inner;
                /* The callee's CONCRETE param type: for a re-dispatched method
                 * (`show` -> `__inst_Show_show_Point`) the resolved FnDef carries
                 * `param_types[i]` = `Point`; a direct concrete instance method
                 * has no arg_full_types but its arg_kinds[i] is the by-value
                 * aggregate kind, so fall back to the arg's own recovered type. */
                Type mwb_param_ty = {0};
                bool mwb_param_wide = false;
                if (reresolved_callee && i < reresolved_callee->n_params &&
                    reresolved_callee->param_types) {
                    mwb_param_ty = emit_resolve_type(ctx,
                                       reresolved_callee->param_types[i]);
                } else if (fn_binding->type.kind == TY_FN &&
                           i < fn_binding->type.as.fn.arity &&
                           (fn_binding->type.as.fn.arg_kinds[i] == TY_ADT ||
                            fn_binding->type.as.fn.arg_kinds[i] == TY_APP ||
                            fn_binding->type.as.fn.arg_kinds[i] == TY_STRUCT) &&
                           mwb_arg && mwb_arg->kind == EX_CALL) {
                    /* A direct concrete instance method has no reresolved FnDef and
                     * no arg_full_types; recover the target concrete type from the
                     * carrier-producing arg itself. */
                    mwb_param_ty = fn_body_tail_byvalue_carrier_type(ctx, mwb_arg);
                }
                mwb_param_wide = mwb_param_ty.kind != TY_UNKNOWN &&
                                 !type_is_heap_struct(mwb_param_ty) &&
                                 !type_is_heap_adt(mwb_param_ty) &&
                                 type_is_wide_byval_adt(mwb_param_ty);
                /* The arg must actually EMIT the int64 box carrier: a generic
                 * `: A` inline-C accessor (`vec-get`, HAMT val readers) whose
                 * declared result is a bare tyvar and that has no by-value ABI
                 * spec.  This is the storage side of multiword-element-boxing --
                 * the value came out of the collection's int64 element slot. */
                bool mwb_arg_is_carrier_accessor = false;
                if (mwb_arg && mwb_arg->kind == EX_CALL &&
                    mwb_arg->as.call_.fn_binding) {
                    const Binding *acb = mwb_arg->as.call_.fn_binding;
                    if (acb->body_is_inline_c && acb->type.kind == TY_FN &&
                        acb->type.as.fn.result_full_type &&
                        acb->type.as.fn.result_full_type->kind == TY_TYVAR &&
                        !find_matched_abi_spec(ctx, mwb_arg, acb))
                        mwb_arg_is_carrier_accessor = true;
                }
                if (!needs_fn_cast && !matched_spec && !pbp_carrier_cast &&
                    mwb_param_wide && mwb_arg_is_carrier_accessor &&
                    emit_arg && !expr_is_pbp_param(ctx, emit_arg)) {
                    /* A wide (> 16 byte) by-value aggregate param is taken
                     * pass-by-pointer (`const T *`); hand it the box pointer cast
                     * to that pointer (no deref).  A 9..16 byte aggregate is taken
                     * by value; deref-unbox the box to the value. */
                    bool pbp = (mwb_param_ty.kind == TY_ADT)
                        ? adt_byval_pass_by_ptr(mwb_param_ty.as.adt_.def)
                        : adt_app_byval_pass_by_ptr(mwb_param_ty);
                    if (pbp) {
                        const char *cn = emit_type_c_name(ctx, mwb_param_ty);
                        Buf pb; buf_init(&pb);
                        buf_printf(&pb, "(const %s *)(intptr_t)(%s)", cn, raw);
                        buf_putc(&pb, '\0');
                        free(raw);
                        raw = strdup(pb.data);
                        buf_free(&pb);
                    } else {
                        raw = emit_carrier_bridge(ctx, body, raw,
                                                  CK_CARRIER, CK_CONCRETE,
                                                  mwb_param_ty);
                    }
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
                /* end-to-end-monomorphization (typed-pointer producer slice):
                 * the two `concrete->carrier` spill branches below fire on
                 * `arg_kinds[i] == TY_INT`, which treats EVERY carrier-ABI slot
                 * (including a concrete `:heap` collection) as an int64 sink.
                 * But when the callee's DECLARED param is a concrete `:heap`
                 * type (`(Vec int)` / `(MutableMap int int)`), its C signature
                 * is a typed pointer (`Vec__int *`), not int64 -- the typed
                 * value flows in directly, and spilling it to the int64 carrier
                 * emits `callee((int64_t)(intptr_t)(p))` against a pointer
                 * formal (-Wint-conversion).  Detect that case and skip the
                 * spill so the typed pointer passes through unchanged.  A
                 * genuine `:int`-sink consumer (stdlib `some?`/`unwrap-or`,
                 * declared `o : int`) has a non-heap full param type, so it is
                 * unaffected and still spills. */
                bool callee_param_is_typed_heap_ptr = false;
                if (fn_binding->type.kind == TY_FN &&
                    i < fn_binding->type.as.fn.arity &&
                    fn_binding->type.as.fn.arg_full_types &&
                    fn_binding->type.as.fn.arg_full_types[i]) {
                    Type pf = *fn_binding->type.as.fn.arg_full_types[i];
                    if ((type_is_heap_struct(pf) || type_is_heap_adt(pf)) &&
                        type_has_concrete_codegen_layout(&pf))
                        callee_param_is_typed_heap_ptr = true;
                }
                /* KB-021: after standardising carrier-ABI values on the int64_t
                 * carrier, only a by-value struct literal still needs bridging
                 * here; a plain var / call result / temp is already a carrier
                 * int64_t that the impl accepts as-is. */
                if (!needs_fn_cast && !matched_spec &&
                    !callee_param_is_typed_heap_ptr &&
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
                /* option-consumers-typed-as-int-carrier: the same
                 * concrete->carrier bridge, but for an ordinary *direct* call
                 * (dict_arg == NULL) whose callee declares the slot as the
                 * int64 carrier (`o : int`).  The stdlib Option/Result
                 * consumers (`some?`, `unwrap-or`, `option-map`, `option-eq?`,
                 * ...) take their handle as `:int` (the historical carrier ABI),
                 * but a function returning `(Option A)` now hands back the
                 * by-value `Option__A` struct -- passing it straight to the
                 * `int` slot is a hard cc type error.  When the argument
                 * genuinely emits a by-value carrier-ABI aggregate, spill it to
                 * the carrier so the `:int`-sink impl reads the heap-pointer
                 * handle it expects.  The `expr_emits_byvalue_carrier_abi` guard
                 * excludes a plain carrier var/call result (already int64), so
                 * this only fires on a real by-value producer.  The dict path
                 * above keeps its own `dict_arg != NULL` branch unchanged. */
                else if (!needs_fn_cast && !matched_spec &&
                         !callee_param_is_typed_heap_ptr &&
                         e->as.call_.dict_arg == NULL &&
                         emit_arg && type_kind_is_aggregate(emit_arg->type.kind) &&
                         type_kind_is_aggregate(e->as.call_.args[i]->type.kind) &&
                         type_uses_carrier_abi(
                             emit_resolve_type(ctx, e->as.call_.args[i]->type)) &&
                         /* option-map-byvalue-result-into-carrier-consumer-let-
                          * inside-arg: recurse through let/do/if/ascribe wrappers
                          * so a by-value `Option__A` producer buried in a nested
                          * form's tail still triggers the &temp+int64 spill at
                          * the carrier-`int` consumer boundary. */
                         fn_body_tail_emits_byvalue_carrier_abi(ctx, emit_arg) &&
                         fn_binding->type.kind == TY_FN &&
                         i < fn_binding->type.as.fn.arity &&
                         (fn_binding->type.as.fn.arg_kinds[i] == TY_INT ||
                          /* Phase 5: a generic (unspecialized) callee whose
                           * declared param is a carrier-ABI type with an ABSTRACT
                           * element (`o : (Option A)` in option-eq? over a Vec
                           * element) lowers that slot to the int64 carrier even
                           * though its arg_kind reads TY_APP -- detect this by the
                           * param resolving to the `int64_t` carrier C type.  A
                           * MONOMORPHIC callee with a concrete param
                           * (`(Option BoundedIdx)`, `(Pair int int)`) c-names to a
                           * by-value struct and is left alone, so a genuinely
                           * by-value sink is not spuriously spilled. */
                          (fn_binding->type.as.fn.arg_full_types &&
                           fn_binding->type.as.fn.arg_full_types[i] &&
                           type_uses_carrier_abi(emit_resolve_type(ctx,
                               *fn_binding->type.as.fn.arg_full_types[i])) &&
                           strcmp(emit_type_c_name(ctx, emit_resolve_type(ctx,
                               *fn_binding->type.as.fn.arg_full_types[i])),
                               "int64_t") == 0))) {
                    raw = emit_carrier_bridge(ctx, body, raw,
                                             CK_CONCRETE, CK_CARRIER,
                                             e->as.call_.args[i]->type);
                }
                /* inline-c-option-byvalue-carrier-straddle: the FORWARD mirror of
                 * the concrete->carrier spill above.  The argument is a carrier
                 * PRODUCER whose logical type is a by-value carrier-ABI aggregate
                 * -- an inline-C fn (or a form whose tail is one) returning
                 * `(Option T)` / `(Result T E)` as the int64 carrier via
                 * tur_box_* / tur_some_ptr / tur_none -- but the callee's param is
                 * the CONCRETE by-value ADT struct (`tur_adt_Option__String`).
                 * Passing the int64 carrier straight into that by-value slot is a
                 * hard cc type error (`incompatible type ... expected
                 * 'tur_adt_Option__String' ... got 'int64_t'`).  Deref the carrier
                 * into the aggregate -- the exact CK_CARRIER->CK_CONCRETE bridge
                 * the `let` binder applies (init_carrier_to_byval).  Gated by the
                 * same two predicates the binder uses (the arg's by-value carrier
                 * type is known AND the arg does not itself emit the aggregate)
                 * plus the callee's recorded param being a by-value ADT struct
                 * (`tur_adt_...`, not the int64 carrier or a pointer), so an
                 * ordinary carrier-`:int` consumer or an already-by-value arg is
                 * untouched.  See
                 * docs/archive/inline-c-option-byvalue-carrier-straddle.md. */
                else if (!needs_fn_cast && !matched_spec &&
                         !callee_param_is_typed_heap_ptr &&
                         emit_arg && fn_name &&
                         !fn_body_tail_emits_byvalue_carrier_abi(ctx, emit_arg)) {
                    Type arg_bv = fn_body_tail_byvalue_carrier_type(ctx, emit_arg);
                    if (arg_bv.kind != TY_UNKNOWN) {
                        const char *pc = emit_sig_lookup_param_ctype(fn_name, i);
                        if (!pc && fn_binding->type.kind == TY_FN &&
                            i < fn_binding->type.as.fn.arity &&
                            fn_binding->type.as.fn.arg_full_types &&
                            fn_binding->type.as.fn.arg_full_types[i])
                            pc = emit_type_c_name(ctx, emit_resolve_type(ctx,
                                     *fn_binding->type.as.fn.arg_full_types[i]));
                        if (pc && strncmp(pc, "tur_adt_", 8) == 0 &&
                            strchr(pc, '*') == NULL) {
                            raw = emit_carrier_bridge(ctx, body, raw,
                                                      CK_CARRIER, CK_CONCRETE,
                                                      arg_bv);
                        }
                    }
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
                        /* Slice 3: a large by-value ADT param (TY_ADT) uses the
                         * same const-T* convention as a struct/struct-app. */
                        _callee_pbp = (_cpk == TY_STRUCT || _cpk == TY_APP ||
                                       _cpk == TY_ADT) &&
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
                    /* inline-c-option-byvalue-carrier-straddle (pass-by-ptr spill):
                     * a WIDE by-value carrier-ABI aggregate param (`(Result T E)`,
                     * > 16 bytes) is taken `const T *`, so the arg is spilled to a
                     * temp whose address is passed.  When the arg is a carrier
                     * PRODUCER (an inline-C fn returning the int64 carrier), the
                     * bare spill `T __tmp = <int64 carrier>;` is an invalid
                     * initializer -- deref the carrier into the aggregate first,
                     * the same CK_CARRIER->CK_CONCRETE bridge the by-value-param
                     * branch above and the `let` binder apply. */
                    Type _pbp_bv = fn_body_tail_byvalue_carrier_type(ctx, emit_arg);
                    if (_pbp_bv.kind != TY_UNKNOWN &&
                        !fn_body_tail_emits_byvalue_carrier_abi(ctx, emit_arg))
                        raw = emit_carrier_bridge(ctx, body, raw,
                                                  CK_CARRIER, CK_CONCRETE, _pbp_bv);
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
                /* GHE struct-receiver: a constrained-generic method call
                 * (`(render-to b ...)` with `b : B`, `^Backend B`) re-resolved
                 * in this ABI spec to a struct/ADT-receiver instance calls
                 * `__inst_<Class>_<method>_<T>(const T *self, ...)`, but the spec
                 * clone holds the receiver by value.  Spill to a temp and pass
                 * its address so the by-value receiver matches the instance
                 * method's `const T *` formal.  Without this a genuinely-sized
                 * struct receiver passed a `T` to a `const T *` formal -- a hard
                 * cc type error; single-field structs only "worked" by sharing
                 * the int64 carrier's by-value ABI.  Scoped to arg 0 (the
                 * receiver / dispatch tyvar) and to callees the predicate
                 * confirms take the receiver by pointer. */
                if (i == 0 && !needs_fn_cast &&
                    emit_reresolved_receiver_is_by_ptr(ctx, e)) {
                    Type _recv_ty;
                    if (!emit_var_spec_arg_type(ctx, emit_arg, &_recv_ty))
                        _recv_ty = emit_resolve_type(ctx, emit_arg->type);
                    char *_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = %s;\n",
                               emit_type_c_name(ctx, _recv_ty), _tmp, raw);
                    free(raw);
                    Buf _ab; buf_init(&_ab);
                    buf_printf(&_ab, "&%s", _tmp);
                    buf_putc(&_ab, '\0');
                    raw = strdup(_ab.data);
                    buf_free(&_ab);
                    free(_tmp);
                }
                /* end-to-end-monomorphization (bucket A): a `:heap` value that
                 * is emitted as its typed pointer (e.g. a `Vec__int *` from a
                 * vec-new/-push spec, or a let-bound typed-pointer var) must be
                 * bridged to the int64 carrier when the callee slot is the
                 * carrier (the inline-C carrier base, or a generic `A`-element
                 * sink such as `some`/`ok`/`vec-push!`-base).  Without the cast
                 * clang warns (`-Wint-conversion`) or errs on the implicit
                 * pointer->int conversion.  Skip when the matched spec already
                 * types this slot as the `:heap` pointer (the typed consumer
                 * takes `Vec__int *` directly -- no bridge), OR when the
                 * callee's DECLARED param is a concrete `:heap` type (a typed-
                 * pointer formal even without a matched spec, e.g. a user fn
                 * `(defn f [v : (Vec int)] ...)` or `[m : (MutableMap int int)]`
                 * -- the typed value flows in directly). */
                if (emit_arg &&
                    (type_is_heap_struct(emit_resolve_type(ctx, emit_arg->type)) ||
                     type_is_heap_adt(emit_resolve_type(ctx, emit_arg->type))) &&
                    expr_emits_byvalue_carrier_abi(ctx, emit_arg) &&
                    !callee_param_is_typed_heap_ptr &&
                    !(matched_spec && i < matched_spec->n_args &&
                      (type_is_heap_struct(matched_spec->arg_types[i]) ||
                       type_is_heap_adt(matched_spec->arg_types[i])))) {
                    raw = emit_carrier_reinterpret(ctx, raw, NULL, "spec-call-arg");
                }
                /* gcc14-int-conversion (carrier-to-typed-param): cast the arg to
                 * the callee's concrete heap-pointer param type.  The existing gate
                 * skipped a carrier-ABI arg, but a HEAP-pointer arg (a `(Vec int)`
                 * value carried on the int64 carrier, then bridged to int64 by an
                 * upstream block) reports as carrier-ABI yet is passed into a
                 * concrete `tur_adt_Vec__int *` param -- `sum_hyvec((int64_t)...)`
                 * is a -Wint-conversion (hard error under GCC >= 14).  Also fire for
                 * such a heap-struct/heap-adt arg: `(<paramtype>)(intptr_t)(raw)` is
                 * value-preserving whether raw is currently int64 or the pointer. */
                if (emit_arg &&
                    callee_param_is_typed_heap_ptr &&
                    !expr_emits_byvalue_carrier_abi(ctx, emit_arg)) {
                    Type pf = *fn_binding->type.as.fn.arg_full_types[i];
                    const char *pty = emit_type_c_name(ctx, pf);
                    Buf _hb; buf_init(&_hb);
                    buf_printf(&_hb, "(%s)(intptr_t)(%s)", pty, raw);
                    buf_putc(&_hb, '\0');
                    free(raw);
                    raw = strdup(_hb.data);
                    buf_free(&_hb);
                }
                /* gcc14-int-conversion (carrier-to-typed-param): a heap-container
                 * argument (`(Vec int)` value, carried on the int64 carrier and
                 * bridged to int64 upstream) passed into a callee whose DECLARED
                 * parameter is a CONCRETE pointer C type (`sum_hyvec(tur_adt_Vec__int
                 * *)`) is a -Wint-conversion -- a hard error under GCC >= 14.  The
                 * `callee_param_is_typed_heap_ptr` correction above misses it because
                 * a `(Vec ...)` container is not classified as a heap-struct/heap-adt
                 * param.  Key directly on the param's C type being a concrete pointer
                 * (not the int64 carrier, void*, or an RcControlBlock/tyvar-carrier
                 * slot) and cast the arg to it -- value-preserving whether raw is
                 * currently int64 or already the pointer. */
                else if (emit_arg &&
                         fn_binding->type.kind == TY_FN &&
                         i < fn_binding->type.as.fn.arity &&
                         fn_binding->type.as.fn.arg_full_types &&
                         fn_binding->type.as.fn.arg_full_types[i]) {
                    Type raw_at = emit_resolve_type(ctx, emit_arg->type);
                    bool arg_is_heap_ptr = type_is_heap_struct(raw_at) ||
                                           type_is_heap_adt(raw_at) ||
                                           raw_at.kind == TY_APP;
                    Type pf = *fn_binding->type.as.fn.arg_full_types[i];
                    /* gcc14-int-conversion (carrier-representation-tracking):
                     * consult the callee's ACTUAL emitted param C-type (recorded
                     * from the forward-decl pass, ground truth).  When it is the
                     * int64 carrier (a generic carrier-ABI callee such as
                     * `map-hamt [K V]` whose `(Map K V)` param emits int64_t), the
                     * arg was correctly bridged to int64 upstream and must NOT be
                     * re-cast to a concrete pointer -- the monomorphized `pf`
                     * c-names to `tur_adt_X *` and would fool the cast below.  Only
                     * fire when the recorded param is genuinely a concrete pointer
                     * (a concrete callee such as `size-of (m : (Map int int))`). */
                    const char *rec_pty = emit_sig_lookup_param_ctype(fn_name, i);
                    bool callee_real_param_is_carrier =
                        rec_pty && !(strlen(rec_pty) >= 1 &&
                                     rec_pty[strlen(rec_pty) - 1] == '*');
                    if (arg_is_heap_ptr && !callee_real_param_is_carrier &&
                        pf.kind != TY_TYVAR &&
                        pf.kind != TY_FORALL && pf.kind != TY_EXISTS) {
                        const char *pty = emit_type_c_name(ctx, pf);
                        size_t L = pty ? strlen(pty) : 0;
                        if (L >= 2 && pty[L - 1] == '*' &&
                            strcmp(pty, "void *") != 0 &&
                            strncmp(pty, "int64_t", 7) != 0 &&
                            strncmp(pty, "RcControlBlock", 14) != 0) {
                            Buf _hb; buf_init(&_hb);
                            buf_printf(&_hb, "(%s)(intptr_t)(%s)", pty, raw);
                            buf_putc(&_hb, '\0');
                            free(raw);
                            raw = strdup(_hb.data);
                            buf_free(&_hb);
                        }
                    }
                }
                /* gcc14-int-conversion (carrier-representation-tracking, reverse
                 * at a spec-dispatch call): the RECORDED callee param is the int64
                 * carrier but the arg EMITS as a concrete pointer -- e.g.
                 * `__inst_Eq_..._int64_t(a, b)` where `b` is a
                 * `tur_adt_Map__cstr__int *` spec param passed into the int64 slot.
                 * `pointer -> int64 param` is a hard error under GCC >= 14.  Uses
                 * the emit_sig ground truth keyed by fn_name (now populated for ABI
                 * specs too), so it fires even when arg_full_types is absent.  Gated
                 * on the arg genuinely emitting a concrete pointer and not already
                 * bridged, so an already-int64 carrier arg is untouched. */
                if (emit_arg && fn_name && raw &&
                    strncmp(raw, "(int64_t)", 9) != 0) {
                    const char *rec_c = emit_sig_lookup_param_ctype(fn_name, i);
                    if (rec_c && strcmp(rec_c, "int64_t") == 0) {
                        const char *acty = emit_binding_repr_c_name(
                            ctx, emit_arg->type, emit_arg);
                        size_t aL = acty ? strlen(acty) : 0;
                        /* the arg genuinely emits a pointer value: a concrete
                         * `tur_adt_X *`, a bare `void *` (a captured env field such
                         * as a channel handle `void * chp`, or a `void *`-returning
                         * value), OR an existential pack whose emitted form is a
                         * `(tur_exists_t)(..)` void* (tur_exists_t is void*).  All
                         * are `pointer -> int64 param` and must reinterpret; passing
                         * a `void *` env field bare into an `int64_t` param (e.g.
                         * `chan_hysend(env->chp, ...)`) is a -Wint-conversion hard
                         * error under macOS clang / GCC >= 14. */
                        bool arg_is_conc_ptr = acty && aL >= 1 &&
                            acty[aL - 1] == '*' && strcmp(acty, "void *") != 0;
                        bool arg_is_voidp = acty && strcmp(acty, "void *") == 0;
                        bool arg_is_exists = strncmp(raw, "(tur_exists_t)", 14) == 0;
                        if (arg_is_conc_ptr || arg_is_voidp || arg_is_exists) {
                            raw = emit_carrier_reinterpret(ctx, raw, NULL, "spec-call-heap-arg");
                        }
                    }
                    /* gcc14-int-conversion (carrier-representation-tracking,
                     * forward at a spec-dispatch call): the mirror of the reverse
                     * cast -- the RECORDED callee param is a CONCRETE pointer but
                     * the arg EMITS as the int64 carrier (e.g. a `cstr` element
                     * carried on int64 passed into `__inst_Eq_eq_qu_cstr(const char
                     * *)` / `__inst_Show_show_cstr`).  `int64 arg -> pointer param`
                     * is a hard error under GCC >= 14.  Cast the int64 arg to the
                     * recorded concrete pointer type.  Gated on the arg emitting as
                     * int64 (repr c-name) so an arg already the pointer is
                     * untouched; the RcControlBlock carrier slot is excluded. */
                    else if (rec_c) {
                        size_t rL = strlen(rec_c);
                        /* hkt-foldable-rc-param: `RcControlBlock *` used to be
                         * excluded here.  It has to be bridged like any other
                         * concrete pointer now that an HKT instance body can hold
                         * its receiver as a real `rc<a>` whose ABI slot is still
                         * the int64 carrier -- `_un_unrc_hyvalue(ta)` with `ta`
                         * declared `int64_t` is a -Wint-conversion otherwise.  The
                         * `acty == "int64_t"` guard below already restricts this to
                         * an arg that genuinely emits as the carrier, so an rc arg
                         * that is already the pointer is untouched either way. */
                        bool rec_is_conc_ptr = rL >= 2 && rec_c[rL - 1] == '*' &&
                            strcmp(rec_c, "void *") != 0;
                        const char *acty = emit_binding_repr_c_name(
                            ctx, emit_arg->type, emit_arg);
                        /* hkt-foldable-rc-param: emit_binding_repr_c_name only
                         * models CARRIER-ABI types -- it returns the by-value
                         * c-name immediately for anything else, so an `rc<a>`
                         * binding reports `RcControlBlock *` and never reveals
                         * that its parameter slot is really the int64 carrier.
                         * emit_carrier_holds_ptr is the flag that does record
                         * exactly that, so consult it as a second signal. */
                        bool arg_slot_is_carrier =
                            emit_arg->kind == EX_VAR && emit_arg->as.var.binding &&
                            emit_arg->as.var.binding->emit_carrier_holds_ptr;
                        if (rec_is_conc_ptr &&
                            ((acty && strcmp(acty, "int64_t") == 0) ||
                             arg_slot_is_carrier)) {
                            Buf _fb; buf_init(&_fb);
                            buf_printf(&_fb, "(%s)(intptr_t)(%s)", rec_c, raw);
                            buf_putc(&_fb, '\0');
                            free(raw);
                            raw = strdup(_fb.data);
                            buf_free(&_fb);
                        }
                    }
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
            /* SR1: the readback half of the `:int`-carrier-parameter bridge.  A
             * callee declared `: int` that really hands back a sum -- `(defn
             * unroll [fix : int] : int ...)` in stdlib/fix.tur -- returns the
             * int64 carrier, while the caller's expression type is the by-value
             * ADT the patterns name.  Deref the box the producing side stored (see
             * the int-carrier-param bridge in the arg loop above), so the two ends
             * of the crossing agree.  Excluded: a transparent int newtype, whose
             * carrier and concrete forms are the same bare int64 -- derefing that
             * would read through a value, not a pointer -- and a matched ABI spec,
             * whose clone already returns the aggregate by value. */
            if (g_sr1_sum_byvalue &&
                fn_binding && fn_binding->type.kind == TY_FN &&
                emit_type_is_byvalue_adt(ctx, e->type) &&
                !type_is_transparent_int_newtype(emit_resolve_type(ctx, e->type)) &&
                !find_matched_abi_spec(ctx, e, fn_binding)) {
                TypeKind rk = fn_binding->type.as.fn.result_kind;
                /* Emitted signature first, declared kind as the fallback -- same
                 * reason as the argument side: a `: int` return refined to the
                 * ADT comes back as the aggregate already, and derefing it would
                 * read through a value rather than a pointer. */
                const char *rct = emit_sig_lookup_ret_ctype(fn_name);
                bool ret_is_carrier_word =
                    (rct && *rct)
                        ? (strcmp(rct, "int64_t") == 0)
                        : (rk == TY_INT || rk == TY_INT64 || rk == TY_UINT64 ||
                           rk == TY_PTR_VOID);
                if (ret_is_carrier_word) {
                    char *ub = emit_agg_unbox(ctx, e->type, result);
                    free(result);
                    result = ub;
                    /* The unbox retypes the expression, and the panic-hoist in
                     * emit_value types its `__ps_N` temp from this note -- without
                     * it the hoist falls back to reading the emitted text and
                     * misreads the deref's own `*(T *)` as the cast-wrap form.
                     * Same re-note the poly-carrier unbox site performs. */
                    note_call_ret(ctx, emit_type_c_name(ctx, e->type));
                }
            }
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
            /* struct-of-closures monomorphization: match this closure against the
             * outer spec's primary inner-closure link AND its extra links, so
             * EVERY closure a `(make-struct S clo1 clo2 ...)` return builds picks
             * its own suffixed env + clone -- not just the first. */
            const EmitAbiSpecialization *_isp =
                emit_inner_closure_spec_for_binding(ctx, _cur_spec,
                                                    closure->fn->binding);
            if (_isp) {
                if (_isp->env_name_override) env_name = _isp->env_name_override;
                thunk_sym_override = _isp->clone_name;
            }

            /* Emit env struct type definition + drop glue at file scope if
             * not already emitted.  Shared with the CPS twin pre-pass in
             * emit_cps_ir.c, which must define the struct BEFORE an
             * earlier-in-file __cps body reads captures through it.
             *
             * Route through pending_handler_fns when available: during
             * emit_fn_def, ctx->file points INSIDE the current function's
             * body buffer, and a `static void drop_glue_...` at block scope
             * is invalid C.  pending_handler_fns drains to file scope AHEAD
             * of the function being emitted -- exactly the position the
             * struct and glue need.  (Previously this fallback wrote to
             * ctx->file and survived only because emission order always let
             * a file-scope site win the dedup guard first.) */
            emit_closure_env_struct_and_glue(
                ctx,
                ctx->pending_handler_fns ? ctx->pending_handler_fns : ctx->file,
                closure, env_name, thunk_sym_override != NULL);

            /* Phase HKT §5: heap-allocate the fat closure struct so that the
             * fat pointer can safely escape from the local stack frame and be
             * passed to HKT helpers as an opaque int64_t.  Layout:
             *   struct __env_N { int64_t __fn; <captures...> }
             * The __fn field holds the thunk pointer (as int64_t).  Callers
             * using the generic fat-closure protocol recover the thunk as:
             *   thunk = (int64_t(*)(void*,int64_t))(intptr_t)fat->__fn
             * and invoke it as thunk(fat_ptr, arg). */
            Type thunk_result = emit_resolve_type(ctx, emit_fn_result_type_from_type(closure->fn->binding->type));
            uint32_t thunk_arity = closure->fn->n_params > 0 ? (uint32_t)(closure->fn->n_params - 1) : 0;
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
            {
                /* closure-drop-glue (Model R): allocate an 8-byte drop-glue
                 * header BEFORE the env and hand back a pointer PAST it, so every
                 * existing use (dispatch via fat[0], capture access by field, the
                 * fat handle that escapes) is byte-identical to the headerless
                 * layout -- only env[-1] is new.  env[-1] holds drop_glue_env_N;
                 * TUR_CLOSURE_DROP(handle) recovers it to release the env. */
                char *base_tmp = fresh_tmp(ctx);
                buf_printf(body,
                    "void *%s = malloc(sizeof(void *) + sizeof(struct %s));\n",
                    base_tmp, env_name->name);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "*(void (**)(void *))%s = drop_glue_%s;\n",
                    base_tmp, env_name->name);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "struct %s *%s = (struct %s *)((char *)%s + sizeof(void *));\n",
                    env_name->name, fat_tmp, env_name->name, base_tmp);
                free(base_tmp);
            }
            indent_buf(body, ctx->indent);
            if (thunk_typedef) {
                buf_printf(body, "%s->__fn = (%s)%s;\n", fat_tmp, thunk_typedef, thunk_sym);
            } else {
                buf_printf(body, "%s->__fn = (int64_t)(intptr_t)%s;\n", fat_tmp, thunk_sym);
            }
            for (uint8_t i = 0; i < closure->n_captures; i++) {
                Binding *captured = closure->captures[i];
                /* Edge 1: skip an eagerly-captured letrec member that became a
                 * captureless global (see the env-struct emission above) -- it
                 * has no env field and is reached by its C symbol directly. */
                if (captured && captured->is_global) continue;
                char *field = raw_name_for_binding(captured);
                /* Edge 1 (hkt-matcher-cata-...): when a NESTED closure captures
                 * the letrec self binding -- i.e. `captured` is bound to the very
                 * closure whose body we are emitting right now -- its runtime
                 * value is this closure's own env box, reachable here only as the
                 * env pointer the thunk received as its first parameter (the
                 * outer-scope `self_NNNN` local is out of scope inside the lifted
                 * thunk).  Store that env pointer (mirrors the S5 self-*call*
                 * box-name rule).  The capture field is the int64_t fn carrier, so
                 * bridge the `void *` env pointer through intptr_t. */
                if (ctx->closure && ctx->closure->fn &&
                    captured->closure_fn_binding &&
                    captured->closure_fn_binding == ctx->closure->fn->binding &&
                    ctx->closure->fn->n_params > 0) {
                    char *envp = raw_name_for_binding(ctx->closure->fn->params[0]);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s->%s = (int64_t)(intptr_t)%s;\n",
                               fat_tmp, field, envp);
                    free(envp);
                    free(field);
                    continue;
                }
                char *cn = name_for_binding(ctx, captured);
                indent_buf(body, ctx->indent);
                /* B5: a captured struct/ADT that is a pass-by-pointer parameter
                 * of the *enclosing* function arrives as `const T *`, but the
                 * env field is declared by value (type_c_name => `T`).  Deref so
                 * the closure stores its own copy of the value rather than the
                 * caller's pointer (which would dangle once the frame returns).
                 * See docs/archive/history/stdlib-type-erasure-cleanup-plan.md (B5). */
                bool captured_is_pbp = false;
                for (uint32_t _p = 0; _p < ctx->n_pbp_params; _p++) {
                    if (ctx->pbp_param_ptrs[_p] == captured) {
                        captured_is_pbp = true;
                        break;
                    }
                }
                /* MB2 (constrained-hkt-forall-mode-b-plan): a dict-clone's
                 * poly-carrier param is held as the int64 carrier, but its env
                 * field is the concrete pointer C type (`tur_adt_Point *`).  Cast
                 * the carrier through intptr_t so the capture is not a
                 * -Wint-conversion (pointer->intptr_t->pointer is a no-op for the
                 * non-carrier case, so this only bites carrier-held params). */
                if (captured->emit_carrier_holds_ptr && !captured_is_pbp) {
                    const char *fcty = emit_type_c_name(ctx, captured->type);
                    buf_printf(body, "%s->%s = (%s)(intptr_t)%s;\n",
                               fat_tmp, field, fcty, cn);
                } else {
                    buf_printf(body, "%s->%s = %s%s;\n",
                               fat_tmp, field, captured_is_pbp ? "*" : "", cn);
                }
                /* closure-drop-glue (Model R) walk slice: an rc-typed capture is a
                 * SHARED owning reference.  Flag-on, RETAIN it at capture (a strong
                 * increment) so the closure holds its own count -- balancing the
                 * decrement the env's drop-glue performs when the closure dies.
                 * This is finding-#1's "retain when duplicated", done for the
                 * refcounted capture kind where it is unconditionally sound: rc
                 * counting handles aliasing, so no move/uniqueness analysis is
                 * needed and an ESCAPING rc-capturing closure no longer dangles
                 * (flag-off it borrows and the source's auto-drop frees the rc out
                 * from under the escaped closure).  Owning captures that are NOT
                 * refcounted (a raw nested-closure handle, a ref) still need move
                 * analysis and are left to the next slice. */
                if (captured->type.kind == TY_RC) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "if (%s->%s) rc_strong_increment(%s->%s);\n",
                               fat_tmp, field, fat_tmp, field);
                }
                /* A Drop-typeclass capture is MOVED into the env (no retain) -- the
                 * source is consumed at elab, so the stored handle is the sole owner
                 * and the drop-glue releases it once. */
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

            /* A `:heap` ADT is already represented as a POINTER to its payload
             * (its ctor mallocs and returns `T *`), so boxing it the generic way
             * -- malloc a cell and store the pointer in it -- leaves cb->value a
             * `T **`. Every consumer (field read/write, walk glue, drop glue)
             * casts cb->value straight to `T *`, so that extra indirection made
             * them read the pointer cell as if it were the struct: `->next`
             * yielded the struct pointer, which then reached
             * rc_strong_decrement as a bogus control block (a crash once the
             * collector touched fields past weak_count), the walker traced
             * garbage, and drop glue freed the cell while leaking the struct.
             *
             * cb->value must point AT the payload, so for a heap ADT adopt the
             * pointer the ctor already produced. See
             * docs/archive/gc-heap-struct-rc-not-a-control-block.md. */
            bool payload_is_heap_adt =
                e->as.rc_of_.expr->type.kind == TY_ADT &&
                e->as.rc_of_.expr->type.as.adt_.def &&
                e->as.rc_of_.expr->type.as.adt_.def->is_heap;

            /* rc-of-sum-type-drops-no-glue: a MULTI-VARIANT ADT is the same
             * shape.  Its ctor mallocs the tag+union record and hands back a
             * pointer as an int64 carrier, so wrapping that in another malloc'd
             * cell leaves cb->value pointing at a cell that *holds* the pointer
             * rather than at the record.  The drop glue then casts the cell,
             * reads the pointer bits as `s->tag`, and matches no case -- so the
             * owning fields were never released even once the glue existed.
             * Adopt the ctor's pointer directly, exactly as the heap case does.
             *
             * rc-of-adt-leaks-the-payload: this used to also require
             * `needs_drop_glue`, which made the fix above apply only to sums
             * carrying an owning field.  A sum WITHOUT one has the identical
             * shape -- its ctor mallocs the record and hands back a pointer as
             * an int64 carrier -- so it kept the wrapper path: cb->value
             * pointed at an 8-byte cell holding the pointer, the default drop
             * glue freed that cell, and the record itself was orphaned.
             * `(rc/of (PA 7))` leaked 16 bytes on every construction.
             *
             * Drop glue is not what makes the pointer adoptable; being a boxed
             * record is.  With the pointer adopted and no explicit glue,
             * rc_set_value re-derives default_rc_drop_fn, which frees exactly
             * the record -- and the wrapper allocation disappears with it. */
            bool payload_is_boxed_adt =
                e->as.rc_of_.expr->type.kind == TY_ADT &&
                e->as.rc_of_.expr->type.as.adt_.def &&
                !e->as.rc_of_.expr->type.as.adt_.def->is_heap &&
                !adt_is_byvalue_product(e->as.rc_of_.expr->type.as.adt_.def);

            char *val_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            if (payload_is_boxed_adt) {
                buf_printf(body, "%s *%s = (%s *)(intptr_t)(%s);\n",
                           inner_type_c, val_tmp, inner_type_c, inner);
            } else if (payload_is_heap_adt) {
                buf_printf(body, "%s %s = %s;\n", inner_type_c, val_tmp, inner);
            } else {
                /* Emit: allocate value separately, then attach it to rc control block. */
                buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                           inner_type_c, val_tmp, inner_type_c, inner_type_c);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*%s = %s;\n", val_tmp, inner);
            }

            char *cb_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Phase 11 / DS3: if the inner value is a struct with rc fields,
             * pass its drop glue and -- so the cycle walker can trace
             * through -- its walk glue via rc_cb_alloc_struct. */
            const char *drop_fn_name = "NULL";
            char dg_name_buf[256];
            char wg_name_buf[256];
            bool struct_with_rc_fields = false;
            /* structdef-retirement DS-C: the TY_STRUCT rc/of arm is dead -- a
             * struct value is a record ADT now (handled below). */
            if (e->as.rc_of_.expr->type.kind == TY_ADT) {
                /* CONV-S1 (slice 2): a by-value ADT is laid out like a struct, so
                 * an `rc/of` over one with rc/ref/weak fields needs the same
                 * field-releasing drop/walk glue (keyed off the C type name). */
                AdtDef *adef = e->as.rc_of_.expr->type.as.adt_.def;
                /* rc-of-sum-type-drops-no-glue: this used to also require
                 * adt_is_byvalue_product(adef), so a MULTI-VARIANT ADT with an
                 * owning field fell through to the else branch below with
                 * drop_fn_name still at its initial "NULL" -- no drop glue and
                 * no walker.  The rc payload was never released and the walker
                 * could not trace through the box.  The glue emitter now
                 * dispatches on the tag, so both shapes are covered. */
                if (adef && adef->needs_drop_glue) {
                    char *mn = mangle_field_name(adef->name);
                    snprintf(dg_name_buf, sizeof(dg_name_buf), "drop_glue_tur_adt_%s", mn);
                    snprintf(wg_name_buf, sizeof(wg_name_buf), "walk_glue_tur_adt_%s", mn);
                    free(mn);
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
            /* rc-scalar-default-glue-invalid-free: repoint through rc_set_value
             * rather than assigning cb->value directly.  With no explicit glue
             * (drop_fn_name == "NULL") it re-derives the defaulted drop glue
             * for a payload that is now a SEPARATE allocation -- rc_cb_alloc's
             * own default assumes the inline (cb + 1) payload and must not
             * free(), so a raw assignment would keep that no-op glue and leak
             * the cell on every drop.  An explicit struct drop glue is passed
             * through unchanged, exactly as the alloc installed it. */
            buf_printf(body, "rc_set_value(%s, %s, %s);\n", cb_tmp, val_tmp, drop_fn_name);

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
                /* Path (a): fn-expr is a function value.  A THIN (bare) fn is a
                 * plain function pointer -- spawn it directly.  A FAT (boxed)
                 * closure -- a capturing lambda, EX_CLOSURE `{__fn, captures...}`
                 * -- is NOT a function pointer: calling the box as code crashes
                 * (docs/archive/compiled-async-capturing-closure-segfault.md).
                 * Route it to the env-taking spawn, which reads the thunk out of
                 * the box and invokes it with the box as its env. */
                char *fn_val = emit_value(ctx, body, fn_expr);
                indent_buf(body, ctx->indent);
                if (fn_expr->type.as.fn.boxed) {
                    buf_printf(body, "void *%s = (void *)tur_async_fiber_closure((void *)(intptr_t)%s);\n", tmp, fn_val);
                } else {
                    buf_printf(body, "void *%s = (void *)tur_async_fiber((int64_t(*)(void))(intptr_t)%s);\n", tmp, fn_val);
                }
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
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tur_stm_begin(__tx);\n");

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
            
            /* retry / check-false: park on the read set, then re-run */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (__tx->retry_requested) {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tur_stm_park(__tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "continue;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");

            /* Read conflict observed mid-body: restart immediately */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (__tx->aborted) {\n");
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
            bool saved_lv = ctx->lvalue_mode;
            ctx->lvalue_mode = true;  /* borrow-struct-field: field operand as lvalue */
            char *inner = emit_value(ctx, body, e->as.borrow_immut_.expr);
            ctx->lvalue_mode = saved_lv;
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
            bool saved_lv_m = ctx->lvalue_mode;
            ctx->lvalue_mode = true;  /* borrow-struct-field: field operand as lvalue */
            char *inner = emit_value(ctx, body, e->as.borrow_mut_.expr);
            ctx->lvalue_mode = saved_lv_m;
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
            /* S1b/cleanup: the guard pointers, kept alive so the scope-exit pop
             * can also be emitted EXPLICITLY after the body.  c2mir discards
             * __attribute__((cleanup)) with no diagnostic (findings 3.1), so
             * relying on it alone left every dynamic binding un-popped under
             * the JIT -- `dynvar-nested` printed `3 3` for `3 1`.  The pop is
             * idempotent, so the attribute still covers the exits this cannot
             * see (a `return` or `goto` out of the block) on the cc path. */
            char **guard_ptrs = n_pairs ? (char **)calloc(n_pairs, sizeof(char *)) : NULL;
            char **guard_vars = n_pairs ? (char **)calloc(n_pairs, sizeof(char *)) : NULL;
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
                if (guard_ptrs) {
                    guard_ptrs[pi] = fptr; guard_vars[pi] = mname;
                    /* Register so an early `return` inside the body pops it. */
                    if (ctx->n_dynvar_guards >= ctx->cap_dynvar_guards) {
                        ctx->cap_dynvar_guards = ctx->cap_dynvar_guards ? ctx->cap_dynvar_guards * 2 : 8;
                        ctx->dynvar_guard_ptrs = (char **)realloc(ctx->dynvar_guard_ptrs,
                            ctx->cap_dynvar_guards * sizeof(char *));
                        ctx->dynvar_guard_names = (char **)realloc(ctx->dynvar_guard_names,
                            ctx->cap_dynvar_guards * sizeof(char *));
                    }
                    ctx->dynvar_guard_ptrs[ctx->n_dynvar_guards] = strdup(fptr);
                    ctx->dynvar_guard_names[ctx->n_dynvar_guards] = strdup(mname);
                    ctx->n_dynvar_guards++;
                }
                else { free(fptr); free(mname); }
                free(vslot);
                free(frame);
            }
            /* Emit body */
            char *bval = emit_value(ctx, body, e->as.dynvar_binding_.body);
            if (!nil_result) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result_tmp, bval);
            }
            free(bval);
            /* Leaving the binding scope: unregister the guards an early
             * `return` inside the body would have popped. */
            for (uint32_t pi = 0; pi < n_pairs && ctx->n_dynvar_guards > 0; pi++) {
                ctx->n_dynvar_guards--;
                free(ctx->dynvar_guard_ptrs[ctx->n_dynvar_guards]);
                free(ctx->dynvar_guard_names[ctx->n_dynvar_guards]);
            }
            /* Explicit scope-exit pops, reverse declaration order -- the order
             * the cleanup attribute itself would have used. */
            for (uint32_t pi = n_pairs; pi-- > 0 && guard_ptrs; ) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "_dynvar_pop_%s(&%s);\n", guard_vars[pi], guard_ptrs[pi]);
                free(guard_ptrs[pi]);
                free(guard_vars[pi]);
            }
            free(guard_ptrs);
            free(guard_vars);
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
        case EX_PERFORM:
        case EX_WITH_HANDLER:
            /* v2 invariant (fiber effect runtime deleted, Stage G): a `perform` and a
             * handler-VALUE `with-handler` lower on the DK backend (emit_cps_ir),
             * never the direct/fiber emitter -- their fiber emitters
             * (emit_effects_perform / emit_effects_with_handler) emitted now-deleted
             * runtime symbols and are removed.  So there is genuinely nothing to
             * emit here, and emitting an undefined reference would be worse.
             * (EX_HANDLE keeps its emitter: a non-effect `handle` shape -- e.g. the
             * contract/MonadError lowering -- still has a live direct path.)
             *
             * This used to `abort()` on the strength of a "corpus-verified
             * unreachable" claim.  It is reachable: a `handle` clause whose body
             * leaves the CPS backend's admissible subset evicts the enclosing
             * function, and the clause's `perform` then lands here.  An ICE gives
             * the user no location and no way to act, so report it as a located
             * codegen error instead -- emit_program already fails the build on
             * diag_had_error().  See
             * docs/archive/handler-clause-statement-if-ices-emitter.md for the
             * shapes that still reach this. */
            diag_emit(DIAG_ERROR, e->span,
                      "this effect operation has no lowering here: the enclosing "
                      "function left the CPS backend's supported subset, and the "
                      "direct emitter cannot lower `perform`. The usual cause is a "
                      "loop inside a `handle` clause -- hoist that work into a "
                      "helper function and call it from the clause. This is a "
                      "compiler limitation, not a mistake in this expression.");
            return atom_nil();
        case EX_HANDLE:          return emit_effects_handle(ctx, body, e);
        case EX_HANDLER_LIT:     return emit_effects_handler_lit(ctx, body, e);
        case EX_COMPOSE_HANDLERS: return emit_effects_compose_handlers(ctx, body, e);
        case EX_RESUME:          return emit_effects_resume(ctx, body, e);
        case EX_DISCONTINUE:     return emit_effects_discontinue(ctx, body, e);
        case EX_MAKE_STRUCT:
            /* structdef-retirement DS-D: every record type lowers to a
             * single-variant ADT and `(make-struct ...)` is rewritten to the
             * auto-bound variant constructor during elaboration (elab_structs.c),
             * so no EX_MAKE_STRUCT node is ever produced.  Reaching here means a
             * StructDef-era node leaked into emit -- an internal invariant break. */
            fprintf(stderr, "tur: internal error: EX_MAKE_STRUCT reached emit_value "
                    "(structdef-retirement: make-struct lowers to a variant ctor)\n");
            abort();
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
            /* borrow-struct-field: read-and-clear the borrow lvalue request so it
             * applies only to THIS field access (the borrow operand), not the
             * nested receiver emit below. */
            bool gf_lvalue = ctx->lvalue_mode;
            ctx->lvalue_mode = false;
            char *sv = emit_value(ctx, body, e->as.get_field_.struct_expr);
            /* SC7: a transparent int newtype (including the lowered record-ADT
             * form, `(defstruct Schema [A] (raw :int))`) IS its single int64
             * field -- the access is the identity.  Catch it here, before the
             * ADT positional/named member-path branch, so `(.raw (fmap s f))`
             * over a `(Schema int)` result reads the int64 carrier directly
             * rather than `.as.Schema._0` on a non-aggregate value.  Mirrors the
             * TY_STRUCT transparent-newtype shortcut further below. */
            if (type_is_transparent_int_newtype(
                    e->as.get_field_.struct_expr->type)) {
                return sv;
            }
            /* CONV-S0/S4: receiver is a single-variant record ADT (def == NULL).
             * The value is the heap-pointer int64 carrier; read the field out of
             * the sole variant's union member.  The flat-product typedef has no
             * tag word but keeps `union { struct { ... } Ctor; } as`, so the
             * `->as.Ctor._<idx>` path is unchanged from the tagged case. */
            if (e->as.get_field_.adt_def) {
                const AdtDef *adt = e->as.get_field_.adt_def;
                const CtorDef *ctor = e->as.get_field_.adt_ctor;
                char *adt_mn = mangle_field_name(adt->name);
                char *mctor  = mangle_field_name(ctor->name);
                /* CONV-S1 seam 4: member-access path -- a flat named record reads
                 * `.value`; a tagged/positional ADT reads `.as.<Ctor>._N`. */
                char *mp = adt_field_member_path(adt, ctor, e->as.get_field_.field_idx);
                const char *cty = type_c_name(e->type);
                /* CONV-S1 seam 4 (fn-field carrier read): a lowered record-ADT
                 * stores a `fn` field -- bare OR fully-typed -- as the int64
                 * carrier (the signature feeds type checking, never layout; the
                 * capability call specialises the function-pointer type from the
                 * call's arg/result types via the intptr_t-cast path).  But
                 * type_c_name(TY_FN) returns the fn's RESULT C name (a bare
                 * function reference convention), so a sub-word result type
                 * (`(fn [int32] int32)`) yields `int32_t` -- and casting the int64
                 * carrier field through `(int32_t)` TRUNCATES the function pointer
                 * to 32 bits, jumping to a wild address at the call.  An int-result
                 * fn happens to dodge this (cty == int64_t, an identity cast), which
                 * is why only sub-word fn fields segfaulted.  Read the carrier at
                 * its real storage width (int64_t) regardless of the fn's result
                 * width; the EX_CALL fn_expr path re-specialises the pointer to the
                 * concrete arg/result signature.  Both the outer `cty` and the
                 * spec-recovery `fld_rcty` derive from the same truncating
                 * type_c_name, so force the carrier width below once fld_rcty is in
                 * hand. */
                bool fn_carrier_field = (e->type.kind == TY_FN);
                if (fn_carrier_field) cty = "int64_t";
                /* CONV-S1 seam 4 (accessor-unbox): inside an ABI spec a generic
                 * accessor reads a TYPE-ERASED field (`(.snd t)` over `(Tuple2 A
                 * B)`).  e->type collapsed the field's tyvar to the int64 carrier
                 * (cty == "int64_t"), but the spec's bindings resolve it to a
                 * concrete BY-VALUE aggregate (`(Tuple2 cstr int)`).  The field is
                 * stored as the int64 B4 box pointer, so the read must DEREF-unbox
                 * it to the aggregate instead of returning the int64.  Gated on the
                 * e->type-vs-spec disagreement, so it is inert wherever no spec
                 * resolution changes the field type (the default path). */
                Type fld_rty = emit_resolve_type(ctx, e->type);
                bool fld_rty_owned = false;
                /* constrained-instance-element-dispatch (lowered record ADT
                 * receiver): the receiver may be a spec param whose ACTIVE arg
                 * type is a concrete by-value ADT app (`(Option cstr)`) while its
                 * declared/use type is the bare carrier ADT (`Option`).  e->type
                 * collapsed the field to the int64 carrier, so the resolve above
                 * left fld_rty int64.  Recover the field's concrete element by
                 * substituting the receiver app's args into the ctor field's
                 * declared full_type, and remember the receiver is by-value so the
                 * read goes directly off the aggregate (below). */
                Type recv_spec_ty = {0};
                bool have_recv_spec = false;
                bool recv_spec_byval_adt = false;
                {
                    const Expr *rv0 = e->as.get_field_.struct_expr;
                    while (rv0 && rv0->kind == EX_ASCRIBE)
                        rv0 = rv0->as.ascribe_.inner;
                    if (rv0 && rv0->kind == EX_VAR &&
                        emit_var_spec_arg_type(ctx, rv0, &recv_spec_ty) &&
                        recv_spec_ty.kind == TY_APP &&
                        type_app_is_concrete_adt(&recv_spec_ty)) {
                        have_recv_spec = true;
                        /* by-value (non-heap) receiver reads its field directly off
                         * the aggregate; a :heap receiver derefs the monomorph
                         * pointer (heap_adt_recv path below) -- both need the
                         * recovered concrete element field type. */
                        recv_spec_byval_adt = !type_is_heap_adt(recv_spec_ty);
                        const char *frc0 = emit_type_c_name(ctx, fld_rty);
                        if (ctor && e->as.get_field_.field_idx < ctor->n_fields &&
                            frc0 && strcmp(frc0, "int64_t") == 0) {
                            AdtDef *rad = NULL; Type raargs[16]; uint8_t ran = 0;
                            const CtorField *rcf =
                                &ctor->fields[e->as.get_field_.field_idx];
                            if (rcf->full_type &&
                                rcf->full_type->kind == TY_TYVAR &&
                                type_extract_adt_app(&recv_spec_ty, &rad, raargs,
                                                     &ran) && rad) {
                                Type sub = substitute_adt_app_type_owned(
                                    rcf->full_type, rad, raargs);
                                if (sub.kind != TY_TYVAR &&
                                    sub.kind != TY_UNKNOWN) {
                                    fld_rty = sub;
                                    fld_rty_owned = (sub.kind == TY_APP);
                                } else {
                                    free_struct_app_type(sub);
                                }
                            }
                        }
                    }
                }
                const char *fld_rcty = emit_type_c_name(ctx, fld_rty);
                /* fn-field carrier read (above): the fn field is the int64 carrier,
                 * so the spec-recovery cast width is int64_t, not the fn's
                 * (possibly sub-word) result C name.  Forcing it here keeps the
                 * eff_fld_rcty / field_byval_unbox paths from re-introducing the
                 * truncating cast. */
                if (fn_carrier_field) fld_rcty = "int64_t";
                bool field_byval_unbox =
                    cty && strcmp(cty, "int64_t") == 0 &&
                    fld_rcty && strcmp(fld_rcty, "int64_t") != 0 &&
                    (fld_rty.kind == TY_APP || fld_rty.kind == TY_STRUCT) &&
                    !type_is_heap_struct(fld_rty) && !type_is_heap_adt(fld_rty) &&
                    ctor && e->as.get_field_.field_idx < ctor->n_fields &&
                    !adt_field_is_inline_byval(&ctor->fields[e->as.get_field_.field_idx]);
                bool adt_through_rc =
                    e->as.get_field_.struct_expr->type.kind == TY_RC;
                /* Parametric-by-value: the receiver may be a concrete flat-product
                 * ADT-app monomorph (`(Box int)`, TY_APP) rather than a
                 * non-parametric TY_ADT.  Both are by-value aggregates, but
                 * adt_is_byvalue_product() takes an AdtDef* and rejects a
                 * parametric def (n_type_params > 0).  Decide by the receiver's
                 * TYPE via the app-aware predicate so a parametric monomorph reads
                 * its field directly off the aggregate instead of falling through
                 * to the (wrong) carrier-pointer deref. */
                /* CONV-S1 seam 4 (carrier-held by-value-ADT receiver): the
                 * receiver's USE type is a by-value ADT (`(Option cstr)` under
                 * lowering) but it is HELD as the int64 carrier -- e.g. a typeclass
                 * instance method's dispatch class-var param `x` declared
                 * `int64_t` (the uniform dict ABI) that the call site spilled +
                 * addressed.  Reading the field off the by-value aggregate
                 * (`(x).as...`) is wrong; cast the int64 carrier to the receiver's
                 * concrete monomorph pointer and deref.  Detect via the receiver
                 * VAR's BINDING type lowering to int64 (not flagged as a by-value
                 * carrier param) while its use type is a by-value aggregate.  Inert
                 * at default: there the same use type is the int64 carrier, so
                 * emit_type_is_byvalue_adt is false. */
                bool recv_held_as_carrier = false;
                {
                    const Expr *rv = e->as.get_field_.struct_expr;
                    while (rv && rv->kind == EX_ASCRIBE) rv = rv->as.ascribe_.inner;
                    if (rv && rv->kind == EX_VAR && rv->as.var.binding)
                        recv_held_as_carrier =
                            rv->as.var.binding->emit_carrier_holds_byval;
                }
                /* applied-unary-instance-head: emit_carrier_holds_byval is set on
                 * the param binding while emitting the CARRIER BASE (`x` is the
                 * int64 carrier there) and persists on the shared binding into the
                 * BY-VALUE spec emission.  When the active spec passes this param
                 * as a concrete by-value ADT app (`tur_adt_Option__cstr x`), the
                 * carrier-pointer deref (`(intptr_t)(x)` on an aggregate -- a hard
                 * cc error) is wrong; the field reads directly off the aggregate.
                 * Suppress the stale flag for this emission. */
                if (recv_spec_byval_adt) recv_held_as_carrier = false;
                /* multiword-element-boxing: the receiver is a CALL to a generic
                 * `: A` inline-C accessor (`(vec-get v i)`, the HAMT val readers)
                 * whose result is a WIDE (> 8 byte) by-value ADT.  Such a call
                 * EMITS the int64 carrier (a heap-box pointer), not the aggregate
                 * -- so `sv` here is the box pointer, and reading `.x` straight off
                 * it (the adt_recv_byvalue path) is a hard cc error.  Detect it and
                 * route into the recv_carrier_byval deref path (cast the int64 to
                 * the receiver's concrete monomorph pointer and read `->field`).
                 * Excludes a receiver that already emits the aggregate by value. */
                bool recv_call_carrier_byval = false;
                {
                    const Expr *rv = e->as.get_field_.struct_expr;
                    while (rv && rv->kind == EX_ASCRIBE) rv = rv->as.ascribe_.inner;
                    if (rv && rv->kind == EX_CALL &&
                        !fn_body_tail_emits_byvalue_carrier_abi(ctx, rv)) {
                        Type bvt = fn_body_tail_byvalue_carrier_type(ctx, rv);
                        /* Increment 3: any-width container elements are boxed
                         * now, so the deref-read applies to narrow (<= 8 byte)
                         * products too -- same shared predicate as the push
                         * bridges and the let-binding read recovery.  A
                         * concrete by-value accessor result still returns
                         * UNKNOWN from the walk above, so it never routes
                         * here. */
                        if (bvt.kind != TY_UNKNOWN &&
                            !type_is_heap_struct(bvt) && !type_is_heap_adt(bvt) &&
                            (type_is_wide_byval_adt(bvt) ||
                             type_is_boxed_container_elem(bvt)))
                            recv_call_carrier_byval = true;
                    }
                }
                bool recv_carrier_byval =
                    (recv_held_as_carrier &&
                     emit_type_is_byvalue_adt(ctx,
                         e->as.get_field_.struct_expr->type)) ||
                    recv_call_carrier_byval;
                bool adt_recv_byvalue =
                    (emit_type_is_byvalue_adt(ctx, e->as.get_field_.struct_expr->type)
                     || recv_spec_byval_adt) &&
                    !recv_held_as_carrier && !recv_call_carrier_byval;
                /* CONV-S1 seam 4 (:heap ADT receiver, concrete element): a
                 * `(defstruct Cons :heap [A] (head A) (tail :int))` lowers to a
                 * :heap record ADT whose monomorph cell stores a by-value
                 * aggregate element INLINE (`tur_adt_Cons__Option__int`).
                 * `(.head xs)` must cast the heap pointer to that MONOMORPH and
                 * read the field at its real (aggregate) type, not the generic
                 * `tur_adt_Cons` carrier (whose `_0` is int64) with an int64->
                 * aggregate cast.  Gate the c-name (which REGISTERS the monomorph
                 * as a side effect) on the cheap is_heap_adt check so a non-heap
                 * receiver never reaches it and unrelated monomorphization is
                 * undisturbed. */
                const char *heap_recv_cn = NULL;
                bool heap_adt_recv = false;
                if (type_is_heap_adt(e->as.get_field_.struct_expr->type)) {
                    /* Prefer the spec-recovered concrete receiver app (`(Cons
                     * float)` -> `tur_adt_Cons__float *`) so a constrained
                     * instance spec reads its :heap field off the right monomorph
                     * rather than the element-erased generic `tur_adt_Cons *`
                     * (whose `_0` is int64). */
                    Type heap_recv_rt = (have_recv_spec &&
                                         type_is_heap_adt(recv_spec_ty))
                        ? recv_spec_ty
                        : emit_resolve_type(ctx,
                              e->as.get_field_.struct_expr->type);
                    if (type_is_heap_adt(heap_recv_rt)) {
                        heap_recv_cn = emit_type_c_name(ctx, heap_recv_rt);
                        heap_adt_recv = heap_recv_cn &&
                            strchr(heap_recv_cn, '*') != NULL;
                    }
                }
                Buf hb; buf_init(&hb);
                if (adt_through_rc) {
                    /* CONV-S1 (slice 2): rc<ADT> receiver.  rc/of mallocs
                     * `type_c_name(ADT)` and stores the value there, so the
                     * rc-block's value pointer either addresses the by-value
                     * aggregate directly, or (for a carrier ADT, type_c_name ==
                     * int64_t) holds the int64 carrier that points at it.  Match
                     * the two layouts so the deref reaches the variant field. */
                    if (adt_is_byvalue_product(adt)) {
                        buf_printf(&hb,
                                   "(%s)((tur_adt_%s *)((RcControlBlock *)(%s))->value)->%s",
                                   cty, adt_mn, sv, mp);
                    } else {
                        buf_printf(&hb,
                                   "(%s)((tur_adt_%s *)(intptr_t)(*(int64_t *)((RcControlBlock *)(%s))->value))->%s",
                                   cty, adt_mn, sv, mp);
                    }
                } else if (recv_carrier_byval) {
                    /* carrier-held by-value-ADT receiver: cast the int64 carrier to
                     * the receiver's concrete monomorph pointer and deref (above). */
                    const char *recv_cn = emit_type_c_name(ctx,
                        emit_resolve_type(ctx, e->as.get_field_.struct_expr->type));
                    buf_printf(&hb, "(%s)((%s *)(intptr_t)(%s))->%s",
                               cty, recv_cn, sv, mp);
                } else if (adt_recv_byvalue) {
                    /* CONV-S1: by-value receiver -- read the field directly off the
                     * aggregate, no carrier pointer deref.  Gated by
                     * emit_type_is_byvalue_adt (app-aware: leaf products as of B3,
                     * and concrete parametric monomorphs).
                     * Slice 3: a large by-value ADT param arrives pass-by-pointer
                     * (`const tur_adt_X *`), so a pbp receiver reads through `->`. */
                    const Expr *adt_recv = e->as.get_field_.struct_expr;
                    while (adt_recv->kind == EX_ASCRIBE)
                        adt_recv = adt_recv->as.ascribe_.inner;
                    bool pbp = expr_is_pbp_param(ctx, adt_recv);
                    /* CONV-S1 seam 3: a :heap ADT receiver is a typed POINTER to
                     * its heap header (`tur_adt_Vec__int *`), so read through `->`
                     * exactly as a pass-by-pointer receiver does. */
                    Type recv_rt = emit_resolve_type(ctx,
                        e->as.get_field_.struct_expr->type);
                    bool heap_recv =
                        (recv_rt.kind == TY_ADT && recv_rt.as.adt_.def &&
                         recv_rt.as.adt_.def->is_heap) ||
                        (recv_rt.kind == TY_APP && type_adt_app_def(&recv_rt) &&
                         type_adt_app_def(&recv_rt)->is_heap);
                    bool use_arrow = pbp || heap_recv;
                    /* slice 4: an inline by-value aggregate field is already its
                     * own aggregate value in the union slot -- read it with no cast
                     * (a cast-to-aggregate is invalid C) and no carrier deref. */
                    bool inline_byval = ctor &&
                        e->as.get_field_.field_idx < ctor->n_fields &&
                        adt_field_is_inline_byval(&ctor->fields[e->as.get_field_.field_idx]);
                    /* construct-template accessor unbox (by-value receiver): the
                     * field's spec-resolved CONCRETE type (fld_rty -- `User`/`Point`
                     * when the accessor spec `ok_val__spec__tur_adt_User_...` is
                     * active and resolves the bare-tyvar field) is a by-value
                     * aggregate the generic `cty` (int64) cast would corrupt.  A
                     * narrow element is stored INLINE (read the aggregate directly);
                     * a WIDE element is the int64 box pointer (deref it); a scalar
                     * element (cstr/float) casts to the element type. */
                    Type eff_fld_rty = fld_rty;
                    const char *eff_fld_rcty =
                        (fld_rcty && strcmp(fld_rcty, "int64_t") != 0
                         && cty && strcmp(cty, "int64_t") == 0)
                            ? fld_rcty : NULL;
                    bool rec_aggregate = eff_fld_rcty &&
                        (eff_fld_rty.kind == TY_APP ||
                         eff_fld_rty.kind == TY_STRUCT ||
                         eff_fld_rty.kind == TY_ADT) &&
                        !type_is_heap_struct(eff_fld_rty) &&
                        !type_is_heap_adt(eff_fld_rty);
                    bool rec_wide = rec_aggregate &&
                        type_is_wide_byval_adt(eff_fld_rty);
                    const char *acc = use_arrow ? "->" : ".";
                    /* CONV-S1 seam 4 (typedef ordering): a lowered Result/Option
                     * monomorph stores a non-parametric value-struct field as a
                     * heap pointer `T *` (adt_field_c_type) -- so the accessor must
                     * DEREF the slot to recover the `T` value, not cast it.  Mirrors
                     * the construction-side box.  Inert at default. */
                    /* structdef-retirement slice 1 / DS-C: a by-value record-ADT
                     * field rides as `tur_adt_T *`; deref the slot to recover the
                     * `T` value.  The former TY_STRUCT disjunct is dead. */
                    bool ros_ptr_slot = adt && adt->name &&
                        (strcmp(adt->name, "Result") == 0 ||
                         strcmp(adt->name, "Option") == 0) &&
                         (fld_rty.kind == TY_ADT && fld_rty.as.adt_.def &&
                          !fld_rty.as.adt_.def->is_heap &&
                          fld_rty.as.adt_.def->n_type_params == 0 &&
                          adt_is_byvalue_product(fld_rty.as.adt_.def));
                    if (ros_ptr_slot) {
                        buf_printf(&hb, "(*(%s)%s%s)", sv, acc, mp);
                    } else if (rec_wide) {
                        buf_printf(&hb, "(*(%s *)(intptr_t)((%s)%s%s))",
                                   eff_fld_rcty, sv, acc, mp);
                    } else if (inline_byval || rec_aggregate) {
                        buf_printf(&hb, "(%s)%s%s", sv, acc, mp);
                    } else if (eff_fld_rcty) {
                        buf_printf(&hb, "(%s)(%s)%s%s", eff_fld_rcty, sv, acc, mp);
                    } else if (use_arrow) {
                        if (gf_lvalue) buf_printf(&hb, "(%s)->%s", sv, mp);
                        else buf_printf(&hb, "(%s)(%s)->%s", cty, sv, mp);
                    } else {
                        /* borrow-struct-field: drop the outer rvalue cast so the
                         * member is a bare lvalue for `&`. */
                        if (gf_lvalue) buf_printf(&hb, "(%s).%s", sv, mp);
                        else buf_printf(&hb, "(%s)(%s).%s", cty, sv, mp);
                    }
                } else if (heap_adt_recv) {
                    /* :heap ADT receiver with a concrete monomorph layout: cast
                     * the heap pointer to the MONOMORPH cell type and read the
                     * field at its real C type.  An aggregate field is stored
                     * inline (read directly, no cast -- a cast-to-aggregate is
                     * invalid C); a scalar/pointer field casts to the SPEC-
                     * RECOVERED element type (fld_rcty -- `double`/`const char *`)
                     * when the erased carrier `cty` is int64, so a float/cstr
                     * element is not value-truncated or pointer-mangled through an
                     * int64 cast. */
                    bool fld_aggregate =
                        (fld_rty.kind == TY_APP || fld_rty.kind == TY_STRUCT ||
                         fld_rty.kind == TY_ADT) &&
                        !type_is_heap_struct(fld_rty) && !type_is_heap_adt(fld_rty);
                    const char *heap_fld_cast =
                        (fld_rcty && strcmp(fld_rcty, "int64_t") != 0 &&
                         cty && strcmp(cty, "int64_t") == 0)
                            ? fld_rcty : cty;
                    if (fld_aggregate)
                        buf_printf(&hb, "((%s)(intptr_t)(%s))->%s",
                                   heap_recv_cn, sv, mp);
                    else
                        buf_printf(&hb, "(%s)((%s)(intptr_t)(%s))->%s",
                                   heap_fld_cast, heap_recv_cn, sv, mp);
                } else if (field_byval_unbox) {
                    /* deref-unbox the carrier-stored B4 box pointer to the spec's
                     * concrete by-value aggregate (accessor-unbox, above). */
                    buf_printf(&hb, "(*(%s *)(intptr_t)(((tur_adt_%s *)(intptr_t)(%s))->%s))",
                               fld_rcty, adt_mn, sv, mp);
                } else if (gf_lvalue) {
                    /* borrow-struct-field: bare member lvalue (no rvalue cast). */
                    buf_printf(&hb, "((tur_adt_%s *)(intptr_t)(%s))->%s",
                               adt_mn, sv, mp);
                } else if ((fld_rty.kind == TY_FLOAT || fld_rty.kind == TY_FLOAT32 ||
                            fld_rty.kind == TY_FLOAT64) &&
                           fld_rcty && strcmp(fld_rcty, "int64_t") != 0 &&
                           cty && strcmp(cty, "int64_t") == 0) {
                    /* ok-val-untyped-catch-box-loses-float: the ERASED
                     * Result/Option carrier declares `int64_t ok_val`, and a
                     * float payload rides in it as its BITS -- that is the
                     * contract the typed construction path honours
                     * (`((union { int64_t s; double d; }){.s = ...}).d`,
                     * emit_core.c).  Reading it through the erased struct and
                     * letting C convert int64 -> double converts the bit
                     * pattern NUMERICALLY instead: `(ok-val r)` on an
                     * unannotated `(catch-unwind (fn [] : float 7.5))` gave
                     * 4.62013e+18.  Reinterpret, as the other consumer does. */
                    buf_printf(&hb,
                        "((union { int64_t s; %s d; }){.s = ((tur_adt_%s *)(intptr_t)(%s))->%s}).d",
                        fld_rcty, adt_mn, sv, mp);
                } else {
                    buf_printf(&hb, "(%s)((tur_adt_%s *)(intptr_t)(%s))->%s",
                               cty, adt_mn, sv, mp);
                }
                buf_putc(&hb, '\0');
                free(sv); free(adt_mn); free(mctor); free(mp);
                if (fld_rty_owned) free_struct_app_type(fld_rty);
                char *r = strdup(hb.data);
                buf_free(&hb);
                return r;
            }
            /* structdef-retirement: the former StructDef field-access path is
             * dead.  Every record now lowers to a single-variant record ADT
             * (handled by the adt_def branch above), and a transparent int
             * newtype is caught by the identity shortcut earlier.  Reaching
             * here means the receiver resolved to neither a record ADT nor a
             * transparent newtype -- an internal invariant break. */
            fprintf(stderr, "tur: emit: EX_GET_FIELD with no backing record ADT\n");
            free(sv);
            abort();
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
                    /* nested-bind-over-result-typed-boundary: a CAPTURING
                     * continuation that returns a by-value aggregate is the fat
                     * twin of the named-wrapper spill gate below.  `do-m` over
                     * `(Result A B)` produces one as soon as there are two
                     * binds: the inner `(fn [b] (ok (+ a b)))` captures `a`, so
                     * it lowers to a fat closure whose `__fn` returns
                     * `tur_adt_Result__int__int` by value -- but the carrier
                     * base instance (`__inst_Monad_bind_Result_tyvar`) invokes
                     * it through `(int64_t(*)(void*,int64_t))k.fn`.  Route it
                     * through a signature-keyed shim that boxes the aggregate
                     * to the int64 carrier the consumer actually reads back.
                     * (The outer, non-capturing continuation goes through the
                     * named-wrapper path and was already paired correctly.) */
                    char *fat_spill = NULL;
                    if (thunk_binding && thunk_binding->type.kind == TY_FN) {
                        thunk_arity = thunk_binding->type.as.fn.arity > 0
                            ? (uint8_t)(thunk_binding->type.as.fn.arity - 1) : 0;
                        for (uint8_t i = 0; i < thunk_arity; i++) {
                            thunk_params[i] = emit_fn_arg_type_from_type(thunk_binding->type, (uint8_t)(i + 1));
                        }
                        Type thunk_result =
                            emit_fn_result_type_from_type(thunk_binding->type);
                        thunk_typedef = ensure_typed_thunk_typedef(
                            ctx, ctx->file, thunk_result,
                            thunk_arity ? thunk_params : NULL,
                            thunk_arity);
                        if (e->as.poly_wrap_.boxes_aggregate ||
                            ctx->poly_wrap_callee_carrier) {
                            /* Resolve `(F A)` to its concrete monomorph so the
                             * by-value-layout check sees a real aggregate (a
                             * raw TY_APP has no by-value layout on its own) --
                             * same resolve the named-wrapper gate performs. */
                            Type spill_result = emit_resolve_type(ctx, thunk_result);
                            Type spill_params[MAX_FN_ARITY];
                            for (uint8_t i = 0; i < thunk_arity; i++)
                                spill_params[i] = emit_resolve_type(ctx, thunk_params[i]);
                            fat_spill = ensure_fat_aggregate_spill_shim(
                                ctx, spill_result,
                                thunk_arity ? spill_params : NULL, thunk_arity);
                        }
                    }
                    /* let-bound-noncapturing-lambda-segfaults-as-fn-arg: a
                     * `:fn` value has three lowerings, and this branch assumed
                     * two of them.  A capturing closure and a forwarded fn
                     * parameter are boxes whose slot 0 is the code pointer; a
                     * let-bound NON-capturing lambda is the raw
                     * `int64_t (*)(int64_t)` with no box at all, so the slot-0
                     * read below dereferenced the function's own machine code
                     * and jumped to it (SIGSEGV, no diagnostic, while the
                     * interpreter got it right).
                     *
                     * Detect it by the one thing that separates the shapes: an
                     * UNBOXED TY_FN binding.  A capturing closure is boxed or
                     * :ptr<void>; a fn-typed parameter arrives as a
                     * tur_poly_fn_t and is boxed too.  Route the unboxed case
                     * through a signature-keyed adapter instead of teaching
                     * the consumer a second calling convention. */
                    char *bare_shim = NULL;
                    if (!fat_spill && !thunk_typedef) {
                        const Expr *bi = e->as.poly_wrap_.inner;
                        while (bi && bi->kind == EX_ASCRIBE) bi = bi->as.ascribe_.inner;
                        const Binding *bb = (bi && bi->kind == EX_VAR)
                            ? bi->as.var.binding : NULL;
                        if (bb && bb->type.kind == TY_FN && !bb->type.as.fn.boxed &&
                            !bb->closure_fn_binding && bb->type.as.fn.arity > 0 &&
                            bb->type.as.fn.arity <= MAX_FN_ARITY) {
                            uint8_t bn = (uint8_t)bb->type.as.fn.arity;
                            Type bparams[MAX_FN_ARITY];
                            for (uint8_t bi2 = 0; bi2 < bn; bi2++)
                                bparams[bi2] = emit_resolve_type(
                                    ctx, emit_fn_arg_type_from_type(bb->type, bi2));
                            Type bres = emit_resolve_type(
                                ctx, emit_fn_result_type_from_type(bb->type));
                            bare_shim = ensure_bare_fnptr_poly_shim(
                                ctx, bres, bn ? bparams : NULL, bn);
                        }
                    }
                    indent_buf(body, ctx->indent);
                    /* The closure value may be carried as int64_t (a let-bound
                     * :ptr<void> closure value) or as a void *; cast through
                     * intptr_t so both forms convert without an int-to-pointer
                     * warning. */
                    buf_printf(body, "void *%s = (void *)(intptr_t)(%s);\n", tmp, fat);
                    free(fat);
                    Buf out; buf_init(&out);
                    if (fat_spill) {
                        buf_printf(&out, "(tur_poly_fn_t){ %s, %s }", tmp, fat_spill);
                    } else if (thunk_typedef) {
                        buf_printf(&out,
                            "(tur_poly_fn_t){ %s, "
                            "(int64_t(*)(void*,int64_t))(*( %s *)(%s)) }",
                            tmp, thunk_typedef, tmp);
                    } else if (bare_shim) {
                        /* The value is a bare C function pointer, not a box:
                         * carry it in the env slot and let the shim cast it
                         * back to its env-less signature.  Reading slot 0 here
                         * would read the function's own machine code. */
                        buf_printf(&out, "(tur_poly_fn_t){ %s, "
                                         "(int64_t(*)(void*,int64_t))%s }",
                                   tmp, bare_shim);
                    } else {
                        buf_printf(&out,
                            "(tur_poly_fn_t){ %s, "
                            "(int64_t(*)(void*,int64_t))(intptr_t)((int64_t*)%s)[0] }",
                            tmp, tmp);
                    }
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(bare_shim);
                    free(fat_spill);
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
            /* M7: if the wrapper RETURNS a by-value aggregate (a Monad/HKT
             * continuation returning `(m b)`), it cannot be cast to the int64
             * `tur_poly_fn_t.fn` ABI without corrupting the struct return -- route
             * it through a carrier-spill shim that boxes the aggregate. */
            const Binding *wbnd = e->as.poly_wrap_.wrapper_binding;
            char *spill = NULL;
            /* Slice 3 (constrained-hkt-forall codegen): only a FORALL carrier sink
             * boxes an aggregate return (the generic int64 carrier consumer
             * unboxes it).  A typed `:fn` carrier / monad continuation is consumed
             * BY VALUE via a concrete-cast call site, so it must NOT be spilled --
             * gating on boxes_aggregate keeps that path byte-for-byte unchanged. */
            if ((e->as.poly_wrap_.boxes_aggregate ||
                 /* increment 2 (bind cell): the enclosing call's resolved
                  * callee is the carrier base entry -- see the flag's setter
                  * in the call-arg loop.  The shim is defensive (NULL unless
                  * the wrapper really returns a by-value aggregate). */
                 ctx->poly_wrap_callee_carrier) &&
                wbnd && wbnd->type.kind == TY_FN) {
                Type wres = wbnd->type.as.fn.result_full_type
                    ? *wbnd->type.as.fn.result_full_type
                    : emit_type_from_kind(wbnd->type.as.fn.result_kind);
                /* Slice 3 (constrained-hkt-forall codegen): resolve a parametric
                 * `(F A)` result to its concrete monomorph (TY_ADT) so the spill
                 * shim's concrete-layout check recognizes a by-value aggregate
                 * return (a raw TY_APP has no by-value layout on its own). */
                wres = emit_resolve_type(ctx, wres);
                uint32_t warity = wbnd->type.as.fn.arity;
                /* params after the env slot (index 0) */
                Type wparams[MAX_FN_ARITY];
                uint8_t nwp = 0;
                for (uint8_t i = 1; i < warity && nwp < MAX_FN_ARITY; i++)
                    wparams[nwp++] = emit_fn_arg_type_from_type(wbnd->type, i);
                spill = ensure_aggregate_spill_shim(ctx, wn, wres, wparams, nwp);
            }
            /* E2 (fat-closure fn-value threading): when the wrapped inner fn is
             * EFFECTFUL (CPS-colored -- it performs or calls a colored fn), emit a
             * `<wrapper>__cps` twin and populate the fat closure's `fn_cps`
             * DK-threading slot, so a call through a fat-closure poly-fn param
             * dispatches the callback onto the caller's trampoline instead of a
             * fresh root (which would leave the effect unhandled).  Restricted to a
             * single `int`/`int64` arg + result (the twin's fixed int64 ABI); an
             * aggregate-result carrier (spill) is excluded. */
            char *fn_cps_name = NULL;
            if (!spill) {
                const Expr *inner = e->as.poly_wrap_.inner;
                while (inner && inner->kind == EX_ASCRIBE) inner = inner->as.ascribe_.inner;
                const Binding *ib = (inner && inner->kind == EX_VAR)
                    ? inner->as.var.binding : NULL;
                if (ib && ib->source_binding) ib = ib->source_binding;
                /* Gate on the wrapped fn being EFFECTFUL (a non-empty effect row):
                 * it performs / propagates an effect, so it is CPS-colored and has
                 * a registered `__cps` entry the twin threads through.  A pure fn
                 * (empty / no row) needs no twin -- the fat closure's direct `.fn`
                 * path is unchanged (no fixture churn, no fn_cps slot).  The
                 * pass-inferred row (FnDef.inferred_effect_row) is the reliable
                 * signal -- a `defn`'s binding TY_FN often carries no declared row;
                 * fall back to the declared row if inference has not run. */
                const struct EffectRow *er = NULL;
                if (ib && ib->source_fn_def && ib->source_fn_def->inferred_effect_row)
                    er = ib->source_fn_def->inferred_effect_row;
                else if (ib && ib->type.kind == TY_FN)
                    er = ib->type.as.fn.effect_row;
                bool effectful = er && er->kind != ERK_EMPTY;
                /* The twin force-declares the wrapped fn as `int64_t <fn>(int64_t)`
                 * (emit_module.c) and dispatches its int64 `__cps` entry, so the
                 * wrapped fn's arg AND result must both be a plain `int`/`int64`
                 * (spelled `int64_t` in C).  A wider kind would mismatch the twin's
                 * forward decl -- exclude it (stays on the delegated direct path). */
                if (ib && ib->is_global && effectful
                    && ib->type.kind == TY_FN && ib->type.as.fn.arity == 1) {
                    TypeKind ak = ib->type.as.fn.arg_kinds[0];
                    TypeKind rk = ib->type.as.fn.result_kind;
                    bool ak_ok = (ak == TY_INT || ak == TY_INT64);
                    bool rk_ok = (rk == TY_INT || rk == TY_INT64);
                    if (ak_ok && rk_ok) {
                        char *iname = raw_name_for_binding(ib);
                        char *twin = ensure_poly_wrap_cps_thunk(ctx, wn, iname);
                        /* twin==NULL means already emitted -- reuse the name. */
                        if (twin) { fn_cps_name = twin; }
                        else {
                            Buf tn; buf_init(&tn);
                            buf_printf(&tn, "%s__cps", wn);
                            buf_putc(&tn, '\0');
                            fn_cps_name = strdup(tn.data);
                            buf_free(&tn);
                        }
                        free(iname);
                    }
                }
            }
            Buf out; buf_init(&out);
            if (fn_cps_name)
                buf_printf(&out, "(tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))%s, %s }",
                           spill ? spill : wn, fn_cps_name);
            else
                buf_printf(&out, "(tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))%s }",
                           spill ? spill : wn);
            buf_putc(&out, '\0');
            free(wn);
            free(spill);
            free(fn_cps_name);
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
            uint32_t arity = (fnty.kind == TY_FN) ? fnty.as.fn.arity : 0;

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

            /* repr-trace: a bare fn crossing into a fat sink -- the shim
             * bridge is where the representation changes hands. */
            if (g_emit_abi_trace) {
                fprintf(stderr, "repr-trace %u:%u bridge bare-to-fat arity=%u %s\n",
                        e->span.line, e->span.col_start, (unsigned)arity,
                        typed_shim ? "typed-shim" : "int64-shim");
            }

            /* fn-value-fat-normalization: when the boxed value is a file-scope
             * FUNCTION, the whole box is a constant -- hoist it to a statically
             * allocated one instead of mallocing a fresh copy on every
             * execution.  Nothing drops a box handed to a normalized fn param,
             * so the per-execution form is an unbounded leak in a loop, not
             * just an allocation (5e6 iterations of `(apply1 add3 acc)` leaked
             * 122 MiB).  Restricted to `is_global` bindings: a local fn binding
             * emits a plain identifier too, but its address is not a
             * link-time constant, and a computed fn value never is.  Across the
             * fixture corpus this covers 4307 of 4334 boxing sites. */
            if (e->as.fn_to_fat_.static_ok &&
                inner->kind == EX_VAR && inner->as.var.binding &&
                inner->as.var.binding->is_global &&
                !inner->as.var.binding->closure_fn_binding &&
                !inner->as.var.binding->is_param &&
                !inner->as.var.binding->is_poly_fn &&
                !inner->as.var.binding->is_fat) {
                char shim_name[64];
                if (!typed_shim)
                    snprintf(shim_name, sizeof shim_name, "__tur_fatshim%u",
                             (unsigned)arity);
                const char *box = ensure_static_fatbox(
                    ctx, typed_shim ? typed_shim : shim_name, fnptr);
                if (box) {
                    if (g_emit_abi_trace)
                        fprintf(stderr, "repr-trace %u:%u bridge bare-to-fat "
                                        "static-box %s\n",
                                e->span.line, e->span.col_start, box);
                    char *sb_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                               "void *%s = (void *)((char *)&%s + sizeof(void *));\n",
                               sb_tmp, box);
                    free(fnptr);
                    free(typed_shim);
                    return sb_tmp;
                }
            }

            char *fat_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            {
                /* closure-drop-glue (Model R): give the bare-fn-to-fat box the same
                 * env[-1] drop-glue header as a capturing env, so TUR_CLOSURE_DROP
                 * releases ANY fat handle uniformly (a captured handle may be either
                 * representation).  A `{shim, orig_fn}` box owns nothing, so its
                 * header is NULL -> tur_closure_drop frees the base allocation. */
                char *base_tmp = fresh_tmp(ctx);
                buf_printf(body, "void *%s = malloc(sizeof(void *) + 2 * sizeof(int64_t));\n",
                           base_tmp);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*(void (**)(void *))%s = 0;\n", base_tmp);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t *%s = (int64_t *)((char *)%s + sizeof(void *));\n",
                           fat_tmp, base_tmp);
                free(base_tmp);
            }
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
            /* repr-trace: a poly-carrier {env,fn} value boxed into a fat
             * handle for a ^fat sink. */
            if (g_emit_abi_trace) {
                fprintf(stderr, "repr-trace %u:%u bridge poly-to-fat\n",
                        e->span.line, e->span.col_start);
            }
            char *pv = emit_value(ctx, body, inner);
            char *pf = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_poly_fn_t %s = %s;\n", pf, pv);
            char *box = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            {
                /* closure-drop-glue (Model R): header the poly-to-fat box too so
                 * TUR_CLOSURE_DROP is uniform across every fat representation.
                 * Header NULL -> tur_closure_drop frees the base (freeing the box
                 * only, as today; the boxed inner env, if any, is not walked here). */
                char *pbase = fresh_tmp(ctx);
                buf_printf(body, "void *%s = malloc(sizeof(void *) + 3 * sizeof(int64_t));\n",
                           pbase);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*(void (**)(void *))%s = 0;\n", pbase);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t *%s = (int64_t *)((char *)%s + sizeof(void *));\n",
                           box, pbase);
                free(pbase);
            }
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
             * See docs/archive/history/parameterized-defopaque.md. */
            bool ascribe_to_opaque =
                /* structdef-retirement slice 5: an opaque newtype is now a
                 * TY_ADT with is_opaque -- ascribing into it (`(:: 7 :Tag)`) is
                 * the same pure int64-carrier relabel as the struct-opaque case,
                 * never a by-value aggregate carrier bridge. */
                (e->type.kind == TY_ADT && e->type.as.adt_.def &&
                 e->type.as.adt_.def->is_opaque) ||
                e->type.kind == TY_TYVAR;
            /* constrained-generic-dispatch-float-element: `(:: (vec-get v i) A)`
             * -- the documented carrier-helper ascription idiom -- recovers the
             * element type `A` for dispatch, but when `A` monomorphizes to a
             * FLOAT width the int64 carrier read from `vec-get` must be
             * bit-reinterpreted to a `double`, exactly as the concrete
             * `(:: ... :float)` ascription is via the method-arg bridge.  Treated
             * as a pure tyvar relabel (ascribe_to_opaque), the raw int64 bits
             * flowed through unreinterpreted, so a `1.5` element arrived as its
             * int64 bit pattern (4.6e18) inside the dispatched method.  A plain
             * C int64->double assignment is a NUMERIC conversion, not a
             * reinterpret, so resolve the tyvar through the active spec and, when
             * it grounds to a float width over an int64 carrier inner, bridge
             * carrier->concrete here.  (int/bool/cstr/struct elements need no
             * reinterpret -- their carrier bits ARE the value -- so only the
             * float widths take this path.) */
            if (e->type.kind == TY_TYVAR &&
                e->as.ascribe_.inner->type.kind == TY_INT &&
                ctx->current_abi_specialization) {
                Type rtv = emit_resolve_type(ctx, e->type);
                if (rtv.kind == TY_FLOAT || rtv.kind == TY_FLOAT32 ||
                    rtv.kind == TY_FLOAT64) {
                    return emit_carrier_bridge(ctx, body, inner_val,
                                               CK_CARRIER, CK_CONCRETE, rtv);
                }
            }
            if (!ascribe_to_opaque &&
                e->as.ascribe_.inner->type.kind == TY_INT &&
                type_kind_is_aggregate(e->type.kind) &&
                !type_uses_carrier_abi(e->type)) {
                return emit_carrier_bridge(ctx, body, inner_val,
                                           CK_CARRIER, CK_CONCRETE, e->type);
            }
            /* M4 follow-up: the former "unbox an int->TY_APP cast when the
             * TY_APP resolves to a concrete struct app" bridge is gone --
             * structdef-retirement DS-D: no struct-headed app resolves to a
             * concrete by-value layout (a parametric aggregate is a record ADT),
             * so the gate never fired. */
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
            /* end-to-end-monomorphization (root 2 retirement): a `:heap` typed
             * pointer (`Vec__int *`, `Cons__int *`, ...) ascribed back to the
             * int64 carrier (`(:: x :int)`) needs an explicit pointer->int
             * relabel.  Inside a by-value/typed spec the inner value IS the
             * concrete typed pointer, so returning it raw passes a pointer into
             * an int64 carrier param (-Wint-conversion / hard cc error).  Gate
             * on a CONCRETE heap layout so the carrier base -- where the inner
             * resolves to abstract `(Vec A)` and is already emitted as int64 --
             * is untouched (no snapshot drift, no redundant cast).  This is the
             * symmetric counterpart to C-3's carrier->concrete heap cast. */
            if (e->type.kind == TY_INT) {
                Type inner_resolved;
                if (!emit_var_spec_arg_type(ctx, e->as.ascribe_.inner,
                                            &inner_resolved))
                    inner_resolved =
                        emit_resolve_type(ctx, e->as.ascribe_.inner->type);
                /* Original heap-STRUCT path (unchanged): a concrete `:heap` struct
                 * pointer ascribed to the int64 carrier needs the pointer->int
                 * relabel; the abstract carrier base (no concrete layout) is left
                 * untouched. */
                if (type_is_heap_struct(inner_resolved) &&
                    type_has_concrete_codegen_layout(&inner_resolved)) {
                    return emit_carrier_bridge(ctx, body, inner_val,
                                               CK_CONCRETE, CK_CARRIER,
                                               inner_resolved);
                }
                /* seam 3: the heap-ADT analogue (lowered Vec/Cons/...).  An ADT app
                 * has NO `type_has_concrete_codegen_layout` (that predicate only
                 * handles struct apps), so gate on the c-name being a concrete
                 * pointer (`tur_adt_<Name>__A *`) -- the same signal
                 * emit_carrier_bridge's heap branch keys on.  When the inner is a
                 * heap-ADT CALL whose elab type collapsed to the abstract `(Cons A)`
                 * (c-name int64_t), recover the concrete result from its matched
                 * spec.  Gated entirely on `type_is_heap_adt`, so it can only fire
                 * for a lowered/hand-written `:heap` ADT -- never for a heap struct
                 * or non-heap value, hence no snapshot drift on the gate-off suite. */
                if (type_is_heap_adt(inner_resolved)) {
                    Type adt_r = inner_resolved;
                    const char *acn = emit_type_c_name(ctx, adt_r);
                    bool adt_ptr = acn && strchr(acn, '*');
                    if (!adt_ptr) {
                        const Expr *ci = e->as.ascribe_.inner;
                        while (ci && ci->kind == EX_ASCRIBE)
                            ci = ci->as.ascribe_.inner;
                        if (ci && ci->kind == EX_CALL) {
                            const EmitAbiSpecialization *isp =
                                find_matched_abi_spec(ctx, ci,
                                                      ci->as.call_.fn_binding);
                            if (isp) {
                                Type sr = emit_resolve_type(ctx, isp->result_type);
                                const char *sn = emit_type_c_name(ctx, sr);
                                if (type_is_heap_adt(sr) && sn && strchr(sn, '*')) {
                                    adt_r = sr;
                                    adt_ptr = true;
                                }
                            }
                        }
                    }
                    if (adt_ptr) {
                        return emit_carrier_bridge(ctx, body, inner_val,
                                                   CK_CONCRETE, CK_CARRIER, adt_r);
                    }
                }
            }
            /* gcc14-int-conversion (carrier-to-typed-param): a PARAMETRIC opaque
             * `(Goal A)` (kind * -> *, `defopaque Goal [A] :ptr<void>`) lowers to
             * the uniform int64 HKT carrier in every applied position -- the
             * function C return type of `: (Goal int)` is `int64_t`, and a binder
             * of that type is `int64_t`.  But ascribing a POINTER value into the
             * bare `:Goal` -- `(:: (fn ...) :Goal)` yields a fat-closure `void *`,
             * `(:: someHandle :Goal)` a `:ptr<void>` -- relabels to a pointer,
             * which then mismatches the int64 carrier at the return/binding
             * (-Wint-conversion, a hard error under GCC >= 14).  Reinterpret the
             * pointer as the int64 carrier so value, binder, and return agree.
             * Only for a PARAMETRIC opaque (n_type_params > 0): a non-parametric
             * opaque keeps its declared pointer carrier and is left untouched. */
            if (e->type.kind == TY_ADT && e->type.as.adt_.def &&
                e->type.as.adt_.def->is_opaque &&
                e->type.as.adt_.def->n_type_params > 0) {
                const char *icty = emit_type_c_name(ctx, e->as.ascribe_.inner->type);
                size_t iL = icty ? strlen(icty) : 0;
                if (icty && iL >= 1 && icty[iL - 1] == '*') {
                    inner_val = emit_carrier_reinterpret(ctx, inner_val, NULL, "closure-capture");
                }
            }
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
                /* constrained-byval: a by-value aggregate payload (a bare
                 * `defstruct` value wider than the int64 carrier) cannot ride
                 * the scalar `(int64_t)(struct)` cast -- cc rejects it. Mirror
                 * the unconstrained byval-aggregate path: heap-box a copy and
                 * store the pointer in `value`. EX_EXISTS_OPEN reads it back
                 * through the pointer, and the rc-managed record uses a drop
                 * hook that frees the box (the no-op default would leak it). */
                Type cpayload_ty =
                    emit_resolve_type(ctx, e->as.exists_pack_.value->type);
                bool payload_byval_agg =
                    exists_payload_is_byval_aggregate(cpayload_ty);
                const char *exist_drop_fn = payload_byval_agg
                    ? "tur_existential_drop_byval" : "tur_existential_drop";
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
                        "RcControlBlock *%s = rc_cb_alloc_kinded(sizeof(tur_existential_t) + (size_t)%u * sizeof(void *), %d, %s, 1 /* RCK_EXISTENTIAL */, %d);\n",
                        cb_tmp, (unsigned)n_w, (int)TY_PTR_VOID, exist_drop_fn,
                        payload_kind_const);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "tur_existential_t *%s = (tur_existential_t *)(%s->value);\n",
                        rec_tmp, cb_tmp);
                }
                indent_buf(body, ctx->indent);
                if (payload_byval_agg) {
                    /* heap-box the aggregate; store the box pointer in value */
                    const char *cname = type_struct_value_c_name(cpayload_ty);
                    char *box = fresh_tmp(ctx);
                    buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                               cname, box, cname, cname);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "if (!%s) { fprintf(stderr, \"pack: out of memory\\n\"); abort(); }\n",
                        box);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "*%s = %s;\n", box, val);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s->value = (int64_t)(intptr_t)(%s);\n",
                               rec_tmp, box);
                    free(box);
                } else if (vk == TY_PTR_VOID || vk == TY_EXISTS || vk == TY_FORALL
                    || vk == TY_RC || vk == TY_REF || vk == TY_WEAK) {
                    buf_printf(body, "%s->value = (int64_t)(intptr_t)(%s);\n",
                               rec_tmp, val);
                } else {
                    buf_printf(body, "%s->value = (int64_t)(%s);\n", rec_tmp, val);
                }
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s->n_witnesses = %u;\n", rec_tmp, (unsigned)n_w);
                for (uint8_t wi = 0; wi < n_w; wi++) {
                    /* constrained-byval dispatch: a heap-boxed by-value struct
                     * payload rides the carrier as a pointer, so the witness
                     * must present a carrier-ABI adapter dict whose receiver is
                     * the int64 box pointer (EX_EXISTS_DISPATCH's ABI).  For a
                     * carrier-representable payload (int/ptr/opaque) the real
                     * dict already matches, so use it directly. */
                    char *exbox = NULL;
                    if (payload_byval_agg) {
                        exbox = ensure_exists_byval_witness_dict(
                                    ctx, e->as.exists_pack_.witnesses[wi],
                                    cpayload_ty);
                    }
                    char dict_name[160];
                    if (exbox) {
                        snprintf(dict_name, sizeof(dict_name), "%s", exbox);
                        free(exbox);
                    } else {
                        emit_dict_name(dict_name, sizeof(dict_name),
                                       e->as.exists_pack_.witnesses[wi]);
                    }
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
                Type payload_ty =
                    emit_resolve_type(ctx, e->as.exists_pack_.value->type);
                if (exists_payload_is_byval_aggregate(payload_ty)) {
                    /* world-resize: the payload is a by-value aggregate wider
                     * than the int64 carrier (e.g. a (GameWorld n) struct with
                     * one sized-dense field per component). Heap-box a copy and
                     * carry the pointer; EX_EXISTS_OPEN reads it back through
                     * the same predicate and frees it (single-use ownership,
                     * matching the :linear open discipline). */
                    const char *cname = type_struct_value_c_name(payload_ty);
                    char *box = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                               cname, box, cname, cname);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "if (!%s) { fprintf(stderr, \"pack: out of memory\\n\"); abort(); }\n",
                        box);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "*%s = %s;\n", box, val);
                    buf_printf(&out, "(tur_exists_t)(%s)", box);
                    free(box);
                } else {
                    /* Scalar (int64_t, bool, etc.) — reinterpret via intptr_t (64-bit safe) */
                    buf_printf(&out, "(tur_exists_t)(intptr_t)((int64_t)(%s))", val);
                }
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
            /* world-resize: a by-value aggregate payload was heap-boxed by
             * EX_EXISTS_PACK (only on the unconstrained, non-record path). Read
             * it back through the pointer and own it -- free after the body, the
             * same single-use discipline as the :linear record path. */
            Type open_bind_ty =
                emit_resolve_type(ctx, e->as.exists_open_.var_binding->type);
            bool packed_is_byval_aggregate =
                !packed_is_record
                && exists_payload_is_byval_aggregate(open_bind_ty);
            /* constrained-byval: a constrained existential whose payload is a
             * by-value aggregate stores a heap-boxed copy and carries the box
             * pointer in the record's `value` slot (see EX_EXISTS_PACK). Read
             * the struct back through the record -> box indirection. */
            bool record_byval_aggregate =
                packed_is_record
                && exists_payload_is_byval_aggregate(open_bind_ty);
            if (record_byval_aggregate) {
                const char *cname = type_struct_value_c_name(open_bind_ty);
                if (packed_is_linear_record) {
                    buf_printf(body,
                        "%s %s = *(%s *)(intptr_t)((tur_existential_t *)(%s))->value;\n",
                        cname, var_name, cname, packed_val);
                } else {
                    buf_printf(body,
                        "%s %s = *(%s *)(intptr_t)((tur_existential_t *)((RcControlBlock *)(%s))->value)->value;\n",
                        cname, var_name, cname, packed_val);
                }
                if (e->as.exists_open_.var_binding)
                    e->as.exists_open_.var_binding->emit_byvalue_carrier_abi = true;
            } else if (packed_is_byval_aggregate) {
                const char *cname = type_struct_value_c_name(open_bind_ty);
                buf_printf(body, "%s %s = *(%s *)(%s);\n",
                           cname, var_name, cname, packed_val);
                /* The binding is a real by-value aggregate, not the int64
                 * carrier -- mark it so EX_GET_FIELD uses direct `.field`
                 * access instead of the `((T *)(intptr_t)sv)->field` carrier
                 * cast (the same flag the let-binding path sets). */
                if (e->as.exists_open_.var_binding)
                    e->as.exists_open_.var_binding->emit_byvalue_carrier_abi = true;
            } else if (packed_is_linear_record) {
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

            /* Existential-open witness dispatch: expose the existential record
             * pointer under a deterministic name (`__exrec_<v>`) so an
             * EX_EXISTS_DISPATCH method call in the body can read the packed
             * witness vtable.  Only constrained records carry witnesses. */
            if (packed_is_record) {
                indent_buf(body, ctx->indent);
                if (packed_is_linear_record) {
                    buf_printf(body,
                        "tur_existential_t *__exrec_%s = (tur_existential_t *)(%s);\n",
                        var_name, packed_val);
                } else {
                    buf_printf(body,
                        "tur_existential_t *__exrec_%s = (tur_existential_t *)((RcControlBlock *)(%s))->value;\n",
                        var_name, packed_val);
                }
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)__exrec_%s;\n", var_name);
            }
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

            /* constrained-byval: a :linear record owns its heap-boxed
             * aggregate payload; free the box before reclaiming the record
             * (the rc-managed path frees it via tur_existential_drop_byval). */
            if (record_byval_aggregate && packed_is_linear_record) {
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "free((void *)(intptr_t)((tur_existential_t *)(%s))->value);\n",
                    packed_val);
            }
            /* EXG6-3: free the bare record now that the (only) open has
             * consumed it.  The linear discipline guarantees this is the
             * single use, so the free is unconditional. */
            if (packed_is_linear_record || packed_is_byval_aggregate) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "free((void *)(%s));\n", packed_val);
            }
            free(packed_val);

            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            return nil_result ? atom_nil() : tmp;
        }
        /* Existential-open witness dispatch: lower `(.m v args...)` to an
         * indirect call through the existential record's packed witness vtable.
         *
         * The record was exposed by EX_EXISTS_OPEN as `__exrec_<v>`.  Its
         * `witnesses[witness_idx]` field points at a `dict_<Class>_<T>` struct
         * -- a flat array of method function pointers in class-declaration
         * order -- so the method pointer is `((void **)witness)[method_idx]`.
         * The receiver flows as the int64 carrier (the open binds `v` to the
         * record's int64 `value`), so the call uses the carrier ABI: each
         * class-variable-typed parameter is `int64_t`, concrete parameters keep
         * their declared C type. */
        case EX_EXISTS_DISPATCH: {
            const TypeClass *tc = e->as.exists_dispatch_.typeclass;
            const TypeClassMethod *m =
                &tc->methods[e->as.exists_dispatch_.method_idx];
            char *vname = name_for_binding(ctx,
                              e->as.exists_dispatch_.open_binding);

            /* Build the carrier-ABI signature of the method.  A parameter typed
             * as the class variable (an abstract tyvar: TY_TYVAR, or TY_STRUCT
             * with a NULL def) erases to the int64 carrier; anything else keeps
             * its concrete C type. */
            Buf sig; buf_init(&sig);
            for (uint32_t j = 0; j < m->n_params; j++) {
                if (j > 0) buf_puts(&sig, ", ");
                Type pt = m->param_types[j];
                bool is_classvar =
                    pt.kind == TY_TYVAR;
                buf_puts(&sig, is_classvar ? "int64_t" : type_c_name(pt));
            }
            buf_putc(&sig, '\0');

            const char *ret_c = type_c_name(e->type);

            /* Emit the argument values first so any side effects are ordered. */
            uint32_t na = e->as.exists_dispatch_.n_args;
            char **argvals = (char **)malloc(na * sizeof(char *));
            for (uint32_t i = 0; i < na; i++) {
                argvals[i] = emit_value(ctx, body, e->as.exists_dispatch_.args[i]);
            }

            Buf out; buf_init(&out);
            buf_printf(&out,
                "((%s (*)(%s))(((void **)(__exrec_%s->witnesses[%u]))[%u]))(",
                ret_c, sig.data, vname,
                (unsigned)e->as.exists_dispatch_.witness_idx,
                (unsigned)e->as.exists_dispatch_.method_idx);
            for (uint32_t i = 0; i < na; i++) {
                if (i > 0) buf_puts(&out, ", ");
                buf_puts(&out, argvals[i]);
                free(argvals[i]);
            }
            buf_putc(&out, ')');
            buf_putc(&out, '\0');

            free(argvals);
            free(vname);
            buf_free(&sig);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
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
            if (e->as.match_.scrutinee->type.kind == TY_SESSION_OFFER) {
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

                    /* A `!`-typed arm body (a `(panic ...)` arm) produces no value: emit it
                     * as a statement, leaving the result temp at its zero init. */
                    if (!nil_result && arm->body->type.kind != TY_NEVER) {
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
                    /* A `!`-typed arm body (a `(panic ...)` arm) produces no value: emit it
                     * as a statement, leaving the result temp at its zero init. */
                    if (!nil_result && arm->body->type.kind != TY_NEVER) {
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
                /* A scrutinee that is not an ADT belongs here whether or not
                 * any arm spells a literal.  Gating on `_has_lit` alone sent
                 * `(match p _ 0)` and `(match p x x)` down the ADT path below,
                 * which reads `adt->name` through a NULL AdtDef -- there is no
                 * AdtDef for an `:int`.  The `!_scrut_is_adt` disjunct below is
                 * that guard; do not narrow it back to `_has_lit`. */
                const Type *_sbase = &e->as.match_.scrutinee->type;
                while (_sbase && _sbase->kind == TY_APP && _sbase->as.app.fn)
                    _sbase = _sbase->as.app.fn;
                bool _scrut_is_adt = _sbase && (_sbase->kind == TY_ADT);
                if (_has_lit || !_scrut_is_adt) {
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
                    /* A FLAT SEQUENCE OF `if` BLOCKS, each jumping to the end
                     * on success -- not an if/else-if chain.
                     *
                     * The chain could not express a GUARD.  A guarded arm may
                     * fail its guard and fall through to the next arm, so its
                     * test is not the whole condition; the chain emitted a bare
                     * `else` for a guarded wildcard and then a second `else`
                     * for the arm after it, producing `'else' without a
                     * previous 'if'` from the C compiler -- a program that
                     * simply could not be built.  It also had no place to bind
                     * a var pattern BEFORE evaluating a guard that mentions it.
                     *
                     * This is the same shape the ADT path below already uses,
                     * for the same reason. */
                    char *_end_label = fresh_tmp(ctx);
                    for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                        MatchArm *arm = &e->as.match_.arms[ai];
                        MatchPattern *pat = &arm->pattern;
                        indent_buf(body, ctx->indent);
                        if (pat->is_wildcard || pat->is_var) {
                            buf_puts(body, "{\n");
                        } else {
                            const char *kw = "if";
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
                        /* The guard is tested AFTER the pattern's binding
                         * exists, since it is allowed to mention it. */
                        if (arm->guard) {
                            char *gv = emit_value(ctx, body, arm->guard);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "if (%s) {\n", gv);
                            free(gv);
                            ctx->indent += 4;
                        }
                        /* A `!`-typed arm body (a `(panic ...)` arm) produces no value: emit it
                         * as a statement, leaving the result temp at its zero init. */
                        if (!nil_result && arm->body->type.kind != TY_NEVER) {
                            char *bv = emit_value(ctx, body, arm->body);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s = %s;\n", tmp, bv);
                            free(bv);
                        } else {
                            emit_stmt(ctx, body, arm->body);
                        }
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "goto __%s;\n", _end_label);
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
                    buf_printf(body, "__%s:;\n", _end_label);
                    free(_end_label);
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
            /* TS4P3: Use the monomorphised struct name when the scrutinee is
             * a concrete ADT app (e.g. tur_adt_Maybe__float instead of tur_adt_Maybe).
             * nested-carrier-match: scrut_is_app_monomorph also gates the
             * inline-vs-deref field read below -- only a monomorph-app scrutinee
             * lays a non-wide by-value ADT field out INLINE; the base carrier /
             * GADT representation always stores it as an int64 box (deref). */
            const char *inst_name = (e->as.match_.scrutinee->type.kind == TY_APP)
                ? type_register_adt_app(e->as.match_.scrutinee->type) : NULL;
            bool scrut_is_app_monomorph = (inst_name != NULL);
            {
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
                 * is guaranteed by the elaborator so the default branch is unreachable.
                 * CONV-S1/B3: a by-value ADT result is a C aggregate -- it cannot be
                 * scalar-initialised with `0` (invalid initializer), so use `{0}`. */
                if (type_is_byvalue_adt_product(e->type))
                    buf_printf(body, "%s %s = {0};\n", type_c_name(e->type), tmp);
                else
                    buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
            }

            /* Emit scrutinee */
            char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);

            /* Phase G4: Check if any arm has a guard */
            bool has_any_guard = false;
            for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                if (e->as.match_.arms[ai].guard) { has_any_guard = true; break; }
            }

            /* CONV-S2: a single-variant flat-product ADT has no `tag` word, so it
             * cannot be dispatched with a `switch (__scrut->tag)`.  Route it
             * through the if-chain path (which never reads the tag for these
             * types -- see the ctor-arm header below) so the sole arm is entered
             * unconditionally.  The single variant makes the match trivially
             * exhaustive, so first-arm-wins is correct. */
            bool adt_flat = adt_is_flat_product(adt);
            /* CONV-S1: a by-value flat product binds its scrutinee as an aggregate
             * (no carrier pointer) and reads fields with `.`.  Gated by
             * adt_is_byvalue_product (LIVE for leaf products as of B3); byval
             * implies flat, so it always takes the if-chain path below. */
            /* Parametric-by-value: an app scrutinee (`(Pair2 int int)`) is a
             * by-value monomorph aggregate too, even though its base def is
             * parametric (adt_is_byvalue_product is false for n_type_params!=0).
             * Widen the by-value decision to the app-aware predicate keyed on the
             * scrutinee's concrete type. */
            Type scrut_ty = e->as.match_.scrutinee->type;
            /* seam 3: a :heap ADT scrutinee is a typed pointer, never a by-value
             * aggregate -- it falls to the carrier branch below (bind as a
             * pointer, read fields with `->`), exactly as a carrier ADT does. */
            /* SR1: the by-value decision belongs to the SCRUTINEE's
             * representation, not to the ADT its patterns name.  Those were the
             * same question while a sum always rode the carrier, so this site
             * could read the answer off `adt` alone.  They come apart on the
             * `:int`-as-type-eraser shape (CLAUDE.md's "No Lazy `:int`
             * Stand-Ins"): `stdlib/fix.tur` declares `(defn unroll [fix : int]
             * ...)` and matches the result against `CountF` constructors, so the
             * pattern's ADT is by-value while the value in hand is an int64
             * carrier word.  Emitting `tur_adt_CountF __scrut = <int64>` is not a
             * coercion, it is "invalid initializer".  When the scrutinee is
             * statically a carrier word, keep the carrier path -- the producing
             * side boxed into it (see the int-carrier-param bridge in the call
             * emitter), so the deref below reads back exactly what was stored. */
            Type rscrut_ty = emit_resolve_type(ctx, scrut_ty);
            bool scrut_is_carrier_word =
                rscrut_ty.kind == TY_INT || rscrut_ty.kind == TY_INT64 ||
                rscrut_ty.kind == TY_UINT64 || rscrut_ty.kind == TY_PTR_VOID;
            bool adt_byval = (adt_is_byvalue_product(adt) ||
                              adt_app_is_byvalue_product(scrut_ty)) &&
                             !scrut_is_carrier_word &&
                             !type_is_heap_adt(scrut_ty) &&
                             !(adt && adt->is_heap);
            /* Slice 3: a large by-value ADT scrutinee that is a pass-by-pointer
             * param arrives as `const tur_adt_X *`, so it is already a pointer --
             * bind it as one and read fields with `->`, never `.`. */
            bool adt_byval_pbp = false;
            if (adt_byval) {
                const Expr *scrut_e = e->as.match_.scrutinee;
                while (scrut_e->kind == EX_ASCRIBE)
                    scrut_e = scrut_e->as.ascribe_.inner;
                adt_byval_pbp = expr_is_pbp_param(ctx, scrut_e);
            }

            if (has_any_guard || adt_flat) {
                /* Phase G4: Emit as if-chain with goto for guard fallthrough */
                char *end_label = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                indent_buf(body, ctx->indent);
                if (adt_byval_pbp) {
                    /* Slice 3: scrutinee is already a `const tur_adt_X *` (pbp
                     * param) -- bind as a pointer, fields read via `->`. */
                    buf_printf(body, "const %s *__scrut = (%s);\n",
                               adt_c_name, scrut_val);
                } else if (adt_byval) {
                    /* CONV-S1: scrutinee is a by-value aggregate -- bind directly. */
                    buf_printf(body, "%s __scrut = (%s);\n", adt_c_name, scrut_val);
                } else {
                    buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
                               adt_c_name, adt_c_name, scrut_val);
                }
                free(scrut_val);

                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->is_var || adt_flat) {
                        /* No tag word on a flat single-variant ADT: the sole
                         * constructor arm is entered unconditionally. */
                        buf_puts(body, "{\n");
                    } else {
                        /* SR1: a by-value sum HAS a tag, and `__scrut` is then the
                         * aggregate itself rather than a carrier pointer, so the
                         * tag test reads with `.` -- the same accessor the field
                         * binds below already use.  Before SR1 a tagged scrutinee
                         * was always a pointer, so this site could hardcode `->`. */
                        buf_printf(body, "if (__scrut%stag == %u) {\n",
                                   (adt_byval && !adt_byval_pbp) ? "." : "->",
                                   pat->ctor->tag);
                    }
                    ctx->indent += 4;

                    /* match-adt-var-arm-does-not-bind: a variable catch-all arm
                     * binds the WHOLE scrutinee.  `__scrut` is the by-value
                     * aggregate, a pbp pointer to it, or the carrier pointer,
                     * so the read matches how it was bound above. */
                    if (pat->is_var && pat->var_binding) {
                        const char *vct = type_c_name(pat->var_binding->type);
                        char *vname = name_for_binding(ctx, pat->var_binding);
                        indent_buf(body, ctx->indent);
                        if (adt_byval_pbp)
                            buf_printf(body, "%s %s = *__scrut;\n", vct, vname);
                        else if (adt_byval)
                            buf_printf(body, "%s %s = __scrut;\n", vct, vname);
                        else
                            buf_printf(body, "%s %s = (%s)(intptr_t)__scrut;\n",
                                       vct, vname, vct);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "(void)%s;\n", vname);
                        free(vname);
                    }

                    /* Bind fields */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        char *_mctor = mangle_field_name(pat->ctor->name);
                        /* CONV-S1: by-value scrutinee reads fields with `.`; a
                         * carrier or (slice 3) pass-by-pointer scrutinee is a
                         * pointer, so it reads with `->`. */
                        const char *acc = (adt_byval && !adt_byval_pbp) ? "." : "->";
                        for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                            Binding *fb = pat->bindings[bi];
                            const char *ctype = type_c_name(fb->type);
                            char *bname = name_for_binding(ctx, fb);
                            /* CONV-S1 seam 4: flat named record binds `.field`; a
                             * tagged/positional ADT binds `.as.<Ctor>._N`. */
                            char *mp = adt_field_member_path(pat->ctor->adt, pat->ctor, bi);
                            indent_buf(body, ctx->indent);
                            /* slice 4: an inline by-value aggregate field is the
                             * aggregate value itself in the union slot -- bind it
                             * directly, no cast (cast-to-aggregate is invalid C)
                             * and no carrier unbox. */
                            bool inline_byval = adt_byval && bi < pat->ctor->n_fields &&
                                adt_field_is_inline_byval(&pat->ctor->fields[bi]);
                            if (inline_byval) {
                                buf_printf(body, "%s %s = __scrut%s%s;\n",
                                           ctype, bname, acc, mp);
                            } else if (emit_type_is_byval_recursive_carrier(ctx, fb->type) ||
                                       (emit_type_is_wide_byval_adt(ctx, fb->type) &&
                                        strcmp(ctype, "int64_t") == 0)) {
                                /* B4: read the int64 carrier raw (no deref).
                                 * slice 1 (<=8): the slot holds the by-value
                                 * wrapper's bits inline.  slice 2 (>8): the slot
                                 * holds a heap-box POINTER.  Only fires when the
                                 * binding is the ERASED int64 carrier (a generic
                                 * element); a concrete wide by-value ADT binding
                                 * (ctype is the aggregate) keeps the B3 deref
                                 * below.  The value crosses to a fat closure as
                                 * the carrier (reinterpret <=8 / box pointer >8)
                                 * and is materialized at the boundary. */
                                buf_printf(body, "%s %s = (%s)__scrut%s%s;\n",
                                           ctype, bname, ctype, acc, mp);
                            } else if (emit_type_is_byvalue_adt(ctx, fb->type)) {
                                if (scrut_is_app_monomorph &&
                                    !emit_type_is_wide_byval_adt(ctx, fb->type)) {
                                    /* nested-carrier-match: in a monomorph-app
                                     * scrutinee a non-wide by-value ADT field
                                     * (e.g. `(Pair2 int int)`) is stored INLINE as
                                     * the aggregate, exactly as the typedef emitter
                                     * lays it out (!type_is_wide_byval_adt ->
                                     * aggregate, not int64).  Read it directly -- a
                                     * deref / intptr_t cast of an aggregate is
                                     * invalid C. */
                                    buf_printf(body, "%s %s = __scrut%s%s;\n",
                                               ctype, bname, acc, mp);
                                } else {
                                    /* B3: a by-value ADT field stored boxed (int64
                                     * heap pointer) in a carrier / GADT slot --
                                     * unbox by deref. */
                                    buf_printf(body,
                                        "%s %s = *(%s *)(intptr_t)(__scrut%s%s);\n",
                                        ctype, bname, ctype, acc, mp);
                                }
                            } else {
                                buf_printf(body, "%s %s = (%s)__scrut%s%s;\n",
                                           ctype, bname, ctype, acc, mp);
                            }
                            free(bname);
                            free(mp);
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
                    /* A `!`-typed arm body (a `(panic ...)` arm) produces no value: emit it
                     * as a statement, leaving the result temp at its zero init. */
                    if (!nil_result && arm->body->type.kind != TY_NEVER) {
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
                if (adt_byval) {
                    /* SR1: a by-value multi-variant sum still needs the tag
                     * test, but the value is an aggregate, not a carrier
                     * pointer -- casting it trips "aggregate value used where
                     * an integer was expected".  Bind a pointer to a local
                     * copy rather than rewriting this arm's ten `__scrut->`
                     * field reads: the copy has exactly the match's lifetime,
                     * and every reader below keeps working unchanged.  (The
                     * if-chain path above already had its own by-value arm;
                     * only the switch path was carrier-only, because a
                     * by-value ADT could not previously carry a tag.) */
                    buf_printf(body, "%s __scrut_v = (%s);\n",
                               adt_c_name, scrut_val);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s *__scrut = &__scrut_v;\n", adt_c_name);
                } else {
                    buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
                               adt_c_name, adt_c_name, scrut_val);
                }
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

                    /* match-adt-var-arm-does-not-bind: the variable catch-all
                     * binds the whole scrutinee (carrier pointer here). */
                    if (pat->is_var && pat->var_binding) {
                        const char *vct = type_c_name(pat->var_binding->type);
                        char *vname = name_for_binding(ctx, pat->var_binding);
                        indent_buf(body, ctx->indent);
                        if (adt_byval)
                            /* Copy, never `&__scrut_v`: the binding can outlive
                             * this arm, and handing out a stack address is the
                             * dangling-element bug the heap-container inserts
                             * already have to defend against. */
                            buf_printf(body, "%s %s = *__scrut;\n", vct, vname);
                        else
                        buf_printf(body, "%s %s = (%s)(intptr_t)__scrut;\n",
                                   vct, vname, vct);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "(void)%s;\n", vname);
                        free(vname);
                    }

                    /* Bind field variables for constructor patterns */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        char *_mctor = mangle_field_name(pat->ctor->name);
                        for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                            Binding *fb = pat->bindings[bi];
                            const char *ctype = type_c_name(fb->type);
                            /* Use name_for_binding to get the canonical C name */
                            char *bname = name_for_binding(ctx, fb);
                            char *mp = adt_field_member_path(pat->ctor->adt, pat->ctor, bi);
                            indent_buf(body, ctx->indent);
                            /* SR1: a by-value SUM inlines its by-value aggregate
                             * fields into the union slot, exactly as the typedef
                             * emitter lays them out (adt_ctor_field_c_type keys
                             * the same predicate off the owner's by-value status).
                             * Bind the aggregate directly -- no carrier unbox, and
                             * no cast, since a cast-to-aggregate is invalid C.
                             * The if-chain path below/above already had this
                             * branch; the switch path could not previously be
                             * reached by a by-value owner, because "flows by
                             * value" implied "has no tag". */
                            bool inline_byval = adt_byval &&
                                bi < pat->ctor->n_fields &&
                                adt_field_is_inline_byval(&pat->ctor->fields[bi]);
                            /* CONV-S1/B3: a by-value ADT field is stored boxed
                             * (int64 heap pointer) in the carrier tagged union;
                             * unbox it by deref. */
                            if (inline_byval) {
                                buf_printf(body, "%s %s = __scrut->%s;\n",
                                           ctype, bname, mp);
                            } else if (emit_type_is_byval_recursive_carrier(ctx, fb->type) ||
                                (emit_type_is_wide_byval_adt(ctx, fb->type) &&
                                 strcmp(ctype, "int64_t") == 0)) {
                                /* B4: read the int64 carrier raw (no deref).
                                 * slice 1 (<=8): inline wrapper bits.  slice 2
                                 * (>8): heap-box POINTER.  Only the erased int64
                                 * carrier binding takes this path; a concrete wide
                                 * by-value ADT binding keeps the B3 deref. */
                                buf_printf(body, "%s %s = (%s)__scrut->%s;\n",
                                           ctype, bname, ctype, mp);
                            } else if (emit_type_is_byvalue_adt(ctx, fb->type)) {
                                if (scrut_is_app_monomorph &&
                                    !emit_type_is_wide_byval_adt(ctx, fb->type)) {
                                    /* nested-carrier-match: in a monomorph-app
                                     * scrutinee a non-wide by-value ADT field
                                     * (e.g. `(Pair2 int int)`) is stored INLINE as
                                     * the aggregate, exactly as the typedef emitter
                                     * lays it out -- read it directly; a deref /
                                     * intptr_t cast of an aggregate is invalid C. */
                                    buf_printf(body, "%s %s = __scrut->%s;\n",
                                               ctype, bname, mp);
                                } else {
                                    /* B3: a by-value ADT field stored boxed (int64
                                     * heap pointer) in a carrier / GADT slot --
                                     * unbox by deref. */
                                    buf_printf(body,
                                        "%s %s = *(%s *)(intptr_t)(__scrut->%s);\n",
                                        ctype, bname, ctype, mp);
                                }
                            } else {
                                buf_printf(body, "%s %s = (%s)__scrut->%s;\n",
                                           ctype, bname, ctype, mp);
                            }
                            free(bname);
                            free(mp);
                        }
                        free(_mctor);
                    }

                    /* Emit body */
                    /* A `!`-typed arm body (a `(panic ...)` arm) produces no value: emit it
                     * as a statement, leaving the result temp at its zero init. */
                    if (!nil_result && arm->body->type.kind != TY_NEVER) {
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
         * docs/archive/history/variadic-rest-closure-cast-plan.md. */
        case EX_CONS_LIST: {
            uint32_t n = e->as.cons_list_.n;
            if (n == 0) return strdup("0LL");
            /* Right-fold: start from the rightmost element and build leftward */
            char *tail = strdup("0LL");  /* nil sentinel */
            for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
                char *head = emit_value(ctx, body, e->as.cons_list_.items[i]);
                TypeKind hk = e->as.cons_list_.items[i]->type.kind;
                Buf cell; buf_init(&cell);
                if (hk == TY_FLOAT || hk == TY_FLOAT64 || hk == TY_FLOAT32) {
                    /* A float head does not survive an integer cast -- store its
                     * IEEE-754 bit pattern in the cons cell's int64 head slot via
                     * a union reinterpret (mirrors EX_UNION_INJECT's float path).
                     * A plain (int64_t) cast would truncate 1.5 to 1, silently
                     * miscompiling a `(list-of 1.5 ...)` / float variadic rest. */
                    const char *cty = (hk == TY_FLOAT32) ? "float" : "double";
                    buf_printf(&cell,
                        "__tur_cons_of(((union { %s d; int64_t i; }){.d = (%s)}).i, %s)",
                        cty, head, tail);
                } else if (g_sr1_sum_byvalue &&
                           emit_type_is_byvalue_adt(
                               ctx, e->as.cons_list_.items[i]->type)) {
                    /* SR1: a by-value AGGREGATE head does not survive
                     * `(int64_t)(intptr_t)` either -- a cast from a struct is not
                     * a coercion, it is a hard C error ("aggregate value used
                     * where an integer was expected").  Route it through the
                     * escaping carrier bridge, which heap-promotes the value and
                     * carries the pointer: the cons cell outlives the statement
                     * that built it, so a stack spill would dangle.  This is the
                     * same crossing a by-value ADT already makes into a carrier
                     * ctor field slot.  Before SR1 only a single-variant product
                     * could be by-value, and one never reached a variadic rest
                     * slot in the suite; a by-value sum does. */
                    char *boxed = emit_carrier_bridge_escaping(
                        ctx, body, head, CK_CONCRETE, CK_CARRIER,
                        emit_resolve_type(ctx, e->as.cons_list_.items[i]->type));
                    head = boxed;
                    buf_printf(&cell, "__tur_cons_of((int64_t)(intptr_t)(%s), %s)", head, tail);
                } else {
                    buf_printf(&cell, "__tur_cons_of((int64_t)(intptr_t)(%s), %s)", head, tail);
                }
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
