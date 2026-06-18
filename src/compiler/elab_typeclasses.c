/* elab_typeclasses.c -- typeclass declarations, instances, and method-call dispatch. */
#include "elab_internal.h"
#include "forms.h"
#include "mangle.h"

/* ---- file-local helper forward declarations ---- */
static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span,
    uint32_t *out_body_start,
    const Symbol **class_type_params, uint8_t n_class_type_params,
    const Kind *class_type_param_kinds,
    const Symbol **assoc_type_names, uint8_t n_assoc_type_names);
static Expr *make_dict_expr(Elab *e, TypeClassInstance *inst, Span span);
static bool rt_type_mentions_tyvar(const Type *t, const char *name);

/* Phase RT: is class method `m` of class `c` a return-only-dispatch method --
 * one of the class's type parameters appears in the return type but in no
 * parameter type?  Such methods select their instance from the expected result
 * type, so their parameters must NOT be coerced to the instance type the way
 * an ordinary receiver-dispatched method's first int parameter is. */
static bool method_is_return_dispatch(const TypeClass *c, const TypeClassMethod *m) {
    for (uint8_t ti = 0; ti < c->n_type_params; ti++) {
        const Symbol *tp = c->type_params[ti];
        if (!tp) continue;
        if (!rt_type_mentions_tyvar(&m->return_type, tp->name)) continue;
        bool in_param = false;
        for (uint8_t pi = 0; pi < m->n_params; pi++) {
            if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                in_param = true;
                break;
            }
        }
        if (!in_param) return true;
    }
    return false;
}

/* KB-030: designated home file (basename) for a built-in primitive type that
 * has no StructDef of its own (e.g. `str`, `rc`).  The orphan-instance check
 * credits a user struct to its defining module via origin_file_id; built-ins
 * have no def and so were always flagged orphan.  This table gives each such
 * built-in a home stdlib file, so an instance declared in that file (e.g.
 * `(definstance Eq [str] ...)` in stdlib/str.tur) is treated as non-orphan,
 * mirroring the ownership rule for user types.  Returns NULL for names that
 * are not registered built-ins. */
static const char *builtin_type_home_basename(const char *type_name) {
    if (!type_name) return NULL;
    if (strcmp(type_name, "str")  == 0) return "str.tur";
    if (strcmp(type_name, "rc")   == 0) return "rc.tur";
    if (strcmp(type_name, "weak") == 0) return "rc.tur";
    return NULL;
}

/* KB-030/KB-027: as above, but keyed on a resolved built-in TypeKind.  Some
 * built-ins resolve to a dedicated TypeKind rather than an opaque-struct name,
 * so they carry no type_arg_syms entry; map those kinds to their home file
 * directly.  Two families:
 *   - rc<T>/weak<T> are data types with their own module -> rc.tur;
 *   - the bare scalar primitives (int, bool, cstr, the sized numeric kinds)
 *     have no data module of their own, so their canonical typeclass instances
 *     live in the comprehensive typeclass module -> typeclass.tur.  This lets
 *     typeclass.tur host `Clone [int]`, `Clone [uint8]`, ... without tripping
 *     the orphan check (Clone is declared in typeclass-clone.tur).
 * Returns NULL for kinds with no fixed home. */
static const char *builtin_kind_home_basename(TypeKind k) {
    switch (k) {
        case TY_RC:
        case TY_WEAK:
            return "rc.tur";
        case TY_INT:
        case TY_BOOL:
        case TY_CSTR:
        case TY_FLOAT:
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
            return "typeclass.tur";
        /* SYM3 (runtime-symbols-plan): the :Sym primitive has no data module of
         * its own; its canonical instances (Eq/Hash/MapKey[Sym]) live in
         * sym.tur, which is only auto-loaded under -Xsymbols.  Crediting Sym to
         * sym.tur keeps those instances non-orphan there. */
        case TY_SYM:
            return "sym.tur";
        default:
            return NULL;
    }
}

/* Return the final path component of `path` (the basename), or `path` itself
 * when it contains no '/'.  NULL-safe. */
static const char *tc_path_basename(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* F3-5 (cross-plan-followups): convert a runtime Type back to its
 * source-form representation so the dispatcher can synthesise inline
 * ascription forms.  Supports only the kinds the dispatch synthesis
 * actually needs: primitives + TY_STRUCT (named) + TY_APP.  Returns
 * NULL for unsupported kinds. */
static Form *type_to_form(Elab *e, const Type *t, Span span) {
    if (!t) return NULL;
    const char *kw = NULL;
    switch (t->kind) {
        case TY_INT:      kw = "int";      break;
        case TY_BOOL:     kw = "bool";     break;
        case TY_CSTR:     kw = "cstr";     break;
        case TY_FLOAT:    kw = "float";    break;
        case TY_INT8:     kw = "int8";     break;
        case TY_INT16:    kw = "int16";    break;
        case TY_INT32:    kw = "int32";    break;
        case TY_UINT8:    kw = "uint8";    break;
        case TY_UINT16:   kw = "uint16";   break;
        case TY_UINT32:   kw = "uint32";   break;
        case TY_UINT64:   kw = "uint64";   break;
        case TY_FLOAT32:  kw = "float32";  break;
        case TY_PTR_VOID: kw = "ptr<void>"; break;
        case TY_SYM:      kw = "Sym";      break;
        default: break;
    }
    if (kw) {
        return form_keyword(e->arena, span,
            intern_cstr(e->st, kw));
    }
    if (t->kind == TY_STRUCT && t->as.struct_.def && t->as.struct_.def->name) {
        /* Bare struct name: just an F_SYM. */
        return form_sym(e->arena, span,
            intern_cstr(e->st, t->as.struct_.def->name));
    }
    if (t->kind == TY_APP) {
        /* Walk the TY_APP chain to collect [head, arg1, arg2, ...]
         * (left-associative -- arg1 is the innermost). */
        const Type *args[8];
        uint8_t n_args = 0;
        const Type *head = t;
        while (head && head->kind == TY_APP && n_args < 8) {
            if (head->as.app.arg) args[n_args++] = head->as.app.arg;
            head = head->as.app.fn;
        }
        if (!head || head->kind != TY_STRUCT ||
            !head->as.struct_.def || !head->as.struct_.def->name) {
            return NULL;
        }
        /* Build (StructName arg1-form arg2-form ...) in original order. */
        uint32_t n_items = 1 + n_args;
        Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
        items[0] = form_sym(e->arena, span,
            intern_cstr(e->st, head->as.struct_.def->name));
        /* args were collected innermost-first; reverse to original order. */
        for (uint8_t i = 0; i < n_args; i++) {
            Form *af = type_to_form(e, args[n_args - 1 - i], span);
            if (!af) return NULL;
            items[1 + i] = af;
        }
        return form_list(e->arena, span, items, n_items);
    }
    return NULL;
}

/* F3-5: map a typed-collection struct identity to its eq-helper symbol
 * and the number of element-comparator parameters the helper expects.
 *
 * Helpers split into two shapes:
 *   1-comparator: (helper m1 m2 elem-cmp)               -- Vec, Map,
 *                                                          Option, Cons
 *   2-comparator: (helper m1 m2 cmp-A cmp-B)            -- Pair, Result
 *
 * For Map's keys the helper relies on the HAMT's hash lookup -- key
 * equality is via the hash + pointer/memcmp, which is correct for
 * primitive keys.  Recursive Map[K (Vec V)] etc. only need the
 * V-comparator threaded recursively.
 *
 * Set is intentionally not listed: `set-eq?` takes no comparator
 * (relies on hash equality entirely), so even single-level dispatch
 * is wrong for non-primitive elements; fixing it requires changes to
 * the helper itself, not just the dispatcher.
 *
 * Returns NULL if the struct is not a recognised typed collection. */
static const Symbol *helper_eq_symbol_for_struct(Elab *e, const StructDef *sd,
                                                  uint8_t *out_n_comparators) {
    if (!sd || !sd->name) return NULL;
    if (strcmp(sd->name, "Vec") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "vec-eq?");
    }
    if (strcmp(sd->name, "Map") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "map-eq?");
    }
    if (strcmp(sd->name, "Option") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "option-eq?");
    }
    if (strcmp(sd->name, "Cons") == 0) {
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "list-eq?");
    }
    if (strcmp(sd->name, "Pair") == 0) {
        if (out_n_comparators) *out_n_comparators = 2;
        return intern_cstr(e->st, "pair-eq?");
    }
    if (strcmp(sd->name, "Result") == 0) {
        if (out_n_comparators) *out_n_comparators = 2;
        return intern_cstr(e->st, "result-eq?");
    }
    if (strcmp(sd->name, "Set") == 0) {
        /* `set-eq?` itself takes no comparator (relies on HAMT hash
         * equality, wrong for non-primitive elements).  The synth path
         * routes to a comparator-taking variant `set-eq-cmp?` instead
         * so structural equality of Set[Vec[int]] etc. works
         * correctly. */
        if (out_n_comparators) *out_n_comparators = 1;
        return intern_cstr(e->st, "set-eq-cmp?");
    }
    return NULL;
}

/* F3-5: build a comparator lambda Form for a single type argument.
 * The lambda has shape `(fn [a b] (.eq? (:: a TYPE) (:: b TYPE)))`
 * where TYPE is the source-form of `elem_type`.  At elaboration time
 * the inner `.eq?` dispatches on the ascribed receiver type, which
 * (thanks to F3-7's sticky ascription) terminates the recursion at
 * the right level.  Returns NULL if `elem_type` cannot be
 * round-tripped to a Form. */
static Form *build_comparator_lambda(Elab *e, const Type *elem_type, Span span) {
    Form *elem_form = type_to_form(e, elem_type, span);
    if (!elem_form) return NULL;

    const Symbol *sym_a       = intern_cstr(e->st, "__cmp_a");
    const Symbol *sym_b       = intern_cstr(e->st, "__cmp_b");
    const Symbol *sym_dot_eq  = intern_cstr(e->st, ".eq?");

    Form **asc_a_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    asc_a_items[0] = form_sym(e->arena, span, e->sym_ascribe);
    asc_a_items[1] = form_sym(e->arena, span, sym_a);
    asc_a_items[2] = elem_form;
    Form *asc_a = form_list(e->arena, span, asc_a_items, 3);

    Form **asc_b_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    asc_b_items[0] = form_sym(e->arena, span, e->sym_ascribe);
    asc_b_items[1] = form_sym(e->arena, span, sym_b);
    asc_b_items[2] = elem_form;
    Form *asc_b = form_list(e->arena, span, asc_b_items, 3);

    Form **dot_eq_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    dot_eq_items[0] = form_sym(e->arena, span, sym_dot_eq);
    dot_eq_items[1] = asc_a;
    dot_eq_items[2] = asc_b;
    Form *dot_eq_call = form_list(e->arena, span, dot_eq_items, 3);

    Form **params_items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    params_items[0] = form_sym(e->arena, span, sym_a);
    params_items[1] = form_sym(e->arena, span, sym_b);
    Form *params_vec = form_vec(e->arena, span, params_items, 2);

    Form **lambda_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    lambda_items[0] = form_sym(e->arena, span, e->sym_fn);
    lambda_items[1] = params_vec;
    lambda_items[2] = dot_eq_call;
    return form_list(e->arena, span, lambda_items, 3);
}

/* GHE5/#4 (non-int fields): produce a zero-valued Form for one struct field's
 * storage kind, used as a dummy argument to `make-struct` so that the
 * MapKey[K] typeclass dispatch sees a value of the right type.  mk-cmp
 * ignores the argument value entirely -- it only needs the type.
 *
 * For non-parameterised structs make-struct does not type-check simple
 * (non-full_type) field values, so the forms produced here satisfy dispatch
 * even when their elaborated type is the default-width variant (e.g.
 * form_float(0.0) elab → TY_FLOAT for a TY_FLOAT32 field).  We still emit
 * the semantically-closest form to remain forward-compatible if make-struct's
 * type checking is tightened in future.
 *
 * Returns NULL for field kinds we cannot synthesise a zero literal for
 * (e.g. TY_FN, TY_RC, TY_REF -- such fields make the struct unsuitable as a
 * map-key witness anyway). */
static Form *zero_form_for_field(Elab *e, const StructField *f, Span span) {
    switch (f->kind) {
        case TY_BOOL:
            return form_bool(e->arena, span, false);
        case TY_CSTR:
            return form_str(e->arena, span, "", 0);
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64:
            return form_float(e->arena, span, 0.0);
        case TY_INT:
        case TY_INT8:  case TY_INT16:  case TY_INT32:  case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return form_int(e->arena, span, 0);
        default:
            return NULL;
    }
}

/* GHE5/#4: build `(mk-cmp (:: 0 K))` -- the MapKey[K] carrier comparator for
 * a concrete key type K.  mk-cmp ignores its argument value (it returns a
 * constant carrier-ABI function pointer), so the ascribed 0 only carries the
 * type K for dispatch.  Returns NULL if K cannot be round-tripped to a Form. */
static Form *build_mapkey_cmp_form(Elab *e, const Type *key_type, Span span) {
    Form *key_form = type_to_form(e, key_type, span);
    if (!key_form) return NULL;
    const Symbol *sym_mk_cmp = intern_cstr(e->st, "mk-cmp");

    Form **asc_items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    asc_items[0] = form_sym(e->arena, span, e->sym_ascribe);
    asc_items[1] = form_int(e->arena, span, 0);
    asc_items[2] = key_form;
    Form *asc = form_list(e->arena, span, asc_items, 3);

    Form **mk_items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    mk_items[0] = form_sym(e->arena, span, sym_mk_cmp);
    mk_items[1] = asc;
    return form_list(e->arena, span, mk_items, 2);
}

/* F3-5: synthesise the dispatcher rewrite for `(.eq? obj other)` when
 * `obj` has TY_APP receiver type AND the outer instance is a known
 * typed-collection whose element type(s) include at least one TY_APP
 * (recursive structural equality).  Returns NULL if conditions aren't
 * met or synthesis isn't applicable.
 *
 * 1-comparator helpers (Vec, Map, Option, Cons) synthesise:
 *   (<helper> obj other (fn [a b] (.eq? (:: a <elem>) (:: b <elem>))))
 *
 * 2-comparator helpers (Pair, Result) synthesise:
 *   (<helper> obj other
 *     (fn [a b] (.eq? (:: a <fst>) (:: b <fst>)))
 *     (fn [a b] (.eq? (:: a <snd>) (:: b <snd>))))
 *
 * The synthesised closures re-enter elab_method_call with `(:: a <T>)`
 * as the receiver, which (thanks to F3-7's sticky ascription) dispatches
 * to the right inner instance.  Recursion terminates at primitive
 * element types where F3-7's single-level path takes over. */
/* CRU B-3: box a synthesized comparator expr into a fat closure.  The
 * constrained-Eq synthesis dispatcher builds its helper call as an EX_CALL node
 * directly, bypassing elab_call's ^fat auto-shim -- so a captureless comparator
 * lambda would reach the (now ^fat) *-eq? value-comparator parameter as a bare
 * function pointer and be misread as a fat box (segfault, per
 * docs/reported/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md).
 * Wrapping it in EX_FN_TO_FAT here boxes a captureless lambda via the
 * per-signature __tur_fatshim_*, and is a pass-through for an already-fat
 * (capturing) closure -- matching the fat dispatch the helper bodies now use.
 * Only *value/element* comparators are boxed; the MapKey `keyeq` carrier stays
 * thin (it is a constant carrier-ABI fn pointer, not a user closure). */
static Expr *box_synth_comparator(Elab *e, Expr *inner) {
    if (!inner) return NULL;
    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID, inner->span);
    shim->as.fn_to_fat_.inner = inner;
    return shim;
}

/* Coerce an argument bound for a fat-closure (is_fat) method parameter into the
 * fat-closure representation, mirroring the `^fat` auto-shim in elab_call.c.  A
 * bare (non-capturing) TY_FN reference is boxed via EX_FN_TO_FAT; a capturing
 * closure (boxed TY_FN), an already-fat :ptr<void> handle, or nil passes
 * through unchanged.  Used by arrow-instance dispatch so `(comp add1 dbl)`
 * feeds its function arguments to the arrow method's fat-closure parameters. */
static Expr *arrow_fat_shim(Elab *e, Expr *a) {
    if (!a) return a;
    if (a->type.kind == TY_FN && !a->type.as.fn.boxed) {
        /* fat-closure-var-passthrough: a ^fat let-binding or parameter whose
         * type was set to a concrete (fn ...) annotation is ALREADY a fat
         * closure carried as int64_t.  Re-type to ptr<void> (the natural fat
         * carrier) so it passes through without being double-boxed into a
         * __tur_fatshim_void___void__ wrapper.  That shim calls slot[1] as a
         * bare one-arg fn, but slot[1] here IS a fat closure whose thunk
         * expects two arguments (env + arg), causing a segfault.
         * Mirrors the is_fat guard in elab_call.c (two-level-sf-closure-
         * return-miscompiles-out-binding). */
        if (a->kind == EX_VAR && a->as.var.binding && a->as.var.binding->is_fat) {
            a->type = TYPE_PTR_VOID;
            return a;
        }
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID, a->span);
        shim->as.fn_to_fat_.inner = a;
        return shim;
    }
    return a;
}

/* True when `tc` has an instance whose head is the function arrow (TY_FN).
 * Used to defer a structurally-return-dispatch method (e.g. `comp [f g] : a`,
 * whose untyped params do not mention the class variable) to argument-based
 * dispatch when the call supplies arrow arguments: the function argument's type
 * selects the arrow instance, so no return-type ascription is needed. */
static bool typeclass_has_arrow_instance(TypeClassEnv *env, const TypeClass *tc) {
    for (TypeClassInstance *inst = env->instances; inst; inst = inst->next) {
        if (inst->typeclass != tc) continue;
        if (inst->n_type_args > 0 && inst->type_args[0].kind == TY_FN) return true;
    }
    return false;
}

static Expr *try_synth_recursive_eq(Elab *e, TypeClassInstance *outer_inst,
                                     Expr *obj, Expr *other_arg, Span span) {
    if (!outer_inst || outer_inst->n_type_args == 0) return NULL;
    if (obj->type.kind != TY_APP || !obj->type.as.app.arg) return NULL;
    /* Look up the helper for this typed-collection. */
    const StructDef *sd = outer_inst->type_args[0].as.struct_.def;

    /* GHE5/#4: content-keyed structural equality for Map[K V].  For a Map the
     * outermost TY_APP arg is V and the inner is K (Map[K V] = app(app(Map,K),V)).
     * The generic Eq[Map] instance compares keys by carrier identity, which is
     * wrong for content keys (:cstr -- distinct pointers with equal text; a
     * heap-boxed struct -- distinct boxes with equal fields).  Since this
     * dispatch site is *concrete*, thread the per-K MapKey comparator -- exactly
     * what map-assoc/map-get do at their concrete call sites -- into the
     * content-aware map-eq-k? helper, alongside the recursive value comparator.
     *
     * Key witnesses to drive MapKey[K] dispatch (mk-cmp ignores its argument
     * value -- it returns a constant carrier-ABI comparator -- so any value of
     * type K serves):
     *   - :cstr        -> (mk-cmp (:: 0 cstr))          (pointer carrier; the
     *                                                    int->cstr ascription is
     *                                                    a no-op)
     *   - struct K     -> (mk-cmp (make-struct K 0 ...)) (a by-value zero struct,
     *                     matching mk-cmp[K]'s by-value ABI; (:: 0 K) would
     *                     instead deref a non-pointer aggregate).  Scoped to
     *                     all-:int-field structs so the 0 literals type-check.
     * Int/bool/float and opaque-over-int keys are inline values whose carrier
     * identity already coincides with content equality, so they are left to the
     * generic path. */
    if (sd && strcmp(sd->name, "Map") == 0 &&
        obj->type.as.app.fn && obj->type.as.app.fn->kind == TY_APP &&
        obj->type.as.app.fn->as.app.arg) {
        const Type *v_type = obj->type.as.app.arg;
        const Type *k_type = obj->type.as.app.fn->as.app.arg;
        Form *kf = NULL;
        if (k_type->kind == TY_CSTR) {
            kf = build_mapkey_cmp_form(e, k_type, span);
        } else if (k_type->kind == TY_STRUCT && k_type->as.struct_.def &&
                   !k_type->as.struct_.def->is_opaque &&
                   k_type->as.struct_.def->n_fields > 0) {
            /* Build (mk-cmp (make-struct K z1 z2 ...)) for any struct whose
             * fields all have synthesisable zero literals (int, bool, float,
             * cstr, and their fixed-width variants).  Uses zero_form_for_field
             * so non-:int field types (e.g. :float32, :cstr, :bool) are now
             * handled too. */
            const StructDef *kd = k_type->as.struct_.def;
            uint32_t n_ms = (uint32_t)kd->n_fields + 2; /* make-struct Name f... */
            Form **ms_items = (Form **)arena_alloc(e->arena, n_ms * sizeof(Form *));
            ms_items[0] = form_sym(e->arena, span,
                                   intern_cstr(e->st, "make-struct"));
            ms_items[1] = form_sym(e->arena, span,
                                   intern_cstr(e->st, kd->name));
            bool ok = true;
            for (uint8_t fi = 0; fi < kd->n_fields; fi++) {
                Form *zf = zero_form_for_field(e, &kd->fields[fi], span);
                if (!zf) { ok = false; break; }
                ms_items[2 + fi] = zf;
            }
            if (ok) {
                Form *ms = form_list(e->arena, span, ms_items, n_ms);
                Form **mk_items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
                mk_items[0] = form_sym(e->arena, span, intern_cstr(e->st, "mk-cmp"));
                mk_items[1] = ms;
                kf = form_list(e->arena, span, mk_items, 2);
            }
        }
        if (kf) {
            Form *vf = build_comparator_lambda(e, v_type, span);
            if (!vf) return NULL;
            Expr *kcmp = elab_form(e, kf);
            Expr *vcmp = elab_form(e, vf);
            if (!kcmp || !vcmp) return NULL;
            Binding *mek_b = scope_lookup(&e->global, intern_cstr(e->st, "map-eq-k?"));
            if (!mek_b) return NULL;
            Expr **ca = (Expr **)arena_alloc(e->arena, 4 * sizeof(Expr *));
            ca[0] = obj; ca[1] = other_arg; ca[2] = kcmp;
            ca[3] = box_synth_comparator(e, vcmp);   /* value comparator: ^fat */
            Expr *out = expr_new(e->arena, EX_CALL, TYPE_BOOL, span);
            out->as.call_.fn_binding = mek_b;
            out->as.call_.fn_expr    = NULL;
            out->as.call_.args       = ca;
            out->as.call_.n_args     = 4;
            out->as.call_.dict_arg   = NULL;
            return out;
        }
        /* Not an intercepted key type -- fall through to the generic path. */
    }

    uint8_t n_comparators = 0;
    const Symbol *helper_sym = helper_eq_symbol_for_struct(e, sd, &n_comparators);
    if (!helper_sym || (n_comparators != 1 && n_comparators != 2)) return NULL;
    Binding *helper_b = scope_lookup(&e->global, helper_sym);
    if (!helper_b) return NULL;

    /* Collect the type-arg(s) the helper's comparator(s) target.
     *
     * For 1-comparator helpers the comparator targets the OUTERMOST
     * arg of the TY_APP chain.  This matches every typed-collection
     * helper signature we currently support:
     *   Vec[A]     -> vec-eq?  with cmp for A     (only arg)
     *   Cons[A]    -> list-eq? with cmp for A     (only arg)
     *   Option[A]  -> option-eq? with cmp for A   (only arg)
     *   Map[K V]   -> map-eq?  with cmp for V     (outermost = V;
     *                                                K rides on HAMT hash)
     *
     * For 2-comparator helpers (Pair[A B], Result[A B]) the helpers
     * expect args in source order: (fst-cmp, snd-cmp) and
     * (ok-cmp, err-cmp).  TY_APP storage is innermost-first so we
     * reverse: source-order arg[0] = A (innermost in storage). */
    const Type *args_collected[2] = {0};
    uint8_t n_collected = 0;
    if (n_comparators == 1) {
        args_collected[0] = obj->type.as.app.arg;
        n_collected = 1;
    } else {
        const Type *raw[4];
        uint8_t n_raw = 0;
        for (const Type *tx = &obj->type;
             tx && tx->kind == TY_APP && n_raw < 4;
             tx = tx->as.app.fn) {
            if (tx->as.app.arg) raw[n_raw++] = tx->as.app.arg;
        }
        /* raw is innermost-first; reverse to source order. */
        for (uint8_t i = 0; i < n_raw && n_collected < 2; i++) {
            args_collected[n_collected++] = raw[n_raw - 1 - i];
        }
    }
    if (n_collected < n_comparators) return NULL;

    /* Only fire when at least one arg type is recursive (TY_APP).
     * If all the relevant args are primitive, the single-level F3-7
     * path already produces the right answer and we should not
     * intercept (the existing dispatch is potentially more
     * efficient via the static singleton vtable). */
    bool any_recursive = false;
    for (uint8_t i = 0; i < n_comparators; i++) {
        if (args_collected[i] && args_collected[i]->kind == TY_APP) {
            any_recursive = true;
            break;
        }
    }
    if (!any_recursive) return NULL;

    /* Build a comparator lambda per arg.  Elaborate them now so any
     * type errors surface before we commit to the synthesised call. */
    Expr *lambdas[2] = {0};
    for (uint8_t i = 0; i < n_comparators; i++) {
        Form *lf = build_comparator_lambda(e, args_collected[i], span);
        if (!lf) return NULL;
        lambdas[i] = elab_form(e, lf);
        if (!lambdas[i]) return NULL;
    }

    /* Build EX_CALL to helper with [obj, other, lambda0, ...]. */
    uint32_t total_args = 2 + (uint32_t)n_comparators;
    Expr **call_args = (Expr **)arena_alloc(e->arena, total_args * sizeof(Expr *));
    call_args[0] = obj;
    call_args[1] = other_arg;
    for (uint8_t i = 0; i < n_comparators; i++) {
        /* CRU B-3: the *-eq? carrier helpers now fat-dispatch their value/element
         * comparator(s); box each synthesized comparator so the captureless
         * lambda arrives as a fat closure rather than a bare pointer. */
        call_args[2 + i] = box_synth_comparator(e, lambdas[i]);
    }

    Expr *out = expr_new(e->arena, EX_CALL, TYPE_BOOL, span);
    out->as.call_.fn_binding = helper_b;
    out->as.call_.fn_expr    = NULL;
    out->as.call_.args       = call_args;
    out->as.call_.n_args     = total_args;
    out->as.call_.dict_arg   = NULL;
    return out;
}

/* Phase 15: Typeclasses */

/* Parse a single typeclass method definition from a Form.
 * Syntax: (method-name [param1 : type1, param2 : type2, ...] : return-type)
 * or: (method-name [param1 param2 ...] : return-type) - types inferred from usage
 */
/* Phase RT: resolve a keyword name to a class type-param index, returning the
 * matching type-param symbol (so a method return/param `:a` becomes TY_TYVAR a
 * when `a` is one of the class's type parameters). */
static const Symbol *class_type_param_match(const char *kw_name, uint32_t kw_len,
                                            const Symbol **class_type_params,
                                            uint8_t n_class_type_params) {
    for (uint8_t i = 0; i < n_class_type_params; i++) {
        const Symbol *tp = class_type_params[i];
        if (tp && tp->len == kw_len && memcmp(tp->name, kw_name, kw_len) == 0) {
            return tp;
        }
    }
    return NULL;
}

/* M7 HKT (flag-gated by g_m7_hkt_enabled): is `sym` a candidate method-level
 * type variable -- a lowercase-leading symbol that is neither a primitive type
 * keyword, a special type-form head (fn/c-fn/void/any), nor a class type param?
 *
 * Rationale (corrected root cause, 2026-06-18): an HKT-applied method signature
 * such as `(gmap [container : (g a) f : (fn [a] b)] : (g b))` parses the HEAD
 * `g` to a NAMED TY_TYVAR (it is a class type param), but the element tyvars
 * `a`/`b` are NOT class params, so type_expr_from_form takes its "unknown ->
 * opaque struct" fallback and they become ANONYMOUS TY_STRUCT{def=NULL}.  That
 * is why `(g b)` resolves to `(type-app tyvar ?)` instead of `(type-app g b)`:
 * the element is anonymous, so layer-0 head substitution yields `(Option ?)`
 * with no named element to refine per call.  Collecting these implicit
 * method-level tyvars and threading them as additional type params makes them
 * resolve to NAMED tyvars, the prerequisite for per-call element refinement. */
static bool m7_is_method_tyvar_name(const Symbol *sym,
                                    const Symbol **class_tp, uint8_t n_class_tp) {
    if (!sym || sym->len == 0) return false;
    char c = sym->name[0];
    if (c < 'a' || c > 'z') return false;            /* tyvars are lowercase */
    if (typekind_from_symbol(sym->name) != TY_UNKNOWN) return false; /* primitive */
    if ((sym->len == 4 && memcmp(sym->name, "void", 4) == 0) ||
        (sym->len == 3 && memcmp(sym->name, "any",  3) == 0) ||
        (sym->len == 2 && memcmp(sym->name, "fn",   2) == 0) ||
        (sym->len == 4 && memcmp(sym->name, "c-fn", 4) == 0))
        return false;
    for (uint8_t i = 0; i < n_class_tp; i++)
        if (class_tp[i] == sym) return false;
    return true;
}

/* M7 HKT: walk a (type-position) form collecting de-duplicated method-level
 * tyvar symbols into `out` (capacity `max`, current count `*n_out`). */
static void m7_collect_form_tyvars(const Form *form,
                                   const Symbol **class_tp, uint8_t n_class_tp,
                                   const Symbol **out, uint8_t *n_out, uint8_t max) {
    if (!form) return;
    switch (form->tag) {
        case F_SYM:
            if (m7_is_method_tyvar_name(form->as.sym, class_tp, n_class_tp)) {
                for (uint8_t i = 0; i < *n_out; i++)
                    if (out[i] == form->as.sym) return;
                if (*n_out < max) out[(*n_out)++] = form->as.sym;
            }
            return;
        case F_TYPE_ANN:
        case F_LIST:
        case F_VEC:
            for (uint32_t i = 0; i < form->as.list.len; i++)
                m7_collect_form_tyvars(form->as.list.items[i], class_tp, n_class_tp,
                                       out, n_out, max);
            return;
        default:
            return;
    }
}

static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span,
                                               uint32_t *out_body_start,
                                               const Symbol **class_type_params,
                                               uint8_t n_class_type_params,
                                               const Kind *class_type_param_kinds,
                                               const Symbol **assoc_type_names,
                                               uint8_t n_assoc_type_names) {
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
    
    /* M7 HKT (flag-gated): build the effective type-param array used for
     * type-expression resolution = the class type params plus any implicit
     * method-level tyvars collected from the param/return type forms.  When the
     * flag is OFF this is identical to (class_type_params, n_class_type_params),
     * so the path is byte-for-byte inert.  See m7_is_method_tyvar_name. */
    const Symbol **eff_tp = class_type_params;
    uint8_t n_eff_tp = n_class_type_params;
    /* Thread the class type-param kinds through to type-expression resolution so
     * an HKT param `^m` (kind `* -> *`) used in an APPLIED position inside a
     * method signature -- e.g. `bind`'s `k : (fn [a] (m b))` -- resolves to a
     * TY_TYVAR carrying KIND_ARROW, not the KIND_STAR default.  Without this,
     * `(m b)` reconstructed by call_instantiate_type at the instance call site
     * trips type_app's kind check (TUR-E0012).  The fmap shape (fn returns a
     * bare element `b`) never builds `(m _)` so it was unaffected; only the
     * monadic shapes (fn returning an applied HKT type) hit it.  See
     * docs/reported/m7-hkt-fn-returning-applied-type-kind-mismatch.md. */
    Kind *eff_kinds = (Kind *)class_type_param_kinds;
    if (g_m7_hkt_enabled) {
        enum { M7_MAX_TP = 16 };
        const Symbol **buf =
            (const Symbol **)arena_alloc(e->arena, M7_MAX_TP * sizeof(const Symbol *));
        Kind *kbuf = (Kind *)arena_alloc(e->arena, M7_MAX_TP * sizeof(Kind));
        uint8_t n = 0;
        for (uint8_t i = 0; i < n_class_type_params && n < M7_MAX_TP; i++) {
            kbuf[n] = class_type_param_kinds ? class_type_param_kinds[i] : KIND_STAR;
            buf[n] = class_type_params[i];
            n++;
        }
        uint8_t n_class_in_buf = n;
        m7_collect_form_tyvars(params_form, class_type_params, n_class_type_params,
                               buf, &n, M7_MAX_TP);
        /* Locate the return form (index 2, or 3 when an effect-row #{...}
         * precedes it) and collect its element tyvars too. */
        {
            uint32_t ri = 2;
            if (method_form->as.list.len > ri &&
                method_form->as.list.items[ri]->tag == F_MAP) ri++;
            if (method_form->as.list.len > ri)
                m7_collect_form_tyvars(method_form->as.list.items[ri],
                                       class_type_params, n_class_type_params,
                                       buf, &n, M7_MAX_TP);
        }
        /* The method-level tyvars appended after the class params are ordinary
         * element variables of kind `*`. */
        for (uint8_t i = n_class_in_buf; i < n; i++) kbuf[i] = KIND_STAR;
        eff_tp = buf;
        eff_kinds = kbuf;
        n_eff_tp = n;
    }

    /* Parse parameters */
    uint8_t n_params = params_form->as.list.len;
    const Symbol **param_names = NULL;
    Type *param_types = NULL;
    /* Phase CCL: callable-param flag array — parallel to param_names/param_types.
     * param_is_fn[i] = true when the i-th param is declared with [name :fn] syntax,
     * marking it as a single-argument callable that should receive tur_poly_fn_t. */
    bool *param_is_fn = NULL;
    /* Prereq 4: per-param flag tracking whether the user wrote an explicit type
     * annotation. Without this, the elaborator can't tell `[v : int]` (explicit)
     * from `[v]` (default to int) -- both become TY_INT in param_types. The
     * substitution site at elab_definstance consults this flag to leave
     * explicit-`:int` params alone instead of rewriting them to the class tyvar. */
    bool *param_explicit_type = NULL;

    if (n_params > 0) {
        param_names = (const Symbol **)arena_alloc(e->arena, n_params * sizeof(const Symbol *));
        param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
        param_is_fn = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        param_explicit_type = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        for (uint8_t i = 0; i < n_params; i++) {
            param_is_fn[i] = false;
            param_explicit_type[i] = false;
        }

        /* actual_p: number of real parameters encountered (keywords don't count). */
        uint8_t actual_p = 0;
        for (uint8_t i = 0; i < n_params; i++) {
            Form *p = params_form->as.list.items[i];
            /* ECS E2d-P6 (Issue 2 secondary): substructural / borrow caret
             * markers (^borrow, ^mut, ^unique, ^linear, ^affine, ^relevant,
             * ^fat) annotate the *next* parameter; they are not parameters
             * themselves.  Skip them so they do not consume a param slot --
             * otherwise `[^borrow s : S idx : int val : E]` mis-aligned the
             * parsed param types (treating `^borrow` as a param), so an
             * instance method inheriting those types saw the wrong type per
             * position.  (The borrow discipline itself is enforced on the
             * elaborated instance-method FnDefs and at call sites.) */
            if (p->tag == F_SYM &&
                (p->as.sym == e->sym_caret_borrow ||
                 p->as.sym == e->sym_caret_mut ||
                 p->as.sym == e->sym_caret_unique ||
                 p->as.sym == e->sym_caret_linear ||
                 p->as.sym == e->sym_caret_affine ||
                 p->as.sym == e->sym_caret_relevant ||
                 p->as.sym == e->sym_caret_fat)) {
                continue;
            }
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
                /* Prereq 4: any explicit annotation -- :int, :bool, :cstr,
                 * :ptr<void>, :fn, or a class tyvar -- pins the param type
                 * and the elaborator must not rewrite it later. */
                param_explicit_type[prev] = true;
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
                    /* Phase RT: a parameter typed `:a` naming a class type
                     * parameter becomes a TY_TYVAR -- this is the dispatch
                     * witness (e.g. schema-of [_ :a]). */
                    const Symbol *tp = class_type_param_match(kw->name, kw->len,
                                                              class_type_params,
                                                              n_class_type_params);
                    if (tp) {
                        param_types[prev] = type_tyvar_named(tp->name);
                    } else {
                        /* ECS E2d-P6 (Issue 1): a parameter typed by an associated
                         * type member (e.g. `val :Elem`) is an abstract projection
                         * resolved per instance; carry it as a named TY_TYVAR. */
                        const Symbol *am = class_type_param_match(kw->name, kw->len,
                                                                  assoc_type_names,
                                                                  n_assoc_type_names);
                        if (am) {
                            param_types[prev] = type_tyvar_named(am->name);
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "unsupported type in typeclass method parameter");
                            return NULL;
                        }
                    }
                }
            } else if (p->tag == F_TYPE_ANN) {
                if (actual_p == 0) {
                    diag_emit(DIAG_ERROR, p->span,
                              "type annotation without preceding parameter");
                    return NULL;
                }
                /* Prereq 4: same as the F_KEYWORD branch -- an explicit
                 * spaced `: type` pins the param type. */
                param_explicit_type[actual_p - 1] = true;
                /* Phase CCL: `: fn` (F_TYPE_ANN wrapping F_SYM("fn")) is the
                 * spaced form of `:fn` -- the poly-closure carrier marker. */
                Form *inner_f = (p->as.list.len > 0) ? p->as.list.items[0] : NULL;
                if (inner_f &&
                    (inner_f->tag == F_SYM || inner_f->tag == F_KEYWORD) &&
                    inner_f->as.sym->len == 2 &&
                    memcmp(inner_f->as.sym->name, "fn", 2) == 0) {
                    param_types[actual_p - 1] = TYPE_PTR_VOID;
                    param_is_fn[actual_p - 1] = true;
                } else if (inner_f &&
                           (inner_f->tag == F_SYM || inner_f->tag == F_KEYWORD) &&
                           class_type_param_match(inner_f->as.sym->name,
                                                  inner_f->as.sym->len,
                                                  assoc_type_names,
                                                  n_assoc_type_names)) {
                    /* ECS E2d-P6 (Issue 1): a spaced `: Elem` naming an associated
                     * type member is an abstract projection; carry it as a named
                     * TY_TYVAR (resolved per instance, like the keyword form). */
                    const Symbol *am = class_type_param_match(inner_f->as.sym->name,
                                                              inner_f->as.sym->len,
                                                              assoc_type_names,
                                                              n_assoc_type_names);
                    param_types[actual_p - 1] = type_tyvar_named(am->name);
                } else {
                    /* ECS E2d-P6 (Issue 2): resolve class type parameters used in
                     * a spaced `: S` (or parametric `: (Dense S)`) parameter
                     * annotation to a named TY_TYVAR.  Without the class type
                     * params here, `S` was parsed as an opaque/unknown type and
                     * the return-only-dispatch detector could not see that S
                     * appears in argument position, mis-classifying an
                     * argument-dispatchable method as return-only. */
                    Type *ft = inner_f
                        ? type_expr_from_form(e, inner_f, NULL,
                                              eff_tp, eff_kinds,
                                              n_eff_tp)
                        : NULL;
                    if (!ft) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "unsupported type in typeclass method parameter");
                        return NULL;
                    }
                    param_types[actual_p - 1] = *ft;
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
                    /* `: type-expr` compound annotation; special-case `: fn`
                     * (the poly-closure carrier) before falling through to the
                     * generic type-expression parser. */
                    Form *ti = (type_f->as.list.len > 0) ? type_f->as.list.items[0] : NULL;
                    if (ti &&
                        (ti->tag == F_SYM || ti->tag == F_KEYWORD) &&
                        ti->as.sym->len == 2 &&
                        memcmp(ti->as.sym->name, "fn", 2) == 0) {
                        param_types[actual_p] = TYPE_PTR_VOID;
                        param_is_fn[actual_p] = true;
                    } else {
                        Type *ft = ti
                            ? type_expr_from_form(e, ti, NULL,
                                                  eff_tp, eff_kinds,
                                                  n_eff_tp)
                            : NULL;
                        if (!ft) {
                            diag_emit(DIAG_ERROR, type_f->span,
                                      "unsupported type form in typeclass method parameter");
                            return NULL;
                        }
                        param_types[actual_p] = *ft;
                    }
                } else if (type_f->tag == F_LIST || type_f->tag == F_VEC) {
                    /* Phase HRT3: allow forall/exists type forms as parameter types */
                    Type *ft = type_expr_from_form(e, type_f, NULL,
                                                   eff_tp, eff_kinds,
                                                   n_eff_tp);
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
        /* Spaced `: T` over a single symbol/keyword: normalise to F_KEYWORD
         * so the class-type-param match below runs. */
        if (ret_form->tag == F_TYPE_ANN && ret_form->as.list.len == 1 &&
            (ret_form->as.list.items[0]->tag == F_KEYWORD ||
             ret_form->as.list.items[0]->tag == F_SYM)) {
            Form *inner = ret_form->as.list.items[0];
            Form *kf = (Form *)arena_alloc(e->arena, sizeof(Form));
            *kf = *inner;
            kf->tag = F_KEYWORD;
            ret_form = kf;
        }
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
                /* Phase RT: a return type `:a` naming a class type parameter
                 * becomes a TY_TYVAR.  This is the return-only dispatch case:
                 * the dispatch variable appears only in the result, so the
                 * instance must be selected from the call's expected type. */
                const Symbol *tp = class_type_param_match(kw->name, kw->len,
                                                          class_type_params,
                                                          n_class_type_params);
                if (tp) {
                    return_type = type_tyvar_named(tp->name);
                } else {
                    /* ECS E2d-P6 (Issue 1): a return type naming an associated
                     * type member (e.g. `(type Elem : Type)` -> `: Elem`) is an
                     * abstract projection over the instance type, resolved per
                     * instance.  Represent it as a named TY_TYVAR carrying the
                     * associated-type name; elab_definstance substitutes it with
                     * the instance's `(type Elem = T)` binding, and a call site
                     * recovers the concrete result from the dispatched instance
                     * method (argument dispatch keys on the receiver type). */
                    const Symbol *am = class_type_param_match(kw->name, kw->len,
                                                              assoc_type_names,
                                                              n_assoc_type_names);
                    if (am) {
                        return_type = type_tyvar_named(am->name);
                    } else {
                        diag_emit(DIAG_ERROR, ret_form->span,
                                  "unsupported return type in typeclass method");
                        return NULL;
                    }
                }
            }
        } else if (ret_form->tag == F_TYPE_ANN) {
            /* `: type-expr` compound return type annotation */
            /* Prereq 5: pass the class's type parameters through so the return
             * type's `a` inside a parameterized form like `(Result a cstr)`
             * resolves to TY_TYVAR rather than an undefined opaque struct.
             * Without this, return-dispatch detection works (rt_type_mentions_tyvar
             * recurses through TY_APP) but the call-site unification can't extract
             * the `a`-position binding, so ascriptions like
             * `(:: (decode ...) (Result cstr cstr))` silently fall back to the
             * first instance. (Bare-`a` return type happens to work because the
             * raw `a` symbol resolves via the class_type_param_match path used
             * elsewhere in this function, not via type_expr_from_form.) */
            Type *ft = (ret_form->as.list.len > 0)
                ? type_expr_from_form(e, ret_form->as.list.items[0], NULL,
                                      eff_tp, eff_kinds,
                                      n_eff_tp)
                : NULL;
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            return_type = *ft;
        } else if (ret_form->tag == F_LIST || ret_form->tag == F_VEC) {
            /* Phase HRT3: allow forall/exists type forms as return types.
             * Prereq 5: same as above -- pass class type params so a
             * nested `a` resolves to TY_TYVAR. */
            Type *ft = type_expr_from_form(e, ret_form, NULL,
                                           eff_tp, eff_kinds,
                                           n_eff_tp);
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
    method->param_explicit_type = param_explicit_type;
    method->n_params = n_params;
    method->return_type = return_type;
    method->effect_row = method_effect_row;  /* ER3: NULL if not annotated */
    method->default_fn_expr = NULL;          /* ER3: set by elab_defclass if body forms exist */
    return method;
}

/* Compare a freshly-parsed typeclass against an existing entry of the same
 * name. Returns true iff all observable signature surface matches: same
 * type-parameter count and kinds, same method count, and per-method the same
 * name, parameter count, parameter type-kinds, and return type-kind. Used to
 * accept idempotent stdlib pre-declarations while rejecting genuine
 * redefinitions. */
static bool typeclass_signatures_match(const TypeClass *existing,
                                       uint8_t n_type_params,
                                       const Kind *type_param_kinds,
                                       uint8_t n_methods,
                                       const TypeClassMethod *methods) {
    if (existing->n_type_params != n_type_params) return false;
    if (existing->n_methods     != n_methods)     return false;
    for (uint8_t i = 0; i < n_type_params; i++) {
        Kind a = existing->type_param_kinds ? existing->type_param_kinds[i] : KIND_STAR;
        Kind b = type_param_kinds          ? type_param_kinds[i]          : KIND_STAR;
        if (a != b) return false;
    }
    for (uint8_t i = 0; i < n_methods; i++) {
        const TypeClassMethod *em = &existing->methods[i];
        const TypeClassMethod *nm = &methods[i];
        if (em->name != nm->name) return false; /* symbols interned -- ptr eq */
        if (em->n_params != nm->n_params) return false;
        if (em->return_type.kind != nm->return_type.kind) return false;
        for (uint8_t j = 0; j < em->n_params; j++) {
            if (em->param_types[j].kind != nm->param_types[j].kind) return false;
        }
    }
    return true;
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

    /* Check if already defined. The decision (skip silently vs hard-error) is
     * deferred until after the new defclass is parsed so we can compare
     * signatures: identical signature = idempotent silent skip (preserves the
     * stdlib pre-declaration of Eq, Functor, etc.); different signature =
     * "typeclass 'Foo' is already defined" diagnostic. See
     * typeclass_signatures_match above. */
    TypeClass *existing = typeclass_env_lookup_typeclass(&e->typeclass_env, name);
    
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

    /* assoc-types-plan: a defclass body interleaves method declarations with
     * associated-type declarations `(type Name : Type)`.  Classify each body
     * form before parsing: a `type` head whose second element is a bare symbol
     * (not a [params] vector) is an associated-type member; everything else is
     * a method.  (A method literally named `type` carries a `[params]` vector
     * as its second element, so the discriminator never misfires.) */
    uint32_t n_body_forms = call->as.list.len - methods_start;
    uint32_t *method_form_idx = n_body_forms > 0
        ? (uint32_t *)arena_alloc(e->arena, n_body_forms * sizeof(uint32_t)) : NULL;
    const Symbol **assoc_type_names = n_body_forms > 0
        ? (const Symbol **)arena_alloc(e->arena, n_body_forms * sizeof(const Symbol *))
        : NULL;
    uint8_t n_assoc_types = 0;
    const Symbol *sym_type_kw = intern_cstr(e->st, "type");

    for (uint32_t i = methods_start; i < call->as.list.len; i++) {
        Form *bf = call->as.list.items[i];
        if (bf->tag == F_LIST && bf->as.list.len >= 2 &&
            bf->as.list.items[0]->tag == F_SYM &&
            bf->as.list.items[0]->as.sym == sym_type_kw &&
            bf->as.list.items[1]->tag == F_SYM) {
            assoc_type_names[n_assoc_types++] = bf->as.list.items[1]->as.sym;
            continue;
        }
        method_form_idx[n_methods++] = i;
    }

    if (n_methods == 0 && n_assoc_types == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires at least one method or associated type");
        return NULL;
    }

    /* Allocate methods array */
    methods = n_methods > 0
        ? (TypeClassMethod *)arena_alloc(e->arena, n_methods * sizeof(TypeClassMethod))
        : NULL;
    /* Per-method body_start so the default-body elaboration can run as a
     * second pass after the redefinition check. */
    uint32_t *method_body_starts = n_methods > 0
        ? (uint32_t *)arena_alloc(e->arena, n_methods * sizeof(uint32_t)) : NULL;

    /* Second pass (signatures only): parse each method's signature.  Default
     * bodies are elaborated in a third pass below so that an idempotent
     * stdlib re-declare can short-circuit without registering orphan
     * __default_* file-level FnDefs. */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[method_form_idx[i]];
        uint32_t body_start = 0;
        TypeClassMethod *method = parse_typeclass_method(e, method_form, call->span, &body_start,
                                                         type_params, n_type_params,
                                                         type_param_kinds,
                                                         assoc_type_names, n_assoc_types);
        if (!method) return NULL;
        methods[i] = *method;
        method_body_starts[i] = body_start;
    }

    /* Redefinition check: same name already registered -> compare signatures.
     * Identical = idempotent silent skip (stdlib pre-declaration case);
     * different = hard error. */
    if (existing) {
        if (existing->n_assoc_types == n_assoc_types &&
            typeclass_signatures_match(existing, n_type_params,
                                       type_param_kinds, n_methods, methods)) {
            return e_nil(e, call->span);
        }
        diag_emit(DIAG_ERROR, call->span,
                  "typeclass '%s' is already defined", name->name);
        return NULL;
    }

    /* Third pass: elaborate default method bodies (if any) now that the
     * defclass is committed. */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[method_form_idx[i]];
        uint32_t body_start = method_body_starts[i];
        TypeClassMethod *method = &methods[i];

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
            memset(def_fd, 0, sizeof(FnDef));
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
            /* LS2/LS3: no surface borrow lifetimes on a synthesised default
             * method; give the lifetime pass a clean context + return Type. */
            lifetime_context_init(&def_fd->lifetime_ctx);
            def_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);

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
    tc->assoc_type_names  = n_assoc_types > 0 ? assoc_type_names : NULL;
    tc->n_assoc_types     = n_assoc_types;
    /* Phase HKT-P4: record the file that defined this typeclass. */
    tc->origin_file_id    = call->span.file_id;
    /* method-vs-defn clash check: a class registered during stdlib auto-load is
     * "intentionally overridable" by a same-named user defn, so it is exempt
     * from the TUR-W0039 clash warning (see elab_toplevel.c). */
    tc->from_stdlib       = e->in_stdlib_load;

    /* Create a TYPECLASS_DEF expression for codegen */
    Expr *tc_expr = expr_new(e->arena, EX_TYPECLASS_DEF, TYPE_NIL, call->span);
    tc_expr->as.typeclass_def_.typeclass = tc;
    elab_register_file_def(e, tc_expr);
    
    /* Create a nil expression as the result (defclass returns nothing) */
    return e_nil(e, call->span);
}

/* Build the codegen type-arg suffix for a typeclass instance, e.g. "_int",
 * "_option", or "_result_int".  This suffix is the discriminator baked into
 * every instance C symbol -- the method functions (__inst_<Class>_<method>SUFFIX)
 * and the dictionary struct/singleton (dict_<Class>SUFFIX).  It mirrors
 * emit_dict_name (emit_core.c) and is the single source of truth shared by both
 * the method-name builder below and the duplicate-instance guard, so the dedup
 * key can never drift from the names actually emitted.
 *
 * Two instances of the same typeclass collide in generated C iff their suffixes
 * are byte-equal.  Returns false on buffer overflow (caller emits the error). */
static bool build_inst_type_suffix(const Type *type_args,
                                   const Symbol **type_arg_syms,
                                   uint8_t n_type_args,
                                   char *out, size_t outlen) {
    size_t len = 0;
    if (outlen == 0) return false;
    out[0] = '\0';
    for (uint8_t j = 0; j < n_type_args; j++) {
        const char *type_component = NULL;
        char ctor_name_buf[32];  /* for TY_STRUCT/TY_APP constructor names (source) */
        char ctor_mangle_buf[128]; /* injective-mangled form of ctor_name_buf */
        switch (type_args[j].kind) {
            case TY_INT:     type_component = "int";     break;
            case TY_BOOL:    type_component = "bool";    break;
            case TY_CSTR:    type_component = "cstr";    break;
            case TY_NIL:     type_component = "nil";     break;
            case TY_PTR_VOID: type_component = "ptr_void"; break;
            case TY_INT8:    type_component = "int8";    break;
            case TY_INT16:   type_component = "int16";   break;
            case TY_INT32:   type_component = "int32";   break;
            case TY_UINT8:   type_component = "uint8";   break;
            case TY_UINT16:  type_component = "uint16";  break;
            case TY_UINT32:  type_component = "uint32";  break;
            case TY_UINT64:  type_component = "uint64";  break;
            case TY_FLOAT:   type_component = "float";   break;
            case TY_FLOAT32: type_component = "float32"; break;
            case TY_FLOAT64: type_component = "float64"; break;
            case TY_SYM:     type_component = "Sym";     break;
            case TY_FN:      type_component = "arrow";   break;
            case TY_STRUCT:
                /* Phase HKT H3: use the original symbol name when available,
                 * falling back to "T" for unnamed struct type args. */
                if (type_arg_syms && type_arg_syms[j]) {
                    uint32_t sym_len = type_arg_syms[j]->len;
                    if (sym_len >= sizeof(ctor_name_buf))
                        sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                    memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                    ctor_name_buf[sym_len] = '\0';
                    tur_mangle_ident(ctor_name_buf, ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else if (type_args[j].as.struct_.def) {
                    type_component = type_args[j].as.struct_.def->name;
                } else {
                    type_component = "T";
                }
                break;
            case TY_APP: {
                /* Phase HKT §3: partial type application -- encode as "ctor_arg" */
                const char *ctor_part = "T";
                const char *arg_part  = "T";
                if (type_arg_syms && type_arg_syms[j]) {
                    ctor_part = type_arg_syms[j]->name;
                }
                if (type_args[j].as.app.arg) {
                    const char *n = type_name(*type_args[j].as.app.arg);
                    if (n) arg_part = n;
                }
                char mctor[64], marg[64];
                tur_mangle_ident(ctor_part, mctor, sizeof(mctor));
                tur_mangle_ident(arg_part, marg, sizeof(marg));
                snprintf(ctor_mangle_buf, sizeof(ctor_mangle_buf), "%s_%s", mctor, marg);
                type_component = ctor_mangle_buf;
                break;
            }
            default: type_component = "T"; break;
        }
        int written = snprintf(out + len, outlen - len, "%s%s",
                               j == 0 ? "_" : "", type_component);
        if (written < 0 || (size_t)written >= outlen - len) return false;
        len += (size_t)written;
    }
    return true;
}

/* ECS E2d-P6 (Issue 2 secondary): substitute a class method's parameter type --
 * which may reference the class's type parameters as named TY_TYVARs (e.g.
 * `val : E` parses to TY_TYVAR("E")) -- with the concrete instance type args, so
 * an instance method body whose params are unannotated (`[s idx val]`) inherits
 * the substituted types (S -> (Dense Pos), E -> Pos) instead of being left as
 * abstract tyvars.  Recurses through TY_APP so a parametric param type like
 * `(Dense E)` is rewritten too.  Returns the type unchanged when it mentions no
 * class type parameter. */
static Type elab_subst_class_tyvars(Arena *arena, Type t,
                                    const Symbol **type_params,
                                    uint8_t n_type_params,
                                    const Type *type_args,
                                    uint8_t n_type_args) {
    if (t.kind == TY_TYVAR && t.as.tyvar_.name) {
        for (uint8_t k = 0; k < n_type_params && k < n_type_args; k++) {
            if (type_params[k] &&
                strcmp(type_params[k]->name, t.as.tyvar_.name) == 0) {
                return type_args[k];
            }
        }
        return t;
    }
    if (t.kind == TY_APP) {
        if (t.as.app.fn) {
            Type *fn = (Type *)arena_alloc(arena, sizeof(Type));
            *fn = elab_subst_class_tyvars(arena, *t.as.app.fn, type_params,
                                          n_type_params, type_args, n_type_args);
            t.as.app.fn = fn;
        }
        if (t.as.app.arg) {
            Type *arg = (Type *)arena_alloc(arena, sizeof(Type));
            *arg = elab_subst_class_tyvars(arena, *t.as.app.arg, type_params,
                                           n_type_params, type_args, n_type_args);
            t.as.app.arg = arg;
        }
        return t;
    }
    return t;
}

/* M7 fix direction 1 (flag-gated): a function value stored as an HKT container
 * element (e.g. the `(fn a b)` element of `(Option (fn a b))` in the
 * Applicative `ap` shape) is physically a fat closure box -- it was boxed at
 * the producer (the EX_FN_TO_FAT shim in elab_call.c) and extracted via the
 * carrier `value` field.  For the instance-method body to CALL it correctly
 * (`((.value ff) x)`), the element fn must be marked `boxed` so the application
 * dispatches through the fat-box thunk instead of a bare fn-pointer call (which
 * reads the box address as code and segfaults).  Walk a substituted body param
 * type and box any unboxed TY_FN that sits in HKT-element position. */
static Type m7_box_hkt_element_fns(Arena *arena, Type t) {
    if (t.kind == TY_APP) {
        if (t.as.app.arg) {
            Type *arg = (Type *)arena_alloc(arena, sizeof(Type));
            *arg = m7_box_hkt_element_fns(arena, *t.as.app.arg);
            t.as.app.arg = arg;
        }
        return t;
    }
    if (t.kind == TY_FN && !t.as.fn.boxed) {
        t.as.fn.boxed = true;
        return t;
    }
    return t;
}

/* M7 HKT layer-4 (flag-gated): is this instance-method body genuinely
 * by-value-constructible?  The emit-side per-(f, A) by-value spec only works
 * when the method body constructs its `(f b)` result IN-BODY via `#{Construct}`
 * calls (`some`/`none`/`ok`/...) -- so its inner constructs recover by value.
 * A body that DELEGATES to a carrier helper (e.g. `Bifunctor [Result]`'s
 * `(result-bimap container ...)`, where `result-bimap` takes a `:int` carrier)
 * cannot, and must stay on the uniform-carrier dispatch until Phase 4.2 rewrites
 * the helper.  Recurse through if/do/let to the tail position.
 *
 * Monadic shapes (Monad `bind`): the tail of a branch may be a CALL to the
 * continuation `k` -- `(k (.value ma))` -- which returns the result family
 * `(m b)` BY VALUE (k is a fn-typed PARAMETER, not a global carrier helper).
 * Admit such a tail too, distinguished from a carrier-delegating helper by
 * (a) the callee is a local fn binding (`!is_global`), not a top-level defn,
 * and (b) the call's result type is the applied `(f b)` family (TY_APP). */
static bool m7_body_constructs_byvalue(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_IF:
            return m7_body_constructs_byvalue(e->as.if_.then_) &&
                   m7_body_constructs_byvalue(e->as.if_.else_or_null);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   m7_body_constructs_byvalue(e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET:
            return m7_body_constructs_byvalue(e->as.let_.body);
        case EX_VAR:
            /* Selection / pass-through tail (Alternative `<|>` / `or-else`):
             * the body returns an EXISTING `(f a)` value -- a parameter or local
             * of the result applied family -- directly, e.g.
             * `(if (some? x) x y)`.  Under the by-value spec the param's type is
             * the by-value `Option__int`, so returning it is already by value;
             * no in-body `#{Construct}` is needed.  Restrict to the applied
             * `(f b)` family (TY_APP) so a bare-element return (the `extract` /
             * Foldable shape, whose result is not an applied type) stays on the
             * uniform carrier path until its own probe hardens it. */
            return e->type.kind == TY_APP;
        case EX_CALL:
            if (e->as.call_.fn_binding &&
                e->as.call_.fn_binding->is_construct_template)
                return true;
            /* Monadic continuation tail call: a local (parameter) fn returning
             * the applied `(f b)` family by value. */
            if (e->as.call_.fn_binding && !e->as.call_.fn_expr &&
                !e->as.call_.fn_binding->is_global &&
                e->type.kind == TY_APP)
                return true;
            return false;
        default:
            return false;
    }
}

/* M7 HKT layers 1+3 (flag-gated): unify a method's DECLARED parameter type
 * (carrying element tyvars, e.g. `(g a)` or `(fn [a] b)`) against the ACTUAL
 * call-site argument type, recording each tyvar -> concrete-type binding.  The
 * collected bindings feed elab_subst_class_tyvars to refine the HKT result
 * `(Option b)` to a ground `(Option int)` at the call site.  Names are interned
 * so they compare by Symbol identity the same way elab_subst_class_tyvars does. */
static void m7_collect_tyvar_bindings(Elab *e, Type decl, Type act,
                                      const Symbol **names, Type *types,
                                      uint8_t *n, uint8_t max) {
    switch (decl.kind) {
        case TY_TYVAR:
            /* A free-tyvar ACTUAL carries no grounding information (e.g. the
             * element of a bare `(none)` argument).  Skip it so a sibling
             * argument that DOES ground this tyvar wins -- the Alternative
             * `(alt2 (none) (some 42))` shape, where the element is recoverable
             * from the second arg.  Without this skip the "keep the first
             * binding" rule below would lock `a` to the free element of arg 0
             * and the result would never ground.  (This also leaves the genuine
             * `ap` caveat -- `(ap (none) (some 41))`, where NO arg grounds `b` --
             * un-grounded, so it still falls back to carrier dispatch.) */
            if (act.kind == TY_TYVAR) return;
            if (decl.as.tyvar_.name) {
                for (uint8_t i = 0; i < *n; i++)
                    if (names[i] && strcmp(names[i]->name, decl.as.tyvar_.name) == 0)
                        return;  /* keep the first binding for a given tyvar */
                if (*n < max) {
                    names[*n] = symtab_intern(e->st,
                        strslice(decl.as.tyvar_.name,
                                 (uint32_t)strlen(decl.as.tyvar_.name)));
                    types[*n] = act;
                    (*n)++;
                }
            }
            return;
        case TY_APP:
            if (act.kind == TY_APP) {
                if (decl.as.app.fn && act.as.app.fn)
                    m7_collect_tyvar_bindings(e, *decl.as.app.fn, *act.as.app.fn,
                                              names, types, n, max);
                if (decl.as.app.arg && act.as.app.arg)
                    m7_collect_tyvar_bindings(e, *decl.as.app.arg, *act.as.app.arg,
                                              names, types, n, max);
            }
            return;
        case TY_FN:
            if (act.kind == TY_FN) {
                uint8_t ar = decl.as.fn.arity < act.as.fn.arity
                             ? decl.as.fn.arity : act.as.fn.arity;
                for (uint8_t i = 0; i < ar; i++) {
                    Type da = (decl.as.fn.arg_full_types && decl.as.fn.arg_full_types[i])
                              ? *decl.as.fn.arg_full_types[i]
                              : type_from_kind(decl.as.fn.arg_kinds[i]);
                    Type aa = (act.as.fn.arg_full_types && act.as.fn.arg_full_types[i])
                              ? *act.as.fn.arg_full_types[i]
                              : type_from_kind(act.as.fn.arg_kinds[i]);
                    m7_collect_tyvar_bindings(e, da, aa, names, types, n, max);
                }
                Type dr = decl.as.fn.result_full_type
                          ? *decl.as.fn.result_full_type
                          : type_from_kind(decl.as.fn.result_kind);
                Type ar2 = act.as.fn.result_full_type
                           ? *act.as.fn.result_full_type
                           : type_from_kind(act.as.fn.result_kind);
                m7_collect_tyvar_bindings(e, dr, ar2, names, types, n, max);
            }
            return;
        default:
            return;
    }
}

/* M7 layer-4 guard: does a (post-substitution) result type still carry a named,
 * un-grounded element tyvar?  When the HKT by-value monomorphization cannot
 * recover a result element tyvar from the call args -- the Applicative `ap`
 * shape, whose result element `b` lives only inside a wrapped function value
 * that erases to `ptr<void>` (docs/reported/m7-hkt-ap-fn-element-carrier-
 * erasure.md) -- the substituted result `(Option b)` keeps `b` free.  Emitting
 * a by-value spec for it mints a half-by-value method (carrier `int64_t`
 * return) while the dispatch dict references a dropped carrier base, a hard cc
 * error.  Detecting the free tyvar lets the caller fall back to the uniform
 * carrier dispatch instead. */
static bool m7_type_has_free_tyvar(Type t) {
    switch (t.kind) {
        case TY_TYVAR:
            return t.as.tyvar_.name != NULL;
        case TY_APP:
            return (t.as.app.fn && m7_type_has_free_tyvar(*t.as.app.fn)) ||
                   (t.as.app.arg && m7_type_has_free_tyvar(*t.as.app.arg));
        case TY_FN: {
            if (t.as.fn.arg_full_types)
                for (uint8_t i = 0; i < t.as.fn.arity; i++)
                    if (t.as.fn.arg_full_types[i] &&
                        m7_type_has_free_tyvar(*t.as.fn.arg_full_types[i]))
                        return true;
            return t.as.fn.result_full_type &&
                   m7_type_has_free_tyvar(*t.as.fn.result_full_type);
        }
        default:
            return false;
    }
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
                    /* Function-arrow instance head: `(->)` (a one-element list
                     * whose item is the `->` symbol) or a bare `->` symbol map
                     * to a dedicated function-arrow constructor of kind
                     * * -> * -> *, represented as a TY_FN marker (arity 0).
                     * This is distinct from the opaque-struct fallback below:
                     * a method parameter typed by the class variable then
                     * resolves to a callable fat closure (see the param-type
                     * substitution further down), so an `Arrow [(->)]` instance
                     * whose body composes/applies its arguments type-checks. */
                    bool is_arrow_head = false;
                    if (arg->tag == F_SYM && arg->as.sym == e->sym_arrow) {
                        is_arrow_head = true;
                    } else if (arg->tag == F_LIST && arg->as.list.len == 1 &&
                               arg->as.list.items[0]->tag == F_SYM &&
                               arg->as.list.items[0]->as.sym == e->sym_arrow) {
                        is_arrow_head = true;
                    }
                    if (is_arrow_head) {
                        memset(&type_args[i], 0, sizeof(type_args[i]));
                        type_args[i].kind = TY_FN;
                        type_args[i].copy_kind = CK_COPY;
                        type_args[i].hkt_kind = KIND_ARROW2;  /* * -> * -> * */
                        /* Stable, C-safe mangling component (the raw `->`
                         * symbol would sanitise to "__"). */
                        type_arg_syms[i] = intern_cstr(e->st, "arrow");
                        continue;
                    }
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
                                } else if (sb && sb->type.kind == TY_ADT && sb->type.as.adt_.def) {
                                    /* A defdata/defgadt type constructor used as an
                                     * instance head, e.g. (definstance Functor [Either] ...).
                                     * Preserve the ADT type so the orphan-instance check
                                     * can credit the module that defines it, and carry the
                                     * symbol for method-name mangling so codegen never
                                     * dereferences the struct_ union on a TY_ADT. */
                                    type_args[i] = sb->type;
                                    type_arg_syms[i] = kw;
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
                    } else if (arg->tag == F_LIST &&
                               (arg->as.list.len == 2 || arg->as.list.len == 3)) {
                        /* Phase HKT §3: (constructor arg) — partial type application.
                         * Parses `(result int)` in type position as TY_APP where
                         * fn = TY_STRUCT(constructor, KIND_ARROW2) and arg = concrete type.
                         * This allows `(definstance Functor [(result int)] ...)`.
                         *
                         * T4 (trailing-parameter head): the 3-element hole form
                         * `(Ctor _ B)` / `(Ctor A _)` marks the *free* parameter
                         * with exactly one `_` and fixes the other.  This lets a
                         * kind-(* -> *) class fix a *trailing* parameter -- e.g.
                         * `(Result _ B)` holds the err arm B and varies the ok arm,
                         * which leftmost-only application (`(Result A)`) cannot
                         * express.  The varying element arm is erased to the int64
                         * carrier, so downstream only needs the constructor identity
                         * (dispatch + orphan check) and a valid (* -> *) head, which
                         * is exactly what fixing the named arm produces.  See
                         * docs/reported/result-param-order-blocks-functor-monad.md. */
                        Form *ctor_form = arg->as.list.items[0];
                        Form *aarg_form;
                        if (arg->as.list.len == 2) {
                            aarg_form = arg->as.list.items[1];
                        } else {
                            const Symbol *us = intern_cstr(e->st, "_");
                            Form *h1 = arg->as.list.items[1];
                            Form *h2 = arg->as.list.items[2];
                            bool h1_hole = (h1->tag == F_SYM || h1->tag == F_KEYWORD)
                                           && h1->as.sym == us;
                            bool h2_hole = (h2->tag == F_SYM || h2->tag == F_KEYWORD)
                                           && h2->as.sym == us;
                            if (h1_hole == h2_hole) {
                                diag_emit(DIAG_ERROR, arg->span,
                                          h1_hole
                                            ? "instance head has two '_' holes; exactly "
                                              "one parameter may be left free (e.g. (Result _ B))"
                                            : "instance head must mark the free parameter "
                                              "with exactly one '_' (e.g. (Result _ B))");
                                return NULL;
                            }
                            /* The fixed (non-hole) arm is the type argument. */
                            aarg_form = h1_hole ? h2 : h1;
                        }
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
                                /* ECS E2d-P6 (Issue 2 secondary): resolve a known
                                 * struct/ADT symbol to its real def, mirroring the
                                 * top-level type-arg parser above.  Without this an
                                 * applied head like `(Dense Pos)` recorded `Pos`
                                 * as an opaque NULL-def struct, so substituting the
                                 * instance type arg into an inherited param type
                                 * (`s : S` -> `(Dense Pos)`) produced an imprecise
                                 * receiver that failed to bind the helper's `(Dense
                                 * A)` type variable.  A primitive numeric name also
                                 * resolves here. */
                                TypeKind ank = typekind_from_symbol(akw->name);
                                Binding *asb = scope_lookup(e->scope, akw);
                                if (ank != TY_UNKNOWN) {
                                    app_arg_type = type_simple(ank, CK_COPY);
                                } else if (asb && asb->type.kind == TY_STRUCT &&
                                           asb->type.as.struct_.def) {
                                    app_arg_type = asb->type;
                                } else if (asb && asb->type.kind == TY_ADT &&
                                           asb->type.as.adt_.def) {
                                    app_arg_type = asb->type;
                                } else {
                                    /* Unknown name — treat as opaque struct */
                                    memset(&app_arg_type, 0, sizeof(app_arg_type));
                                    app_arg_type.kind = TY_STRUCT;
                                    app_arg_type.copy_kind = CK_MOVE;
                                    app_arg_type.as.struct_.def = NULL;
                                }
                            }
                        } else {
                            diag_emit(DIAG_ERROR, aarg_form->span,
                                      "type application argument must be a type keyword or symbol");
                            return NULL;
                        }
                        /* Build fn type for the constructor being applied.
                         * If the constructor names a real defdata/defstruct in
                         * scope (e.g. `Either` in `(Either E)`), preserve its
                         * TY_ADT/TY_STRUCT identity -- including the def's
                         * origin_file_id -- so the orphan-instance check can
                         * credit the owning module.  Otherwise fall back to an
                         * opaque KIND_ARROW2 TY_STRUCT (e.g. `result`/`vec`). */
                        Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                        memset(fn_type, 0, sizeof(Type));
                        Binding *ctor_b = scope_lookup(e->scope, ctor_sym);
                        if (ctor_b && (ctor_b->type.kind == TY_ADT ||
                                       (ctor_b->type.kind == TY_STRUCT &&
                                        ctor_b->type.as.struct_.def))) {
                            *fn_type = ctor_b->type;
                            fn_type->hkt_kind = KIND_ARROW2;
                        } else {
                            fn_type->kind = TY_STRUCT;
                            fn_type->copy_kind = CK_MOVE;
                            fn_type->hkt_kind = KIND_ARROW2;
                            fn_type->as.struct_.def = NULL;
                        }
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
                        /* Check if current is a tyvar placeholder (potential
                         * higher-kinded constructor head when applied to a next
                         * arg) and next is a type.  Accepts the legacy anonymous
                         * TY_STRUCT{def=NULL} shape and the named TY_TYVAR
                         * introduced by Direction A step 2a. */
                        if ((type_args[i].kind == TY_STRUCT && type_args[i].as.struct_.def == NULL)
                            || type_args[i].kind == TY_TYVAR) {
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
    /* Names of the tyvars introduced by the constraint vector (e.g. `A` in
     * `[(Eq A)]`).  These must be in scope as type variables while elaborating
     * the instance method bodies so that a bare `A` in an ascription such as
     * `(:: t1 (Cons A))` resolves to the constraint tyvar rather than a
     * same-named global type.  Pushed onto e->sig_tyvars for pass 2.  See
     * docs/reported/m5-suite-residual-6-failures-2026-06-14.md (root cause A). */
    const Symbol *constraint_tyvar_syms[32];
    uint8_t n_constraint_tyvar_syms = 0;

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
                        int8_t p_idx = -1;
                        const Symbol *ct_var = NULL;
                        if (constraint_form->as.list.len >= 2) {
                            Form *type_arg_form = constraint_form->as.list.items[1];
                            if (type_arg_form->tag == F_SYM) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                if (n_constraint_tyvar_syms < 32) {
                                    bool dup = false;
                                    for (uint8_t d = 0; d < n_constraint_tyvar_syms; d++) {
                                        if (constraint_tyvar_syms[d] == type_param_name) {
                                            dup = true; break;
                                        }
                                    }
                                    if (!dup)
                                        constraint_tyvar_syms[n_constraint_tyvar_syms++] =
                                            type_param_name;
                                }
                                /* M5 gap 4: also record the constraint var on the
                                 * TypeConstraint so the emit composition pass can
                                 * resolve it to its concrete element type. */
                                ct_var = type_param_name;
                                bool found = false;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    if (type_arg_form->as.sym->len == 3 &&
                                        memcmp(type_arg_form->as.sym->name, "int", 3) == 0) {
                                        constrained_type = TYPE_INT; found = true;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "bool", 4) == 0) {
                                        constrained_type = TYPE_BOOL; found = true;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "cstr", 4) == 0) {
                                        constrained_type = TYPE_CSTR; found = true;
                                    }
                                }
                                /* PTC4: unresolved symbol may be a struct type param */
                                if (!found) {
                                    for (uint8_t j = 0; j < n_type_args && p_idx < 0; j++) {
                                        if (type_args[j].kind == TY_STRUCT &&
                                            type_args[j].as.struct_.def) {
                                            StructDef *sdef = type_args[j].as.struct_.def;
                                            for (uint8_t k = 0; k < sdef->n_type_params; k++) {
                                                if (strcmp(sdef->type_params[k],
                                                           type_param_name->name) == 0) {
                                                    p_idx = (int8_t)k;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type,
                            .param_idx = p_idx,
                            .tyvar = ct_var
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
                        int8_t p_idx = -1;
                        const Symbol *ct_var = NULL;
                        if (idx + 1 < n_items) {
                            Form *type_arg_form = next_form->as.list.items[idx + 1];
                            if (type_arg_form->tag == F_SYM) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                ct_var = type_param_name;
                                bool found = false;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    if (type_arg_form->as.sym->len == 3 &&
                                        memcmp(type_arg_form->as.sym->name, "int", 3) == 0) {
                                        constrained_type = TYPE_INT; found = true;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "bool", 4) == 0) {
                                        constrained_type = TYPE_BOOL; found = true;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "cstr", 4) == 0) {
                                        constrained_type = TYPE_CSTR; found = true;
                                    }
                                }
                                /* PTC4: unresolved symbol may be a struct type param */
                                if (!found) {
                                    for (uint8_t j = 0; j < n_type_args && p_idx < 0; j++) {
                                        if (type_args[j].kind == TY_STRUCT &&
                                            type_args[j].as.struct_.def) {
                                            StructDef *sdef = type_args[j].as.struct_.def;
                                            for (uint8_t k = 0; k < sdef->n_type_params; k++) {
                                                if (strcmp(sdef->type_params[k],
                                                           type_param_name->name) == 0) {
                                                    p_idx = (int8_t)k;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type,
                            .param_idx = p_idx,
                            .tyvar = ct_var
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
            /* PTC4/PTC6: constraints with param_idx >= 0 refer to struct type params
             * whose concrete types are not known at definstance time; skip PTC2. */
            if (type_param_constraints[i].param_idx >= 0) continue;
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

    /* Compute the instance's codegen type-arg suffix once -- it depends only on
     * the type args, not on any individual method, and is reused both for the
     * duplicate-instance guard just below and for every method name built later. */
    char inst_type_suffix[64];
    if (!build_inst_type_suffix(type_args, type_arg_syms, n_type_args,
                                inst_type_suffix, sizeof(inst_type_suffix))) {
        diag_emit(DIAG_ERROR, call->span,
                  "typeclass instance name is too long for '%s'", tc_name->name);
        return NULL;
    }

    /* Idempotent re-instance guard.  An instance whose (typeclass, type-arg
     * suffix) matches one already registered would re-emit the same dictionary
     * struct/singleton and __inst_* method functions, producing a hard C
     * "redefinition" ODR error.  This fires whenever the same instance is seen
     * twice -- e.g. a module that explicitly (load "stdlib/typeclass.tur")s
     * while an auto-loaded partial typeclass stub (typeclass-clone.tur, ...)
     * already supplied the same primitive instance.  The first definition wins;
     * the redundant one is a silent no-op, matching the include-guard mental
     * model for repeated loads.  See docs/reported/load-not-idempotent-typeclass.md. */
    for (TypeClassInstance *prev = e->typeclass_env.instances; prev; prev = prev->next) {
        if (prev->typeclass != tc || prev->n_type_args != n_type_args) continue;
        char prev_suffix[64];
        if (!build_inst_type_suffix(prev->type_args, prev->type_arg_syms,
                                    prev->n_type_args, prev_suffix, sizeof(prev_suffix)))
            continue;
        if (strcmp(prev_suffix, inst_type_suffix) == 0) {
            /* Already have this exact instance; emit nothing further. */
            return e_nil(e, call->span);
        }
    }

    /* assoc-types-plan: an instance body interleaves method implementations
     * with associated-type bindings `(type Name = <type-expr>)`.  Classify the
     * forms after impls_start: a `type` head whose second element is a bare
     * symbol is an associated-type binding; everything else is a method impl.
     * Bindings are resolved after the instance is registered (so the type
     * expression may reference the instance's own type args). */
    uint32_t n_inst_body = call->as.list.len - impls_start;
    Form **method_impl_forms = n_inst_body > 0
        ? (Form **)arena_alloc(e->arena, n_inst_body * sizeof(Form *)) : NULL;
    uint32_t n_method_impl_forms = 0;
    const Symbol **assoc_bind_names = n_inst_body > 0
        ? (const Symbol **)arena_alloc(e->arena, n_inst_body * sizeof(const Symbol *))
        : NULL;
    Form **assoc_bind_forms = n_inst_body > 0
        ? (Form **)arena_alloc(e->arena, n_inst_body * sizeof(Form *)) : NULL;
    uint32_t n_assoc_binds = 0;
    {
        const Symbol *sym_type_kw = intern_cstr(e->st, "type");
        for (uint32_t bi = impls_start; bi < call->as.list.len; bi++) {
            Form *bf = call->as.list.items[bi];
            if (bf->tag == F_LIST && bf->as.list.len >= 2 &&
                bf->as.list.items[0]->tag == F_SYM &&
                bf->as.list.items[0]->as.sym == sym_type_kw &&
                bf->as.list.items[1]->tag == F_SYM) {
                /* (type Name = <type-expr>)  -- an optional `=` token may sit
                 * between the name and the type expression. */
                const Symbol *sym_eq = intern_cstr(e->st, "=");
                uint32_t te_idx = 2;
                if (bf->as.list.len > te_idx &&
                    bf->as.list.items[te_idx]->tag == F_SYM &&
                    bf->as.list.items[te_idx]->as.sym == sym_eq) {
                    te_idx++;
                }
                if (bf->as.list.len <= te_idx) {
                    diag_emit(DIAG_ERROR, bf->span,
                              "associated type binding requires a type: "
                              "(type %s = <type>)", bf->as.list.items[1]->as.sym->name);
                    return NULL;
                }
                assoc_bind_names[n_assoc_binds] = bf->as.list.items[1]->as.sym;
                assoc_bind_forms[n_assoc_binds] = bf->as.list.items[te_idx];
                n_assoc_binds++;
                continue;
            }
            method_impl_forms[n_method_impl_forms++] = bf;
        }
    }

    /* Parse method implementations */
    /* Each method impl is a function definition without the 'defn' keyword */
    /* Syntax: (method-name [param1 param2 ...] body...)
     * The number of methods must match the typeclass definition.
     */

    if (n_method_impl_forms < tc->n_methods) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d method implementations for '%s', got %d",
                  tc->n_methods, tc_name->name, n_method_impl_forms);
        return NULL;
    }
    
    /* For Phase 15 v1, we store method implementations as FnDef pointers.
     * In a full implementation, these would be stored in the instance and
     * codegen would generate dictionary structs. For now, we validate syntax.
     */
    FnDef **method_impls = NULL;
    if (tc->n_methods > 0) {
        method_impls = (FnDef **)arena_alloc(e->arena, tc->n_methods * sizeof(FnDef *));
        for (uint8_t i = 0; i < tc->n_methods; i++) method_impls[i] = NULL;
    }

    /* Intra-instance method dispatch: register the instance and wire up its
     * type args / method-impl slots BEFORE elaborating any method body, so that
     * a `(.sibling self ...)` call inside one method's body can resolve to
     * another method of the *same* instance.
     *
     * Elaboration is split into two passes so that *forward* references and
     * mutual recursion between siblings resolve too:
     *   Pass 1 parses each method's signature (name / params / return type) and
     *     creates its FnDef + binding shell, filling `method_impls[i]` and
     *     registering the file-scope def -- but leaves the body as a nil
     *     placeholder.  After pass 1 every sibling slot is non-NULL.
     *   Pass 2 elaborates each method body against the now-complete instance,
     *     so a call to a sibling defined *later* in the same `definstance`
     *     (or a mutually recursive pair) sees a populated slot.
     * The instance itself is registered on the env list up front so the
     * type-based `.method` dispatcher selects it.  Post-registration
     * bookkeeping (orphan check, INSTANCE_DEF expr) still happens after the
     * loops. */
    typedef struct {
        Form     *impl_form;        /* (method-name [params...] body...) */
        uint32_t  impl_body_start;  /* index of first body form within impl_form */
        Binding **method_params;    /* param bindings to push into the body scope */
        uint8_t   n_method_params;
        FnDef    *method_fd;        /* shell whose ->body pass 2 fills in */
        bool      arrow_return;     /* class return type is the (->) class var:
                                     * refine the method's result type from the
                                     * elaborated body (a callable closure) in
                                     * pass 2. */
    } InstMethodPass;
    InstMethodPass *passes = NULL;
    if (tc->n_methods > 0) {
        passes = (InstMethodPass *)arena_alloc(e->arena,
            tc->n_methods * sizeof(InstMethodPass));
        memset(passes, 0, tc->n_methods * sizeof(InstMethodPass));
    }

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

    /* assoc-types-plan: resolve and store associated-type bindings.  Every
     * member the class declares must be bound exactly once; an unknown member
     * name or a missing binding is a hard error reported on the instance, so a
     * wrong/forgotten projection surfaces here rather than at a downstream use
     * site.
     *
     * ECS E2d-P6 (Issue 1): resolved up front -- before the method loop --
     * so an instance method body whose param/return type names an associated
     * member (e.g. `val : Elem` -> TY_TYVAR("Elem")) can substitute it with the
     * concrete binding (`(type Elem = Pos)` -> Pos). */
    if (tc->n_assoc_types > 0 || n_assoc_binds > 0) {
        Type *resolved = tc->n_assoc_types > 0
            ? (Type *)arena_alloc(e->arena, tc->n_assoc_types * sizeof(Type)) : NULL;
        bool *bound = tc->n_assoc_types > 0
            ? (bool *)arena_alloc(e->arena, tc->n_assoc_types * sizeof(bool)) : NULL;
        for (uint8_t k = 0; k < tc->n_assoc_types; k++) bound[k] = false;
        for (uint32_t bi = 0; bi < n_assoc_binds; bi++) {
            uint8_t idx = 0; bool found = false;
            for (uint8_t k = 0; k < tc->n_assoc_types; k++) {
                if (tc->assoc_type_names[k] == assoc_bind_names[bi]) {
                    idx = k; found = true; break;
                }
            }
            if (!found) {
                diag_emit(DIAG_ERROR, call->span,
                          "definstance: '%s' is not an associated type of typeclass '%s'",
                          assoc_bind_names[bi]->name, tc_name->name);
                return NULL;
            }
            Type *rt = type_expr_from_form(e, assoc_bind_forms[bi], NULL, NULL, NULL, 0);
            if (!rt) {
                diag_emit(DIAG_ERROR, assoc_bind_forms[bi]->span,
                          "definstance: unsupported type for associated type '%s'",
                          assoc_bind_names[bi]->name);
                return NULL;
            }
            resolved[idx] = *rt;
            bound[idx] = true;
        }
        for (uint8_t k = 0; k < tc->n_assoc_types; k++) {
            if (!bound[k]) {
                diag_emit(DIAG_ERROR, call->span,
                          "definstance: missing binding for associated type '%s' of '%s'",
                          tc->assoc_type_names[k]->name, tc_name->name);
                return NULL;
            }
        }
        inst->assoc_types = resolved;
        inst->n_assoc_types = tc->n_assoc_types;
    }

    for (uint8_t i = 0; i < tc->n_methods; i++) {
        Form *impl_form = method_impl_forms[i];
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

        /* Create a synthetic name for this method implementation */
        /* Format: __inst_<typeclass>_<method>_<typeargs> e.g. __inst_MyEq_eq_int */
        enum {
            MAX_INSTANCE_METHOD_NAME_LEN = 192,
            MAX_SANITIZED_METHOD_NAME_LEN = 64,
            MAX_INSTANCE_TYPE_SUFFIX_LEN = 64,
        };
        char method_name[MAX_INSTANCE_METHOD_NAME_LEN];
        
        /* Mangle method name into a C identifier via the shared mangler so
         * sigil method pairs (`>>>`/`<<<`) get distinct instance-function
         * names instead of colliding on `___`. */
        char sanitized_method_name[MAX_SANITIZED_METHOD_NAME_LEN];
        const char *method_name_str = tc->methods[i].name->name;
        tur_mangle_ident(method_name_str, sanitized_method_name,
                         sizeof(sanitized_method_name));

        /* Type arg suffix was computed once up front (inst_type_suffix) -- it is
         * identical for every method of this instance and shared with the
         * duplicate-instance guard, so the emitted names stay in lock-step. */
        int method_name_written =
            snprintf(method_name, sizeof(method_name), "__inst_%.*s_%s%s",
                     (int)tc_name->len, tc_name->name, sanitized_method_name,
                     inst_type_suffix);
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

        /* Phase RT: substitute a tyvar return type (the dispatch variable) with
         * the instance's concrete type argument, so the emitted impl returns
         * the instance type (e.g. (decode! [raw :int] : a) becomes : User for
         * HasSchema[User]).  An explicit annotation below still wins. */
        if (return_type.kind == TY_TYVAR && return_type.as.tyvar_.name) {
            bool subst = false;
            for (uint8_t ti = 0; ti < tc->n_type_params && ti < n_type_args; ti++) {
                if (tc->type_params[ti] &&
                    strcmp(tc->type_params[ti]->name,
                           return_type.as.tyvar_.name) == 0) {
                    return_type = type_args[ti];
                    subst = true;
                    break;
                }
            }
            /* ECS E2d-P6 (Issue 1): a return type naming an associated type
             * member (`: Elem`) substitutes with this instance's binding
             * (`(type Elem = Pos)` -> Pos), so the emitted impl returns the
             * concrete projected type. */
            for (uint8_t ak = 0; !subst && ak < inst->n_assoc_types; ak++) {
                if (tc->assoc_type_names[ak] &&
                    strcmp(tc->assoc_type_names[ak]->name,
                           return_type.as.tyvar_.name) == 0) {
                    return_type = inst->assoc_types[ak];
                    break;
                }
            }
        }
        /* M7 layer 0 (flag-gated): an HKT-applied return like `(g b)` arrives as
         * TY_APP(TY_TYVAR g, TY_TYVAR b).  Substitute the HKT class param in the
         * application HEAD (`g`) with this instance's constructor (type_args[ti],
         * e.g. Option), leaving the element tyvar (`b`) abstract for per-call
         * refinement.  Result: `(Option b)`.  Only the outermost head is
         * rewritten (the common `(f a)` / `(f b)` shape). */
        else if (g_m7_hkt_enabled && return_type.kind == TY_APP &&
                 return_type.as.app.fn &&
                 return_type.as.app.fn->kind == TY_TYVAR &&
                 return_type.as.app.fn->as.tyvar_.name) {
            const char *head = return_type.as.app.fn->as.tyvar_.name;
            for (uint8_t ti = 0; ti < tc->n_type_params && ti < n_type_args; ti++) {
                if (tc->type_params[ti] &&
                    strcmp(tc->type_params[ti]->name, head) == 0) {
                    Type *new_fn = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *new_fn = type_args[ti];
                    return_type.as.app.fn = new_fn;
                    break;
                }
            }
        }
        /* Arrow head: a method whose declared return is the class variable
         * (e.g. `comp : a` under `Arrow [(->)]`) returns a callable closure.
         * The arrow marker carries no arity yet, so flag it as a boxed TY_FN
         * placeholder here and refine its full signature from the elaborated
         * body in pass 2 (the body's `(fn [x] ...)` carries the real arity). */
        bool arrow_return = false;
        if (return_type.kind == TY_FN && return_type.as.fn.arity == 0) {
            arrow_return = true;
            return_type.as.fn.boxed = true;
        }
        /* Check for return type annotation after params */
        if (impl_form->as.list.len >= 3) {
            Form *ret_or_body = impl_form->as.list.items[2];
            /* Accept both fused `:T` (F_KEYWORD) and spaced `: T` (F_TYPE_ANN). */
            const Symbol *kw = NULL;
            if (ret_or_body->tag == F_KEYWORD) {
                kw = ret_or_body->as.sym;
            } else if (ret_or_body->tag == F_TYPE_ANN &&
                       ret_or_body->as.list.len == 1 &&
                       (ret_or_body->as.list.items[0]->tag == F_SYM ||
                        ret_or_body->as.list.items[0]->tag == F_KEYWORD)) {
                kw = ret_or_body->as.list.items[0]->as.sym;
            }
            if (kw) {
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    return_type = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    return_type = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    return_type = TYPE_CSTR;
                } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                    /* A :float instance return must carry the real register class
                     * (xmm0/double) all the way through the emitted impl signature
                     * and the monomorphic dispatch -- otherwise the result is
                     * numerically truncated at the int64-carrier return boundary
                     * (e.g. 6.5 -> 6).  See the typeclass-method return-type
                     * erasure follow-up. */
                    return_type = TYPE_FLOAT;
                } else if (kw->len == 7 && memcmp(kw->name, "float32", 7) == 0) {
                    return_type = TYPE_FLOAT32;
                } else if (kw->len == 7 && memcmp(kw->name, "float64", 7) == 0) {
                    return_type = TYPE_FLOAT64;
                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                    return_type = TYPE_NIL;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    return_type = TYPE_PTR_VOID;
                } else {
                    /* Fall back to a full type expression so compound instance
                     * return annotations (e.g. `: (Vec int)`, a struct/ADT name)
                     * resolve instead of being silently dropped to the class
                     * carrier.  The keyword shortcuts above stay for the common
                     * primitives; type_expr_from_form covers the rest.  If it
                     * cannot resolve, leave return_type at the class default
                     * (back-compat with annotations the resolver does not know). */
                    Form *tf = (ret_or_body->tag == F_TYPE_ANN &&
                                ret_or_body->as.list.len >= 1)
                        ? ret_or_body->as.list.items[0] : ret_or_body;
                    Type *ft = type_expr_from_form(e, tf, NULL, NULL, NULL, 0);
                    if (ft) return_type = *ft;
                }
                impl_body_start = 3;
            }
        }
        
        /* Parse parameters */
        Binding **method_params = NULL;
        uint8_t n_method_params = 0;
        Type *method_param_types = NULL;
        
        if (impl_params_form->tag == F_VEC) {
            uint8_t max_method_params = (uint8_t)impl_params_form->as.list.len;
            if (max_method_params > 0) {
                method_params = (Binding **)arena_alloc(e->arena, 
                    max_method_params * sizeof(Binding *));
                method_param_types = (Type *)arena_alloc(e->arena, 
                    max_method_params * sizeof(Type));
                
                for (uint8_t j = 0; j < max_method_params; j++) {
                    Form *p = impl_params_form->as.list.items[j];
                    if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
                        if (n_method_params == 0) {
                            diag_emit(DIAG_ERROR, p->span,
                                      "method parameter type annotation without a preceding parameter");
                            return NULL;
                        }
                        uint8_t prev = n_method_params - 1;
                        Type param_type = method_param_types[prev];
                        if (p->tag == F_TYPE_ANN) {
                            Type *ann = (p->as.list.len > 0)
                                ? type_expr_from_form(e, p->as.list.items[0], NULL, NULL, NULL, 0)
                                : NULL;
                            if (!ann) {
                                diag_emit(DIAG_ERROR, p->span,
                                          "unsupported type form in method parameter");
                                return NULL;
                            }
                            param_type = *ann;
                        } else {
                            const Symbol *kw = p->as.sym;
                            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                param_type = TYPE_INT;
                            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                param_type = TYPE_BOOL;
                            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                param_type = TYPE_CSTR;
                            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                                param_type = TYPE_NIL;
                            } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                                       (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                                param_type = TYPE_PTR_VOID;
                            } else {
                                diag_emit(DIAG_ERROR, p->span,
                                          "unsupported type in method parameter");
                                return NULL;
                            }
                        }
                        bool param_is_poly = (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                        if (!param_is_poly
                            && tc->methods[i].param_is_fn
                            && prev < tc->methods[i].n_params
                            && tc->methods[i].param_is_fn[prev]) {
                            param_is_poly = true;
                            param_type = TYPE_PTR_VOID;
                        }
                        Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                        method_params[prev]->type = c_param_type;
                        method_params[prev]->is_poly_fn = param_is_poly;
                        method_params[prev]->poly_type = NULL;
                        if (param_is_poly) {
                            Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *pt = param_type;
                            method_params[prev]->poly_type = pt;
                        }
                        method_param_types[prev] = c_param_type;
                        continue;
                    }

                    Type param_type = TYPE_INT;
                    /* Arrow head: a class-variable-typed parameter becomes a
                     * callable fat closure (TY_PTR_VOID + is_fat), so a method
                     * body that applies it -- `(g (f x))` -- routes through the
                     * fat-dispatch path instead of erroring "not a function". */
                    bool param_is_fat = false;

                    /* Phase 15: Try to use type from typeclass method definition */
                    if (tc->methods[i].param_types && n_method_params < tc->methods[i].n_params) {
                        param_type = tc->methods[i].param_types[n_method_params];
                    }

                    /* ECS E2d-P6 (Issue 2 secondary): when the class method param
                     * type references the class's type parameters (e.g. `val : E`
                     * -> TY_TYVAR("E"), or `s : S` -> TY_TYVAR("S")), substitute
                     * them with this instance's concrete type args so an
                     * unannotated instance-body param (`[s idx val]`) inherits the
                     * right type instead of an abstract tyvar.  This is the
                     * general, multi-param-aware replacement for the legacy
                     * receiver-only `type_args[0]` rewrite below (which keyed on
                     * the param defaulting to TY_INT). */
                    bool param_was_tyvar_subst = false;
                    if (param_type.kind == TY_TYVAR || param_type.kind == TY_APP) {
                        for (uint8_t ck = 0; n_type_args > 0 && ck < tc->n_type_params; ck++) {
                            if (tc->type_params[ck] &&
                                rt_type_mentions_tyvar(&param_type,
                                                       tc->type_params[ck]->name)) {
                                param_was_tyvar_subst = true;
                                break;
                            }
                        }
                        /* ECS E2d-P6 (Issue 1): also substitute associated-type
                         * tyvars (`val : Elem`) with this instance's binding
                         * (`(type Elem = Pos)` -> Pos). */
                        for (uint8_t ak = 0; !param_was_tyvar_subst &&
                                 ak < inst->n_assoc_types; ak++) {
                            if (tc->assoc_type_names[ak] &&
                                rt_type_mentions_tyvar(&param_type,
                                                       tc->assoc_type_names[ak]->name)) {
                                param_was_tyvar_subst = true;
                                break;
                            }
                        }
                        if (param_was_tyvar_subst) {
                            if (n_type_args > 0) {
                                param_type = elab_subst_class_tyvars(
                                    e->arena, param_type,
                                    tc->type_params, tc->n_type_params,
                                    type_args, n_type_args);
                            }
                            if (inst->n_assoc_types > 0) {
                                param_type = elab_subst_class_tyvars(
                                    e->arena, param_type,
                                    tc->assoc_type_names, tc->n_assoc_types,
                                    inst->assoc_types, inst->n_assoc_types);
                            }
                        }
                    }

                    /* Phase 15: Substitute type variables with type args */
                    /* For v1: if the param type is TYPE_INT (default) and we have type args,
                     * use the first type arg */
                    /* CS1b: elab_param_type is the type used for scope bindings and body
                     * elaboration; param_type is the ABI type used for the emitted signature. */
                    Type elab_param_type = param_type;
                    /* Arrow head: under `Arrow [(->)]`, a method parameter typed
                     * by the class variable is the function arrow itself -- a
                     * callable closure.  Carry it as a fat-closure sink
                     * (:ptr<void> + is_fat), the same representation the
                     * bare-function arrow layer uses for its `^fat` parameters,
                     * so applying it (`(g (f x))`) dispatches through the fat
                     * protocol.  This applies even to a return-dispatch method
                     * (e.g. `comp [f g] : a`, whose untyped params default to the
                     * carrier): the arrow instance head makes the params arrows. */
                    if (param_was_tyvar_subst) {
                        /* M7 fix direction 1 (flag-gated): box any fn that sits
                         * in HKT-element position of the body param type, so
                         * calling an HKT-wrapped function (`((.value ff) x)` in
                         * the Applicative `ap` shape) fat-dispatches through the
                         * box instead of bare-calling the box address. */
                        if (g_m7_hkt_enabled)
                            elab_param_type = m7_box_hkt_element_fns(e->arena,
                                                                     elab_param_type);
                        /* The substituted full type (elab_param_type) is what the
                         * method body sees; lower the ABI/signature type to the
                         * int64 carrier for applied/parametric types, matching the
                         * dispatch ABI used for concrete instances elsewhere. */
                        if (elab_param_type.kind == TY_APP ||
                            (elab_param_type.kind == TY_STRUCT &&
                             elab_param_type.as.struct_.def &&
                             elab_param_type.as.struct_.def->n_type_params > 0)) {
                            param_type = TYPE_INT;
                        } else {
                            param_type = elab_param_type;
                        }
                    }
                    else if (param_type.kind == TY_INT && n_type_args > 0 &&
                        type_args[0].kind == TY_FN) {
                        elab_param_type = TYPE_PTR_VOID;
                        param_type = TYPE_PTR_VOID;
                        param_is_fat = true;
                    }
                    /* Phase RT: for a return-only-dispatch method, parameters
                     * are genuine (concrete) inputs, not the dispatch receiver,
                     * so do not rewrite an int parameter to the instance type.
                     *
                     * Prereq 4: the `param_type.kind == TY_INT` test is broad --
                     * untyped params default to TY_INT, but an explicit `:int`
                     * annotation lands on the same kind. Skip the rewrite when
                     * the user explicitly annotated the param (tracked via
                     * `param_explicit_type[]`, populated by parse_typeclass_method).
                     * Without this, a class like
                     * `(defclass Decode [a] (decode [v : int] : (Result a cstr)))`
                     * would emit `__inst_Decode_decode_cstr(const char *)` for
                     * the cstr instance -- silently substituting cstr in where
                     * the user pinned int, and segfaulting at runtime. */
                    else if (param_type.kind == TY_INT && n_type_args > 0 &&
                        !method_is_return_dispatch(tc, &tc->methods[i]) &&
                        !(tc->methods[i].param_explicit_type &&
                          n_method_params < tc->methods[i].n_params &&
                          tc->methods[i].param_explicit_type[n_method_params])) {
                        elab_param_type = type_args[0];
                        /* PTC4: KIND_ARROW struct type-constructors (have type params) are
                         * applied as TY_APP at call sites, which lowers to int64_t in C.
                         * Use int64_t so the method signature matches the dispatch ABI.
                         * CS1b: preserve the full struct type in elab_param_type so that
                         * field-access forms inside the method body can resolve correctly.
                         * T4: a partially-applied instance head (e.g. `(Result _ B)` /
                         * `(Either E)`) records the receiver type as a TY_APP, which also
                         * lowers to the int64_t carrier.  Force the carrier here too --
                         * otherwise a concrete by-value struct receiver (e.g. an ascribed
                         * `(Result int int)`) is marshalled by-address into the int64_t
                         * impl signature and emits invalid C. */
                        if (elab_param_type.kind == TY_STRUCT && elab_param_type.as.struct_.def &&
                            elab_param_type.as.struct_.def->n_type_params > 0) {
                            param_type = TYPE_INT;
                        } else if (elab_param_type.kind == TY_APP) {
                            param_type = TYPE_INT;
                        } else {
                            param_type = elab_param_type;
                        }
                    }

                    /* Phase HRT3: if the param type is TY_FORALL, treat it as a poly fn param.
                     * Phase CCL: also treat :fn-annotated params (param_is_fn) as poly fn. */
                    bool param_is_poly = (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                    if (!param_is_poly
                        && tc->methods[i].param_is_fn
                        && n_method_params < tc->methods[i].n_params
                        && tc->methods[i].param_is_fn[n_method_params]) {
                        param_is_poly = true;
                        param_type = TYPE_PTR_VOID;
                        elab_param_type = TYPE_PTR_VOID;
                    }
                    Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                    if (p->tag == F_SYM) {
                        /* Simple parameter name */
                        method_params[n_method_params] = binding_new(e, p->as.sym, elab_param_type, false, false, p->span);
                        if (param_is_poly) {
                            method_params[n_method_params]->is_poly_fn = true;
                            Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *pt = param_type;
                            method_params[n_method_params]->poly_type = pt;
                        }
                        if (param_is_fat) method_params[n_method_params]->is_fat = true;
                        method_param_types[n_method_params++] = c_param_type;
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
                            } else if (p->as.list.len >= 2 && p->as.list.items[1]->tag == F_TYPE_ANN) {
                                Type *ann = (p->as.list.items[1]->as.list.len > 0)
                                    ? type_expr_from_form(e, p->as.list.items[1]->as.list.items[0], NULL, NULL, NULL, 0)
                                    : NULL;
                                if (!ann) {
                                    diag_emit(DIAG_ERROR, p->span,
                                              "unsupported type form in method parameter");
                                    return NULL;
                                }
                                param_type = *ann;
                            } else if (p->as.list.len >= 2
                                       && (p->as.list.items[1]->tag == F_LIST || p->as.list.items[1]->tag == F_VEC)) {
                                Type *ann = type_expr_from_form(e, p->as.list.items[1], NULL, NULL, NULL, 0);
                                if (!ann) {
                                    diag_emit(DIAG_ERROR, p->span,
                                              "unsupported type form in method parameter");
                                    return NULL;
                                }
                                param_type = *ann;
                            }
                            /* Phase CCL: :fn-annotated params also become poly fn */
                            if (!param_is_poly
                                && tc->methods[i].param_is_fn
                                && n_method_params < tc->methods[i].n_params
                                && tc->methods[i].param_is_fn[n_method_params]) {
                                param_is_poly = true;
                                param_type = TYPE_PTR_VOID;
                            }
                            param_is_poly = param_is_poly
                                || (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                            c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                            method_params[n_method_params] = binding_new(e, name_f->as.sym, c_param_type, false, false, p->span);
                            if (param_is_poly) {
                                method_params[n_method_params]->is_poly_fn = true;
                                Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *pt = param_type;
                                method_params[n_method_params]->poly_type = pt;
                            }
                            method_param_types[n_method_params++] = c_param_type;
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
        
        /* Pass 2 (below) elaborates the body once every sibling slot is wired
         * up.  For now the FnDef carries a nil placeholder body. */
        Expr *method_body = e_nil(e, impl_form->span);

        /* Create a proper function type for the method */
        TypeKind param_kinds[MAX_FN_ARITY];
        for (uint8_t j = 0; j < n_method_params; j++) {
            param_kinds[j] = method_param_types[j].kind;
        }
        Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);
        /* Arrow head: mirror each fat-closure parameter into the method's fn
         * type so call sites auto-shim a bare function argument into a fat box
         * (the same arg_fat plumbing the regular defn path uses for `^fat`). */
        for (uint8_t j = 0; j < n_method_params; j++) {
            if (method_params[j]->is_fat) fn_type.as.fn.arg_fat[j] = true;
        }
        /* Closure-returning instance methods: a method whose declared return
         * type is a function type (e.g. (arr-of [f] : (fn [:int] :int))) must
         * carry the full TY_FN through result_full_type, exactly like the
         * regular defn path (elab_fns.c "Issue 1b").  Without it, codegen falls
         * back to emit_type_from_kind(TY_FN) -- a zeroed fn shell whose result
         * kind is TY_UNKNOWN -- and the dict-field / impl-signature return type
         * lowers to an unknown-void carrier, silently dropping the returned fat
         * closure handle.  Attaching the full type makes type_c_name lower the
         * fn carrier to int64_t (the fat-closure handle the rest of the
         * language and TUR_APPLY* expect). */
        if (return_type.kind == TY_FN) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* ECS E2d-P6 (value-level projection): a method returning a *by-value*
         * struct or concrete ADT -- e.g. an instance whose return type
         * substituted to the by-value `Pos` (a class-var `: E`, an associated
         * member `: Elem`, or a direct `: Pos`) -- must carry the precise
         * return Type so the emitted impl, the dict slot, and the call site
         * agree on the by-value layout.  `type_from_kind(result_kind)` alone
         * drops the struct def, leaving `(.field (method ...))` unable to
         * resolve and a let-bound result mis-lowered against the carrier.
         *
         * Scope: genuinely by-value nominal types only.  Parametric structs and
         * applied/opaque heads (TY_APP, e.g. the receiver `(Dense Pos)`) keep
         * the documented int64 carrier ABI -- they are erased everywhere else,
         * and forcing them by-value here would diverge from the dispatch ABI. */
        else if ((return_type.kind == TY_STRUCT && return_type.as.struct_.def &&
                  return_type.as.struct_.def->n_type_params == 0) ||
                 (return_type.kind == TY_ADT && return_type.as.adt_.def &&
                  return_type.as.adt_.def->n_type_params == 0)) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* M7 layer 2 (flag-gated): carry an HKT-applied TY_APP return (`(Option b)`
         * after layer-0 head substitution) through result_full_type so the call
         * site receives the named applied head + element tyvar to refine, instead
         * of an anonymous `(type-app ? ?)`. */
        else if (g_m7_hkt_enabled && return_type.kind == TY_APP) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }

        /* Create FnDef for the method implementation */
        FnDef *method_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
        memset(method_fd, 0, sizeof(FnDef));
        Binding *method_binding = binding_new(e, method_sym, fn_type, false, true, impl_form->span);
        /* method_sym->name is ALREADY a mangled C identifier (built as
         * "__inst_<class>_<method>_<typesuffix>" above, with the method part run
         * through tur_mangle_ident). It must be emitted verbatim: re-mangling it
         * would double-encode every '_' as "_un" and desync the definition from
         * the use site (the dict initializer rebuilds the same name fresh). Pin
         * it via c_export_name, the documented "emit this C name as-is" bypass. */
        method_binding->c_export_name = method_sym->name;
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
        /* LS2/LS3: instance methods carry no surface borrow lifetimes; give the
         * lifetime pass a clean context + return Type rather than garbage. */
        lifetime_context_init(&method_fd->lifetime_ctx);
        method_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);

        /* method_impls[i] must be populated now so a sibling `.method self`
         * dispatch (including forward / mutually-recursive references) resolves
         * during pass 2.  The file-scope registration and global binding are
         * deferred to pass 2, AFTER the body is elaborated, so they happen
         * after the body's lifted lambdas are registered -- preserving the
         * original `[body lambdas...][method def]` emit order.  Registering the
         * method def first would emit the method ahead of its closure's lifted
         * thunk; the closure's file-scope env struct (written while emitting the
         * method body) would then land textually inside the method, out of
         * scope for the later thunk -- an "undefined struct __env_N" miscompile. */
        method_impls[i] = method_fd;
        /* M4a: backlink the method FnDef to its owning instance so emit_module
         * can identify instance methods in O(1) and (when the class is non-HKT)
         * route them through the per-instantiation emit path.  See
         * docs/upcoming/m4-typeclass-per-method-abi-plan.md. */
        method_fd->owner_instance = inst;

        /* Stash what pass 2 needs to elaborate this method's body. */
        passes[i].impl_form       = impl_form;
        passes[i].impl_body_start = impl_body_start;
        passes[i].method_params   = method_params;
        passes[i].n_method_params = n_method_params;
        passes[i].method_fd       = method_fd;
        passes[i].arrow_return    = arrow_return;
    }

    /* Bring the constraint tyvars (e.g. `A` from `[(Eq A)]`) into the
     * signature-tyvar scope for the duration of pass 2's body elaboration, so a
     * bare `A` in an ascription resolves to the tyvar over a same-named global
     * type (root cause A). Saved/restored to keep enclosing scopes intact.
     * (M5 gap 4 reached the same goal via a separate `inst_body_type_params`
     * scope; main's `sig_tyvars` route subsumes it, so only `.tyvar` recording
     * above is kept for the emit-side composition pass.) */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    for (uint8_t ti = 0; ti < n_constraint_tyvar_syms; ti++) {
        if (e->n_sig_tyvars >= 32) break;
        const char *nm = constraint_tyvar_syms[ti]
            ? constraint_tyvar_syms[ti]->name : NULL;
        if (!nm) continue;
        bool dup = false;
        for (uint8_t s = 0; s < e->n_sig_tyvars; s++) {
            if (e->sig_tyvars[s] && strcmp(e->sig_tyvars[s], nm) == 0) {
                dup = true; break;
            }
        }
        if (!dup) e->sig_tyvars[e->n_sig_tyvars++] = nm;
    }

    /* M5 (multi-param struct instance): also bring the instance head's full
     * type-ctor param list (e.g. K and V of `MutableMap [K V]`) into sig-tyvar
     * scope, not only the constraint-named ones.  Without this, an
     * *unconstrained* param (`K`, with no `(Eq K)`) used in a method-body
     * ascription `(MutableMap K V)` falls back to an opaque TY_STRUCT instead
     * of an abstract tyvar, so the call's abi_bindings record it as a concrete
     * struct and a by-value helper called from the instance body never gets a
     * by-value spec interned.  See
     * docs/reported/m5-multiparam-instance-unconstrained-tyvar-blocks-byval-spec.md. */
    for (uint8_t ta = 0; ta < n_type_args; ta++) {
        if (type_args[ta].kind != TY_STRUCT || !type_args[ta].as.struct_.def)
            continue;
        StructDef *hsd = type_args[ta].as.struct_.def;
        for (uint8_t k = 0; k < hsd->n_type_params && e->n_sig_tyvars < 32; k++) {
            const char *nm = hsd->type_params[k];
            if (!nm) continue;
            bool dup = false;
            for (uint8_t s = 0; s < e->n_sig_tyvars; s++) {
                if (e->sig_tyvars[s] && strcmp(e->sig_tyvars[s], nm) == 0) {
                    dup = true; break;
                }
            }
            if (!dup) e->sig_tyvars[e->n_sig_tyvars++] = nm;
        }
    }

    /* Pass 2: every sibling's FnDef shell and `method_impls` slot is now
     * populated, so elaborate each method body.  A call to a sibling defined
     * later in this same `definstance` -- or a mutually recursive pair --
     * resolves because the dispatcher finds a non-NULL slot for it. */
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        InstMethodPass *mp = &passes[i];
        Form *impl_form = mp->impl_form;
        uint32_t impl_body_start = mp->impl_body_start;

        /* Elaborate the body - push a scope with method parameters */
        Scope method_scope;
        scope_init(&method_scope, e->scope);
        e->scope = &method_scope;

        /* Add method parameters to scope */
        for (uint8_t j = 0; j < mp->n_method_params; j++) {
            scope_add(&method_scope, mp->method_params[j]);
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
                if (!method_body) { e->fn_body_depth--; e->scope = method_scope.parent; scope_free(&method_scope); return NULL; }
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

        FnDef *method_fd = mp->method_fd;
        method_fd->body = method_body;

        /* Arrow head: the method's declared return was the class variable (the
         * function arrow), flagged as a boxed-TY_FN placeholder in pass 1.  Now
         * that the body is elaborated, refine the result's full signature from
         * the body's actual closure type (arity, boxing) so a caller binding
         * the result -- `(let [h (comp f g)] (h 3))` -- sees a callable closure
         * with the right arity rather than an arity-0 shell. */
        if (mp->arrow_return && method_body && method_body->type.kind == TY_FN &&
            method_fd->binding->type.kind == TY_FN) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = method_body->type;
            /* Preserve the body's actual boxing: a *capturing* arrow body (e.g.
             * comp's `(fn [x] (g (f x)))`) is a boxed fat closure, so a caller
             * applying the result -- `(h 3)` -- uses the thunk convention; a
             * *non-capturing* body (e.g. Category ident's `(fn [x] x)`) is
             * lifted to a bare function pointer and must stay unboxed so `(i 41)`
             * emits a direct call (and arrow_fat_shim boxes it when fed to a fat
             * parameter).  Forcing boxed=true here mis-types the non-capturing
             * case and crashes the thunk dispatch on a code address. */
            method_fd->binding->type.as.fn.result_full_type = rft;
        }

        /* Register the method now -- after its body's lifted lambdas were
         * registered above -- so emit order is `[body lambdas...][method def]`,
         * matching the original single-pass behaviour. */
        scope_add(&e->global, method_fd->binding);
        Expr *method_def_expr = expr_new(e->arena, EX_FN_DEF,
                                         method_fd->binding->type, mp->impl_form->span);
        method_def_expr->as.fn_def_.fn = method_fd;
        elab_register_file_def(e, method_def_expr);
    }
    /* Restore the signature-tyvar scope now that every method body is done. */
    e->n_sig_tyvars = saved_n_sig_tyvars;

    /* Instance was registered and its fields wired up before the body loops
     * (see above) so intra-instance `.sibling self` dispatch resolves -- now
     * including forward references and mutual recursion.  The method_impls
     * slots are fully populated with elaborated bodies. */

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
        const char *cur_basename =
            tc_path_basename(diag_file_path(call->span.file_id));
        for (uint8_t i = 0; i < n_type_args && !owns_a_type_arg; i++) {
            if (type_args[i].kind == TY_STRUCT && type_args[i].as.struct_.def) {
                if (type_args[i].as.struct_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
                continue;
            }
            /* An ADT (defdata/defgadt) type-arg, like Functor [Either], is owned
             * by the module that declares it -- mirror the TY_STRUCT path. */
            if (type_args[i].kind == TY_ADT && type_args[i].as.adt_.def) {
                if (type_args[i].as.adt_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
                continue;
            }
            /* A partially-applied head, like Functor [(Either E)], is a TY_APP
             * whose fn carries the constructor's ADT/struct identity.  Credit
             * the instance to the constructor's owning module. */
            if (type_args[i].kind == TY_APP && type_args[i].as.app.fn) {
                const Type *fn = type_args[i].as.app.fn;
                if (fn->kind == TY_ADT && fn->as.adt_.def &&
                    fn->as.adt_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                } else if (fn->kind == TY_STRUCT && fn->as.struct_.def &&
                           fn->as.struct_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
                continue;
            }
            /* KB-030: a built-in primitive type (str, rc, ...) has no StructDef,
             * so it can never match origin_file_id.  Credit it to its designated
             * home file instead, so primitive instances can live in the natural
             * module without tripping the orphan check.  The home is found
             * either from the resolved TypeKind (rc/weak -> TY_RC/TY_WEAK) or,
             * for opaque-struct names with no dedicated kind (str), from the
             * recorded type-arg symbol. */
            const char *home = builtin_kind_home_basename(type_args[i].kind);
            if (!home && type_arg_syms && type_arg_syms[i]) {
                home = builtin_type_home_basename(type_arg_syms[i]->name);
            }
            if (home && cur_basename && strcmp(cur_basename, home) == 0) {
                owns_a_type_arg = true;
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
    char type_suffix[320] = "";  /* wide enough for the longest mangled component (<=259) */
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_SYM:      component = "Sym";      break;
            case TY_STRUCT:
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.struct_.def &&
                         inst->type_args[i].as.struct_.def->name)
                    component = inst->type_args[i].as.struct_.def->name;
                break;
            default: break;
        }
        char comp_buf[128];
        tur_mangle_ident(component, comp_buf, sizeof(comp_buf));
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(dst, dstlen, "dict_%s%s", tc->name->name, type_suffix);
    return d;
}

/* Phase RT: does `t` (or any nested type) reference the named type variable? */
static bool rt_type_mentions_tyvar(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return rt_type_mentions_tyvar(t->as.app.fn, name) ||
                   rt_type_mentions_tyvar(t->as.app.arg, name);
        default:
            return false;
    }
}

/* Phase RT: walk a method's declared return type in parallel with the expected
 * type, binding the dispatch tyvar `rv` to the corresponding subtree of the
 * expected type.  Handles the bare case (return == `a`, binds a := expected)
 * and structured returns ((Result a E) vs (Result User E), binds a := User).
 * Returns true and writes *out_bound when `rv` was located; false otherwise. */
static bool rt_unify_return(const Type *ret, const Type *expected,
                            const char *rv, Type *out_bound) {
    if (!ret || !expected) return false;
    if (ret->kind == TY_TYVAR && ret->as.tyvar_.name &&
        strcmp(ret->as.tyvar_.name, rv) == 0) {
        *out_bound = *expected;
        return true;
    }
    if (ret->kind == TY_APP && expected->kind == TY_APP) {
        if (rt_unify_return(ret->as.app.arg, expected->as.app.arg, rv, out_bound))
            return true;
        if (rt_unify_return(ret->as.app.fn, expected->as.app.fn, rv, out_bound))
            return true;
    }
    return false;
}

/* Shared callable-result helper (return-type-dispatch-nullary-arrow plan, T2).
 * A method whose binding return type is a *boxed* TY_FN carries a callable fat
 * closure value (the capturing arrow body recovered in pass 2 -- report
 * function-arrow-not-instantiable, fix #1).  Return its full signature so a
 * caller applying the result -- `(h 3)` -- sees the real arity and dispatches
 * through the fat (thunk) protocol instead of an arity-0 shell.  Otherwise fall
 * back to the supplied carrier type: the unboxed fn-handle ABI in
 * elab_method_call, or the unified/ascribed `bound` in return dispatch.
 *
 * Note the boxed gate: a regular closure-returning method may declare an
 * *unboxed* TY_FN return whose body nonetheless captures (so the runtime value
 * is a fat box, applied through TUR_APPLY1).  Typing that as a bare function
 * pointer would miscompile a direct call, so unboxed declared returns keep the
 * opaque carrier here.  Arrow methods, whose result_full_type is refined from
 * the *body* in pass 2 (accurate boxing), are handled directly in
 * elab_try_return_dispatch where the unboxed bare-fn case is wanted. */
static Type method_callable_result_type(const Binding *binding, Type fallback) {
    if (binding && binding->type.kind == TY_FN) {
        const Type *rft = binding->type.as.fn.result_full_type;
        if (rft && rft->kind == TY_FN && rft->as.fn.boxed) {
            return *rft;
        }
    }
    return fallback;
}

Expr *elab_try_return_dispatch(Elab *e, const Form *call, const Symbol *name,
                               bool *handled) {
    if (handled) *handled = false;
    if (!name || call->tag != F_LIST) return NULL;

    TypeClassEnv *env = &e->typeclass_env;

    /* Find a return-only-dispatch method named `name`: one of the class's type
     * parameters appears in the method's return type but in none of its
     * parameter types, so the instance can only be picked from the expected
     * result type. */
    TypeClass *tc = NULL;
    uint8_t midx = 0;
    const TypeClassMethod *meth = NULL;
    const char *disp_tv = NULL;
    for (TypeClass *c = env->typeclasses; c != NULL && !meth; c = c->next) {
        for (uint8_t mi = 0; mi < c->n_methods; mi++) {
            const TypeClassMethod *m = &c->methods[mi];
            if (!(m->name->len == name->len &&
                  memcmp(m->name->name, name->name, name->len) == 0)) {
                continue;
            }
            /* ECS E2d-P6 (Issue 2): multi-param dispatch resolvable from an
             * argument-position param.  When SOME class type parameter appears
             * in a parameter type, argument-based dispatch can pin the instance
             * (the receiver's static type selects it), and any return-only
             * param is then read off the matched instance.  Decline
             * return-only dispatch here so elab_method_call's argument dispatch
             * takes over -- otherwise a method like `(sop-get [s : S idx : int]
             * : E)` on `(defclass StorageOps [S E])` would key the lookup on E
             * (return-only) and fail to find the instance even though S is fully
             * known from the receiver.  A genuinely return-only method (every
             * class type param absent from the parameter list, e.g. Serializable
             * `(deserialize [b : ptr<void>] : a)`) still flows through below. */
            bool any_tp_in_param = false;
            for (uint8_t ti = 0; ti < c->n_type_params && !any_tp_in_param; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                for (uint8_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        any_tp_in_param = true;
                        break;
                    }
                }
            }
            if (any_tp_in_param) continue;
            for (uint8_t ti = 0; ti < c->n_type_params; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                if (!rt_type_mentions_tyvar(&m->return_type, tp->name)) continue;
                bool in_param = false;
                for (uint8_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        in_param = true;
                        break;
                    }
                }
                if (in_param) continue;
                tc = c; midx = mi; meth = m; disp_tv = tp->name;
                break;
            }
            if (meth) break;
        }
    }
    if (!meth) return NULL;  /* not a return-only-dispatch method */
    if (handled) *handled = true;

    /* Arrow head: if this class has a function-arrow instance and the call has
     * arguments, the arrow argument (a function) selects the instance via the
     * normal argument-based dispatcher -- so decline return-dispatch here and
     * let argument dispatch take over, rather than demanding a return-type
     * ascription.  Nullary arrow methods (no argument to dispatch on, e.g. an
     * ArrowZero `zero`) still fall through to expected-type return-dispatch. */
    if (call->as.list.len > 1 &&
        typeclass_has_arrow_instance(&e->typeclass_env, tc)) {
        if (handled) *handled = false;
        return NULL;
    }

    Type bound;
    TypeClassInstance *inst = NULL;
    bool abstract_return_dispatch = false;
    if (!e->expected_type) {
        /* Mechanism B (return-type-dispatch-nullary-arrow plan, T3): with no
         * expected type a nullary arrow method (e.g. Category `ident`) has
         * nothing to dispatch on.  Fall back to the instance set: if `tc`+`meth`
         * has exactly one implementing instance and its head is the function
         * arrow, select it unambiguously.  Gating to a *unique arrow* instance
         * keeps existing multi-instance return-only classes (default-of,
         * schema-of, decode!, ...) on the ascription-required path below. */
        TypeClassInstance *uniq = NULL;
        int n_impl = 0;
        for (TypeClassInstance *it = env->instances; it; it = it->next) {
            if (it->typeclass != tc) continue;
            if (midx >= it->n_method_impls || !it->method_impls[midx]) continue;
            n_impl++;
            uniq = it;
        }
        if (n_impl == 1 && uniq->n_type_args > 0 &&
            uniq->type_args[0].kind == TY_FN) {
            inst  = uniq;
            bound = uniq->type_args[0];
        } else {
            /* T4: genuinely ambiguous (no instance, or >1, or non-arrow head) --
             * keep requiring an ascription; never silently pick an instance. */
            diag_emit(DIAG_ERROR, call->span,
                      "cannot infer type for return-directed method '%s'; add a "
                      "type ascription, e.g. (:: (%s ...) T)",
                      name->name, name->name);
            return NULL;
        }
    } else {
        /* Bind the dispatch tyvar from the expected type (bare or structured). */
        if (!rt_unify_return(&meth->return_type, e->expected_type, disp_tv,
                             &bound)) {
            diag_emit(DIAG_ERROR, call->span,
                      "ascribed type does not match the result shape of '%s'",
                      name->name);
            return NULL;
        }
        /* return-dispatch-tyvar (docs/reported/return-dispatch-tyvar-silent-
         * misdispatch.md): when the ascription pins the result to an *abstract*
         * type variable -- e.g. `(:: (deserialize b) A)` inside a constrained
         * `(defn round [A] [(Serializable A)] ...)` -- `bound` is a TY_TYVAR
         * (or the TY_STRUCT-NULL-def "abstract tyvar" representation) and there
         * is no concrete instance to pick yet.  Mirror the receiver-dispatch
         * path (`obj_is_abstract_tyvar` in elab_method_call): select the
         * carrier-compatible `int` instance as the polymorphic-base
         * representative, and tag the call so the emit-side re-resolution
         * (emit_core.c) specializes it to the concrete A per monomorphization.
         * Without this we either silently mis-dispatched to the `ptr<void>`
         * instance (the original bug, back when `deserialize` returned `:int`
         * and the class var was absent from the signature) or hard-errored. */
        bool bound_is_abstract_tyvar =
            bound.kind == TY_TYVAR ||
            (bound.kind == TY_STRUCT && bound.as.struct_.def == NULL);
        if (bound_is_abstract_tyvar) {
            for (TypeClassInstance *it = env->instances; it; it = it->next) {
                if (it->typeclass != tc) continue;
                if (midx >= it->n_method_impls || !it->method_impls[midx]) continue;
                if (it->n_type_args > 0 && it->type_args[0].kind == TY_INT) {
                    inst = it;
                    break;
                }
            }
            abstract_return_dispatch = (inst != NULL);
        }
        if (!inst) {
            inst = typeclass_env_lookup_instance(env, tc, &bound, 1);
        }
        if (!inst) {
            diag_emit(DIAG_ERROR, call->span,
                      "no instance '%s %s'", tc->name->name, type_name(bound));
            return NULL;
        }
    }
    FnDef *impl = inst->method_impls[midx];
    if (!impl || !impl->binding) {
        diag_emit(DIAG_ERROR, call->span,
                  "instance '%s %s' has no implementation for method '%s'",
                  tc->name->name, type_name(bound), name->name);
        return NULL;
    }

    /* Elaborate any arguments (clearing the expected-type channel so they are
     * synthesized normally). */
    uint32_t n_args = call->as.list.len - 1;
    Expr **args = (n_args == 0) ? NULL
        : (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    Type *saved_expected = e->expected_type;
    e->expected_type = NULL;
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) { e->expected_type = saved_expected; return NULL; }
    }
    e->expected_type = saved_expected;

    /* Thread the callable result type (plan T2/T3).  For an arrow method the
     * impl binding's result_full_type was refined from the *body* in pass 2,
     * so its boxing is accurate: a non-capturing body (Category ident's
     * `(fn [x] x)`) is an unboxed bare function pointer applied directly
     * (`(i 41)`), while a capturing body (comp's `(fn [x] (g (f x)))`) is a
     * boxed fat closure applied through the thunk protocol.  Use it whichever
     * way it is boxed so the result carries the real arity; non-arrow
     * return-dispatch methods (default-of, schema-of, ...) have no fn-typed
     * result_full_type and fall back to the unified/ascribed return type.
     *
     * Prereq 5: when an ascription pins the call's return type (the common
     * case for return-dispatch methods), use `*e->expected_type` rather than
     * the bare `bound` dispatch-tyvar value. Otherwise a method declared
     * `(decode [v : int] : (Result a cstr))` ascribed to `(Result cstr cstr)`
     * would set the call's elab type to `cstr` (just the `a`-binding) instead
     * of `(Result cstr cstr)` -- the carrier-to-by-value bridge at the
     * `ok-val` consumer's call site then misses because the elab type kind is
     * TY_CSTR rather than TY_APP, and the compile fails to compile when
     * passing the int64 carrier into a parameter expecting the by-value
     * struct. */
    Type result_type = e->expected_type ? *e->expected_type : bound;
    if (impl->binding && impl->binding->type.kind == TY_FN) {
        const Type *rft = impl->binding->type.as.fn.result_full_type;
        if (rft && rft->kind == TY_FN) {
            result_type = *rft;
        }
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    out->as.call_.fn_binding = impl->binding;
    out->as.call_.fn_expr    = NULL;
    out->as.call_.args       = args;
    out->as.call_.n_args     = n_args;
    /* M4c Path A return-side
     * (docs/reported/m4c-path-a-result-side-needs-return-dispatch-elab-hook.md):
     * mirror the receiver-dispatch path's abi_bindings population (around
     * line 4047) so emit_abi_register_call mints a per-instantiation spec
     * for return-dispatch typeclass methods too.  `bound` here is the
     * call-site's pinned dispatch-tyvar value — for `(:: (dec 42) (Result
     * int cstr))` it's `int`, exactly what the spec system needs to
     * substitute the method's `(Result a cstr)` return into `(Result int
     * cstr)`.  HKT carve-out preserved. */
    if (inst && tc && out->as.call_.fn_binding != NULL) {
        bool is_hkt = false;
        if (tc->type_param_kinds) {
            for (uint8_t i = 0; i < tc->n_type_params; i++) {
                if (tc->type_param_kinds[i] != KIND_STAR) { is_hkt = true; break; }
            }
        }
        if (!is_hkt && tc->n_type_params == 1 && tc->type_params[0]) {
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = bound;
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = 1;
        }
    }
    /* return-dispatch-tyvar: tag the abstract-tyvar return-dispatch call with a
     * dict_arg carrying the (representative int) instance + method name, so the
     * emit-side return-dispatch re-resolution can specialize it to the concrete
     * A per monomorphization (keyed on the call's result type, which is the
     * abstract tyvar here).  `out->type` stays the tyvar `bound`. */
    if (abstract_return_dispatch && inst) {
        Expr *dict_expr = make_dict_expr(e, inst, call->span);
        tur_mangle_ident(name->name, dict_expr->as.dict_.method_name,
                         sizeof(dict_expr->as.dict_.method_name));
        out->as.call_.dict_arg = dict_expr;
        out->type = bound;  /* keep the abstract tyvar for emit re-resolution */
    }
    return out;
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
            tur_mangle_ident(method_name, dict_w->as.dict_.method_name,
                             sizeof(dict_w->as.dict_.method_name));

            /* Build EX_CALL: args array is [obj_w, args_w...]. */
            Expr **call_args_w = (Expr **)arena_alloc(e->arena,
                                                       (n_args_w + 1) * sizeof(Expr *));
            call_args_w[0] = obj_w;
            for (uint32_t i = 0; i < n_args_w; i++) call_args_w[i + 1] = args_w[i];

            Expr *out_w = expr_new(e->arena, EX_CALL, result_type_w, call->span);
            /* Phase H §1: witness pins a specific instance statically, so emit
             * a direct call to the instance impl instead of dict-dispatching. */
            if (witness_method_fn && witness_method_fn->binding) {
                out_w->as.call_.fn_binding = witness_method_fn->binding;
                out_w->as.call_.fn_expr    = NULL;
            } else {
                out_w->as.call_.fn_binding = NULL;
                out_w->as.call_.fn_expr    = dict_w;
            }
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
        const Type *field_owner_type = &obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
            field_owner_type = &base;
        }
        /* DS3-2b: auto-deref rc<Struct> receivers so (.field rc-of-s)
         * resolves through the struct def carried on the rc type. */
        StructDef *rc_struct_def = NULL;
        if (base.kind == TY_RC && base.as.rc.struct_def) {
            rc_struct_def = base.as.rc.struct_def;
            base.kind = TY_STRUCT;
            base.as.struct_.def = rc_struct_def;
            field_owner_type = &base;
        }
        StructDef *app_struct_def = NULL;
        if (base.kind == TY_APP) {
            Type app_args[8];
            for (uint32_t si = 0; si < e->n_struct_defs; si++) {
                StructDef *candidate = e->struct_defs[si];
                if (candidate->n_type_params == 0 || candidate->n_type_params > 8) continue;
                if (elab_struct_type_extract_args(&base, candidate, app_args)) {
                    app_struct_def = candidate;
                    base = type_struct(candidate);
                    field_owner_type = &obj->type;
                    break;
                }
            }
        }
        if (base.kind == TY_STRUCT) {
            /* Object is a struct — try field lookup */
            StructDef *def = app_struct_def ? app_struct_def : base.as.struct_.def;
            /* Gap H item 2: when a typeclass method is declared with a typed
             * parameter `[w : W]`, the elaborator can produce a TY_STRUCT
             * with a NULL def for the receiver inside the method body if
             * the class type variable W never got concretely bound (or
             * binding propagation hit a soft spot). Pre-fix this segfaulted
             * dereferencing `def->n_fields`; now we skip the field-access
             * fast path and fall through to the regular typeclass-dispatch
             * lookup below, which emits a clean diagnostic if no instance
             * matches. Filed under
             * docs/reported/typeclass-constrained-defn-rejected.md. */
            if (!def) {
                /* Treat as "not a struct after all" -- bail to dispatch path. */
                goto skip_struct_field_lookup;
            }
            for (uint32_t i = 0; i < def->n_fields; i++) {
                if (strcmp(def->fields[i].name, method_name) == 0) {
                    /* Found matching field — build EX_GET_FIELD */
                    Type field_type = elab_struct_field_use_type(e, field_owner_type, def, &def->fields[i]);
                    Expr *out = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
                    out->as.get_field_.struct_expr = obj;
                    out->as.get_field_.field_idx = i;
                    out->as.get_field_.def = def;
                    /* LT1/T3: Extracting an lref<T> field from a :move struct transfers
                     * linear ownership of that field out of the struct.  Mark the struct
                     * binding as moved so a second extraction of the same field triggers
                     * TUR_E0005 (use-after-move).  :linear struct receivers are already
                     * handled by the F_SYM is_linear_consumed path above. */
                    if (g_linear_enabled && def->fields[i].kind == TY_LREF &&
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
skip_struct_field_lookup:;
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
        if (base.kind == TY_APP) {
            Type app_args[8];
            for (uint32_t si = 0; si < e->n_struct_defs; si++) {
                StructDef *candidate = e->struct_defs[si];
                if (candidate->n_type_params == 0 || candidate->n_type_params > 8) continue;
                if (elab_struct_type_extract_args(&base, candidate, app_args)) {
                    base = type_struct(candidate);
                    break;
                }
            }
        }
        if (base.kind == TY_STRUCT) {
            StructDef *def = base.as.struct_.def;
            /* Gap H item 2 (mirrored from the earlier struct-field-lookup
             * branch at L3320-3336): the class-var receiver can be left as
             * a TY_STRUCT with NULL def when the class type variable was
             * never concretely bound -- e.g. inside a constrained-poly
             * defn `[(Eq A)] [x : A]` whose receiver's resolved type stays
             * an unbound stand-in.  Dereferencing `def->n_fields` here
             * segfaulted (the original report's "elab_typeclasses.c:3388
             * SEGV").  Bail out to the regular typeclass-dispatch lookup
             * below, which emits a clean diagnostic if no instance matches.
             *
             * Filed under docs/reported/m5-eq-vec-rewrite-fn-arg-loses-
             * annotation.md (gap 2). */
            if (!def) goto skip_capability_field_lookup;
            for (uint32_t i = 0; i < def->n_fields; i++) {
                if (strcmp(def->fields[i].name, method_name) == 0 &&
                    def->fields[i].kind == TY_FN) {
                    /* Build EX_GET_FIELD for the function pointer */
                    Type field_type = elab_struct_field_use_type(e, &obj->type, def, &def->fields[i]);
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
skip_capability_field_lookup:;
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
    /* stdlib-hkt-consolidation T1: when the receiver type is erased to int64_t,
     * every name-matching instance becomes a "fallback" and >1 of them is
     * reported as ambiguous (TUR_E0020). Adding stdlib HKT instances (e.g.
     * Functor/Monad [Option]) introduces a second fallback for programs that
     * define their own same-shaped instance, turning previously-unambiguous
     * dispatch into an error. To keep a locally-defined instance authoritative,
     * prefer the unique *user* (non-stdlib) fallback when the only other
     * candidates are stdlib-provided. This is purely additive: it only changes
     * cases that today emit TUR_E0020, so no currently-resolving dispatch is
     * affected. The key is the instance's *origin* file (not the call span,
     * which for macro-expanded `.bind`/`.fmap` points at stdlib/macros.tur). */
    FnDef *user_fallback_method = NULL;
    TypeClassInstance *user_fallback_inst = NULL;
    int user_fallback_count = 0;

    /* GHE (constrained-generic-instance-dispatch): when the receiver is a bare
     * type variable `K` (a constrained generic type parameter, e.g. the `x : K`
     * of `(defn f [^Hash K x :K] ...)`), the concrete instance is not known
     * until the function is monomorphized.  This compiler realizes constrained
     * generics via emit-time ABI specialization, so we must:
     *   (a) pick a *carrier-compatible* representative instance here -- the one
     *       whose type_args[0] is TY_INT, since the polymorphic base clone takes
     *       the int64_t carrier and a tyvar key bottoms out at the carrier.
     *       This makes the base clone valid C *and* correct for `int` keys.
     *   (b) tag the call (via dict_arg, built below from best_inst's typeclass)
     *       so emit_call_name can re-resolve to __inst_<Class>_<method>_<T> for
     *       each non-carrier ABI specialization (cstr/bool/float32/...).
     * Without this the old KIND_ARROW path spuriously matched the first instance
     * whose type_args[0] failed the (incomplete) primitive test -- typically
     * Hash[float32] -- baking a wrong, type-incompatible callee into the body. */
    /* M5 (docs/reported/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md):
     * An EX_ASCRIBE-to-tyvar receiver (`(:: v A)`) elaborates to a
     * TY_STRUCT with NULL def -- the "abstract tyvar" representation
     * the elaborator uses when ascribing a concrete value to a class-
     * constraint tyvar.  Treat it the same as TY_TYVAR for typeclass
     * dispatch: pick the carrier-compatible (int) instance, then let
     * emit-side re-resolution (emit_core.c:emit_reresolve_method_call)
     * specialize to the concrete A per call site.  Without this branch
     * the dispatch fell through to the KIND_ARROW iteration below and
     * picked the first non-primitive instance from the env (typically
     * `Eq MutableMap`, alphabetically near the head), producing a
     * silent miscompile that SIGSEGV'd at runtime. */
    bool obj_is_abstract_tyvar =
        obj->type.kind == TY_TYVAR ||
        (obj->type.kind == TY_STRUCT && obj->type.as.struct_.def == NULL);
    if (obj_is_abstract_tyvar) {
        TypeClassInstance *carrier_inst = NULL;
        FnDef *carrier_method = NULL;
        for (TypeClassInstance *inst = e->typeclass_env.instances;
             inst != NULL && !carrier_inst; inst = inst->next) {
            for (uint8_t i = 0; i < inst->typeclass->n_methods; i++) {
                const TypeClassMethod *method = &inst->typeclass->methods[i];
                if (method->name->len != method_name_len ||
                    memcmp(method->name->name, method_name, method_name_len) != 0) {
                    continue;
                }
                if (inst->n_type_args > 0 && inst->type_args[0].kind == TY_INT) {
                    carrier_inst = inst;
                    carrier_method = inst->method_impls[i];
                }
                break; /* one method match per instance */
            }
        }
        if (carrier_inst) {
            best_method = carrier_method;
            best_inst = carrier_inst;
            exact_match_found = true;
            goto found_method;
        }
        /* No int instance for this class: fall through to the generic search
         * (keeps prior behavior for classes without a carrier-compatible
         * instance; such a constrained generic would still need a fix). */
    }

    /* Determine the effective constructor kind from the obj type. */
    Kind obj_ck = KIND_STAR;
    {
        TypeKind tk = obj->type.kind;
        /* M5 fix: the sized numeric variants are primitives too; without
         * them, an obj of type :float32 / :int8 / etc. gets obj_ck=KIND_ARROW
         * and the wrong-direction iteration matches via the non-primitive
         * fallthrough branch instead of the kind-exact-match branch.  See
         * the symmetric inst_is_primitive fix below at line ~3675. */
        bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                             tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID ||
                             tk == TY_SYM  || tk == TY_UNKNOWN ||
                             tk == TY_INT8 || tk == TY_INT16 || tk == TY_INT32 ||
                             tk == TY_INT64 ||
                             tk == TY_UINT8 || tk == TY_UINT16 || tk == TY_UINT32 ||
                             tk == TY_UINT64 ||
                             tk == TY_FLOAT32 || tk == TY_FLOAT64);
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
             * exactly; for KIND_ARROW we accept any non-primitive whose
             * struct constructor matches (when known via TY_APP). */
            if (inst->n_type_args > 0 && obj->type.kind != TY_UNKNOWN) {
                bool type_ok;
                if (obj_ck == KIND_STAR) {
                    type_ok = (inst->type_args[0].kind == obj->type.kind);
                } else {
                    /* KIND_ARROW: accept non-primitive instance type_args.
                     * F3-7 (cross-plan-followups): when the receiver is a
                     * TY_APP, walk to its head and use the struct identity
                     * to discriminate Eq[Vec] from Eq[Map] etc.  Without
                     * this, a TY_APP(Vec, int) receiver matches the first
                     * KIND_ARROW Eq instance in registration order
                     * (typically Eq[Set]) and we silently dispatch through
                     * the wrong vtable. */
                    TypeKind itk = inst->type_args[0].kind;
                    /* M5 fix (docs/reported/m5-constrained-poly-spec-wrong-
                     * dispatch-for-parametric-receiver.md): the sized numeric
                     * variants are primitives too -- without listing them, an
                     * `Eq float32` (or `Eq int32` / `uint8` / etc.) instance
                     * slips through `type_ok = !inst_is_primitive` for a
                     * parametric receiver like `(Vec A)`, becomes a false
                     * "good match", and overrides the correctly-rejected
                     * `Eq Vec` (rejected by its `(Eq A)` type-param constraint
                     * because A is still a TYVAR at this elab pass).  Result
                     * was a baked `__inst_Eq_eq_qu_float32(Vec__int, Vec__int)`
                     * call -- a hard cc error. */
                    bool inst_is_primitive =
                        (itk == TY_INT  || itk == TY_BOOL || itk == TY_CSTR ||
                         itk == TY_NIL  || itk == TY_FLOAT || itk == TY_PTR_VOID ||
                         itk == TY_SYM  ||
                         itk == TY_INT8 || itk == TY_INT16 || itk == TY_INT32 ||
                         itk == TY_INT64 ||
                         itk == TY_UINT8 || itk == TY_UINT16 || itk == TY_UINT32 ||
                         itk == TY_UINT64 ||
                         itk == TY_FLOAT32 || itk == TY_FLOAT64);
                    type_ok = !inst_is_primitive;
                    /* Arrow head (itk == TY_FN): an `Arrow [(->)]` instance
                     * matches only a function receiver, never a struct/vec.
                     * Conversely, a function receiver must not bind a non-arrow
                     * KIND_ARROW instance (e.g. an opaque container). */
                    if (type_ok && (itk == TY_FN || obj->type.kind == TY_FN)) {
                        type_ok = (itk == TY_FN && obj->type.kind == TY_FN);
                    }
                    if (type_ok && obj->type.kind == TY_APP && itk == TY_STRUCT) {
                        const Type *head = &obj->type;
                        while (head && head->kind == TY_APP) head = head->as.app.fn;
                        if (head && head->kind == TY_STRUCT &&
                            inst->type_args[0].as.struct_.def != NULL &&
                            inst->type_args[0].as.struct_.def != head->as.struct_.def) {
                            type_ok = false;
                        }
                    }
                    /* ECS E2d-P6 (Issue 3): when BOTH the receiver and the
                     * instance head are fully-applied type constructors -- e.g. a
                     * receiver `(Dense Pos)` against instances `[(Dense Pos) Pos]`
                     * and `[(Sparse Vel) Vel]` -- the bare `!inst_is_primitive`
                     * test above accepts every non-primitive instance, so the
                     * first one in registration order silently wins regardless of
                     * the constructor (Dense vs Sparse) or argument (Pos vs Vel).
                     * Discriminate by (a) head-constructor identity and (b) the
                     * leftmost type argument when both sides are concrete, so each
                     * storage backend dispatches to its own instance.  This is the
                     * miscompile case CLAUDE.md flags: "works by luck because the
                     * register classes happen to match".
                     *
                     * Carrier/tyvar/erased instance arguments (a partially-applied
                     * head like `Functor [(Result _ B)]`, whose varying arm is
                     * erased to the int64 carrier) act as wildcards: comparing
                     * only concrete-vs-concrete keeps those instances matching. */
                    /* Plain (non-applied) aggregate receiver against a plain
                     * aggregate instance head: discriminate by type identity.
                     * Without this, every struct/ADT instance of a class
                     * (e.g. Render[Color4], Render[Color8], Render[Color24])
                     * looks like an equally-good non-primitive match, so the
                     * first one in registration order silently wins.  Since
                     * instances are prepended (typeclass.c:register_instance),
                     * "first in registration order" is the LAST-declared
                     * instance -- so a struct-argument typeclass call resolves
                     * every call site to the last-declared instance.  That is a
                     * miscompile (a hard cc type error when the struct layouts
                     * differ, a silent wrong-vtable dispatch when they happen to
                     * match -- the "works by luck because the register classes
                     * match" case CLAUDE.md flags).  Carrier/erased heads
                     * (NULL def) stay wildcards. */
                    if (type_ok && obj->type.kind == TY_STRUCT && itk == TY_STRUCT &&
                        obj->type.as.struct_.def && inst->type_args[0].as.struct_.def &&
                        obj->type.as.struct_.def != inst->type_args[0].as.struct_.def) {
                        type_ok = false;
                    }
                    if (type_ok && obj->type.kind == TY_ADT && itk == TY_ADT &&
                        obj->type.as.adt_.def && inst->type_args[0].as.adt_.def &&
                        obj->type.as.adt_.def != inst->type_args[0].as.adt_.def) {
                        type_ok = false;
                    }
                    if (type_ok && obj->type.kind == TY_APP &&
                        inst->type_args[0].kind == TY_APP) {
                        const Type *oh = &obj->type;
                        while (oh && oh->kind == TY_APP) oh = oh->as.app.fn;
                        const Type *ih = &inst->type_args[0];
                        while (ih && ih->kind == TY_APP) ih = ih->as.app.fn;
                        bool heads_differ = false;
                        if (oh && ih && oh->kind == TY_STRUCT && ih->kind == TY_STRUCT &&
                            oh->as.struct_.def && ih->as.struct_.def &&
                            oh->as.struct_.def != ih->as.struct_.def) {
                            heads_differ = true;
                        } else if (oh && ih && oh->kind == TY_ADT && ih->kind == TY_ADT &&
                                   oh->as.adt_.def && ih->as.adt_.def &&
                                   oh->as.adt_.def != ih->as.adt_.def) {
                            heads_differ = true;
                        }
                        if (heads_differ) {
                            type_ok = false;
                        } else {
                            const Type *oa = obj->type.as.app.arg;
                            const Type *ia = inst->type_args[0].as.app.arg;
                            bool oa_concrete = oa &&
                                ((oa->kind == TY_STRUCT && oa->as.struct_.def) ||
                                 (oa->kind == TY_ADT && oa->as.adt_.def));
                            bool ia_concrete = ia &&
                                ((ia->kind == TY_STRUCT && ia->as.struct_.def) ||
                                 (ia->kind == TY_ADT && ia->as.adt_.def));
                            if (oa_concrete && ia_concrete && !type_eq(*oa, *ia)) {
                                type_ok = false;
                            }
                        }
                    }
                }
                if (!type_ok) {
                    /* Record as fallback but keep searching. */
                    fallback_count++;
                    if (!best_method) { best_method = inst->method_impls[i]; best_inst = inst; }
                    /* Track user (non-stdlib) fallbacks so an ambiguity between a
                     * local instance and a stdlib one resolves to the local one. */
                    {
                        const char *opath = diag_file_path(inst->origin_file_id);
                        /* stdlib files always live directly under a dir ending
                         * in "stdlib" (see resolve_stdlib_root in main.c), so the
                         * path contains "stdlib/<basename>". The root may be
                         * relative ("stdlib/option.tur") or absolute, so match
                         * the "stdlib/" component without requiring a leading
                         * slash. Classify purely by path: file_id 0 is the entry
                         * (user) file, NOT unknown -- its path is the user's
                         * program, so it must count as a user instance. */
                        bool is_stdlib = (opath && strstr(opath, "stdlib/") != NULL);
                        if (!is_stdlib) {
                            user_fallback_count++;
                            if (!user_fallback_method) {
                                user_fallback_method = inst->method_impls[i];
                                user_fallback_inst = inst;
                            }
                        }
                    }
                    continue;
                }
            }
            /* Phase PTC3/PTC4: Check type parameter constraints on this instance.
             * Extract TY_APP elem types in type_params order (innermost first)
             * to support multi-param types like Map[K V]. */
            if (inst->type_param_constraints && inst->n_type_param_constraints > 0) {
                Type obj_type = obj->type;
                const Type *elem_types = NULL;
                uint8_t n_elem = 0;
                Type elem_buf[8];
                if (obj->type.kind == TY_APP) {
                    Type raw[8];
                    uint8_t n_raw = 0;
                    for (const Type *tx = &obj->type;
                         tx && tx->kind == TY_APP && n_raw < 8;
                         tx = tx->as.app.fn) {
                        if (tx->as.app.arg) raw[n_raw++] = *tx->as.app.arg;
                    }
                    for (uint8_t ri = 0; ri < n_raw; ri++)
                        elem_buf[ri] = raw[n_raw - 1 - ri];
                    n_elem = n_raw;
                    if (n_elem > 0) elem_types = elem_buf;
                }
                if (!typeclass_instance_constraints_satisfied(inst, &obj_type, 1,
                                                              elem_types, n_elem,
                                                              &e->typeclass_env)) {
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
        /* stdlib-hkt-consolidation T1: if exactly one of the ambiguous
         * candidates is a user-defined (non-stdlib) instance, it shadows the
         * stdlib instance(s) and dispatch resolves to it instead of erroring. */
        if (user_fallback_count == 1 && user_fallback_method) {
            best_method = user_fallback_method;
            best_inst = user_fallback_inst;
            goto resolved_user_fallback;
        }
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
resolved_user_fallback:;

    /* obj was already elaborated above for dispatch; elaborate the remaining args. */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[2 + i]);
        if (!args[i]) return NULL;
    }

    /* Arrow-identity passthrough: capture the pre-shim argument types so a
     * method whose body returns one of its (fat) parameters verbatim -- e.g.
     * `(arr [f] f)` under `Arrow [(->)]` -- can recover the concrete arrow
     * signature (and thus the real arity) of the argument it passes through.
     * Later passes (poly-wrap, arrow_fat_shim) rewrite obj/args into opaque
     * fat boxes, erasing the TY_FN type, so snapshot it now. */
    Type obj_orig_type = obj->type;
    Type *args_orig_types = n_args
        ? (Type *)arena_alloc(e->arena, n_args * sizeof(Type)) : NULL;
    for (uint32_t i = 0; i < n_args; i++) args_orig_types[i] = args[i]->type;

    /* F3-5 (cross-plan-followups): per-call-site synthesis for the
     * recursive case of typed-collection `.eq?` dispatch.  When the
     * outer instance is a constrained typed-collection (e.g. Eq[Vec])
     * and the receiver's element type is itself a TY_APP (e.g.
     * Vec[Vec[int]]), bypass the constrained instance's hardcoded
     * `(fn [a b] (= a b))` body and synthesise a direct call to the
     * helper (vec-eq?) with an inline comparator lambda whose
     * params are ascribed to the element type.  The inner `.eq?`
     * re-enters this dispatcher at the next level, terminating at
     * primitives where F3-7 takes over. */
    if (best_inst &&
        best_inst->n_type_param_constraints > 0 &&
        method_name_len == 3 &&
        memcmp(method_name, "eq?", 3) == 0 &&
        n_args == 1) {
        Expr *synth = try_synth_recursive_eq(e, best_inst, obj, args[0], call->span);
        if (synth) return synth;
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
            Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, obj->span, false);
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
                 * is a fat closure (TY_PTR_VOID, or CRU B-1's boxed TY_FN
                 * closure value — capturing or non-capturing lambda), wrap it
                 * for tur_poly_fn_t packing in the emitter. */
                if (args[i]->type.kind == TY_PTR_VOID ||
                    (args[i]->type.kind == TY_FN && args[i]->type.as.fn.boxed)) {
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
            } else if (inner_b->closure_fn_binding && !inner_b->is_global) {
                /* CRU: a *capturing closure VALUE* bound to a local reaching a
                 * `:fn` typeclass-method param.  make_poly_wrapper would emit a
                 * file-scope wrapper statically referencing the local env var
                 * (out of scope at file scope -> uncompilable C).  Pack the
                 * runtime closure inline instead; the is_closure emit path reads
                 * the thunk from the box's slot 0 at runtime, so a capturing
                 * closure round-trips correctly. */
                wrap->as.poly_wrap_.wrapper_binding = NULL;
                wrap->as.poly_wrap_.is_closure = true;
            } else {
                uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                    ? (uint8_t)inner_b->type.as.fn.arity : 1;
                Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span, false);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            args[i] = wrap;
        }
    }

    /* Arrow head (and any fat-closure method parameter): auto-shim a bare
     * (non-capturing) function argument into a fat-closure box, mirroring the
     * `^fat` coercion the regular call path applies (elab_call.c).  A capturing
     * closure (boxed TY_FN) or an already-fat :ptr<void> handle passes through.
     * params[0] is the receiver (obj); params[i+1] matches args[i]. */
    if (best_method->n_params > 0 && best_method->params[0] &&
        best_method->params[0]->is_fat) {
        obj = arrow_fat_shim(e, obj);
    }
    for (uint32_t i = 0; i < n_args; i++) {
        uint8_t pidx = 1 + (uint8_t)i;
        if (pidx < best_method->n_params && best_method->params[pidx] &&
            best_method->params[pidx]->is_fat) {
            args[i] = arrow_fat_shim(e, args[i]);
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
        /* Arrow head: when the method returns a *boxed* closure value (a
         * callable `(fn [x] ...)` recovered from an arrow instance body), use
         * its full signature so a caller applying the result -- `(h 3)` --
         * sees the real arity instead of an arity-0 shell.  Closure-returning
         * methods that carry the int64 fat handle (an unboxed fn-typed return
         * annotation) keep the carrier ABI via type_from_kind below. */
        result_type = method_callable_result_type(
            best_method->binding,
            type_from_kind(best_method->binding->type.as.fn.result_kind));
        /* ECS E2d-P6 (value-level projection): recover the precise by-value
         * struct/ADT return (with its def) from the impl binding's
         * result_full_type, so a struct result carries its def to the call site
         * -- `(.field (method ...))` resolves and a let-bound `p : Pos` lowers
         * to the by-value struct instead of the def-less carrier kind.  Gated to
         * the same non-parametric nominal types the impl side threads above;
         * boxed-fn results are already handled by method_callable_result_type. */
        {
            const Type *rft = best_method->binding->type.as.fn.result_full_type;
            if (rft &&
                ((rft->kind == TY_STRUCT && rft->as.struct_.def &&
                  rft->as.struct_.def->n_type_params == 0) ||
                 (rft->kind == TY_ADT && rft->as.adt_.def &&
                  rft->as.adt_.def->n_type_params == 0))) {
                result_type = *rft;
            }
        }
        /* Arrow-identity passthrough: the method's declared return is the arrow
         * class variable (a boxed arity-0 TY_FN shell) and its body is a bare
         * reference to one of the method's parameters -- so the dispatched
         * result *is* exactly that argument (e.g. `(arr [f] f)` returns `f`).
         * The arity-0 shell is uncallable (`(a 5)` => "returns ?"), so recover
         * the argument's concrete arrow signature here, where the call-site arg
         * type is known.  Force the boxed (thunk) convention: a fat parameter is
         * returned as a fat box, so the caller must apply it through TUR_APPLY*
         * rather than a direct fn-pointer call. */
        if (result_type.kind == TY_FN && result_type.as.fn.arity == 0 &&
            best_method->body && best_method->body->kind == EX_VAR) {
            const Binding *vb = best_method->body->as.var.binding;
            int passthru_idx = -1;
            for (uint8_t pi = 0; pi < best_method->n_params; pi++) {
                if (best_method->params[pi] == vb) { passthru_idx = pi; break; }
            }
            if (passthru_idx >= 0) {
                Type at = (passthru_idx == 0)
                    ? obj_orig_type
                    : args_orig_types[passthru_idx - 1];
                if (at.kind == TY_FN) {
                    at.as.fn.boxed = true;
                    result_type = at;
                }
            }
        }
    } else {
        result_type = best_method->body->type;
        if (result_type.kind == TY_UNKNOWN || result_type.kind == TY_NIL) {
            result_type = TYPE_INT;
        }
    }

    /* SC7 (chainable HKT return): a class method may declare a concrete `:int`
     * return (so the dictionary slot/ABI stay int64), yet its instance body
     * yields a transparent int-newtype container -- e.g. a Functor `fmap` whose
     * body is `(make-struct Schema (schema/fmap ...))` typed `(Schema b)`.
     * Propagate that container type as the call's result so the next HKT
     * operator (`ap`/`alt-or`) can dispatch on its `(Schema _)` head.  The C
     * representation is unchanged (a transparent newtype is int64 everywhere),
     * and instances whose bodies are bare `:int` carriers are unaffected. */
    if (best_method->body &&
        type_is_transparent_int_newtype(best_method->body->type)) {
        result_type = best_method->body->type;
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
        /* Mangle the method name into the EX_DICT node (must match the dict
         * field + instance-function spelling produced by the shared mangler). */
        tur_mangle_ident(method_name, dict_expr->as.dict_.method_name,
                         sizeof(dict_expr->as.dict_.method_name));
    }

    /* M7 HKT layers 1+3 (flag-gated): an HKT-applied method result `(Option b)`
     * arrives here as an empty `type_from_kind(TY_APP)` => `(type-app ? ?)`,
     * because the method's result_kind is TY_APP but type_from_kind drops the
     * head/element.  Recover the real result: take the binding's
     * result_full_type rft (`(Option b)`, with `b` a named element tyvar after
     * the parse fix + layer-0 head substitution), unify the declared parameter
     * types against the actual argument types to bind `b` (and any other element
     * tyvars), and substitute into rft so it grounds to `(Option int)`. */
    /* M7 layer-4 prep: persist the element-tyvar bindings collected here so they
     * can be attached as the dispatch call's abi_bindings below (the emit-side
     * per-(f, A) by-value spec interning reads them). */
    const Symbol *m7_bind_names[16];
    Type m7_bind_types[16];
    uint8_t m7_nb = 0;
    /* M7 layer-4 guard (see m7_type_has_free_tyvar): stays false unless the
     * substituted result type fully grounds, so an `ap`-style call whose result
     * element cannot be recovered falls back to carrier dispatch. */
    bool m7_byvalue_grounded = false;
    /* M7 layer-4: is the instance body by-value-expressible (pure-Turmeric)?
     * Computed up front because it gates BOTH the by-value result-type commit
     * below AND the abi_bindings attachment further down -- the two MUST agree.
     * Committing a by-value result type for a CARRIER-bodied method (so the
     * consumer reads by value) without also interning the by-value spec (so the
     * producer returns by value) is exactly the carrier-vs-by-value mismatch
     * that silently miscompiles a selection body to 0 (Alternative `<|>`). */
    bool m7_body_byvalue_ok = best_method && best_method->body &&
        best_method->body->kind != EX_INLINE_C &&
        m7_body_constructs_byvalue(best_method->body);
    if (g_m7_hkt_enabled && best_inst && best_inst->typeclass &&
        best_method->binding->type.kind == TY_FN &&
        best_method->binding->type.as.fn.result_full_type &&
        best_method->binding->type.as.fn.result_full_type->kind == TY_APP) {
        TypeClass *tc = best_inst->typeclass;
        const TypeClassMethod *cm = NULL;
        for (uint8_t mi = 0; mi < tc->n_methods; mi++)
            if (tc->methods[mi].name &&
                strcmp(tc->methods[mi].name->name, method_name) == 0) {
                cm = &tc->methods[mi];
                break;
            }
        if (cm) {
            /* The declared element tyvars `(g a)` / `(fn [a] b)` live on the
             * CLASS method's param_types (parsed with the method-tyvar fix), not
             * on the instance binding (whose arg_full_types is NULL). */
            const Type *rft = best_method->binding->type.as.fn.result_full_type;
            if (cm->n_params >= 1)
                m7_collect_tyvar_bindings(e, cm->param_types[0], obj_orig_type,
                                          m7_bind_names, m7_bind_types, &m7_nb, 16);
            for (uint32_t i = 0; i < n_args; i++) {
                uint8_t pidx = 1 + (uint8_t)i;
                if (pidx < cm->n_params)
                    m7_collect_tyvar_bindings(e, cm->param_types[pidx],
                                              args_orig_types[i], m7_bind_names,
                                              m7_bind_types, &m7_nb, 16);
            }
            if (m7_nb > 0) {
                Type substituted = elab_subst_class_tyvars(
                    e->arena, *rft, m7_bind_names, m7_nb, m7_bind_types, m7_nb);
                /* Only commit the by-value result type (and, below, the by-value
                 * element bindings) when the result fully grounds.  A residual
                 * free element tyvar -- the `ap` fat-closure-carrier case --
                 * keeps the carrier result_type so dispatch falls back to the
                 * uniform carrier ABI instead of emitting a broken half-by-value
                 * spec with a dangling carrier-base dict reference. */
                if (!m7_type_has_free_tyvar(substituted) && m7_body_byvalue_ok) {
                    result_type = substituted;
                    m7_byvalue_grounded = true;
                }
            }
        }
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    /* Phase H §1: receiver type is statically known here (best_inst was resolved
     * by the type-based instance search above; ambiguous matches would have
     * errored with TUR_E0020 earlier).  Emit a direct call to the instance
     * impl, skipping the dictionary indirection.  Keep dict_arg as an
     * annotation for downstream passes that may still want to know which
     * instance was selected.  Fall back to dict dispatch only when there is
     * no resolved instance (defensive). */
    if (best_method && best_method->binding) {
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    } else if (dict_expr && !has_poly_params) {
        out->as.call_.fn_binding = NULL;
        out->as.call_.fn_expr    = dict_expr;
    } else {
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    }
    out->as.call_.args    = call_args;
    out->as.call_.n_args  = n_args + 1;
    out->as.call_.dict_arg = dict_expr;  /* annotation for downstream passes */
    /* M4c Path A step 1 (docs/upcoming/m4c-execution-plan.md): bind the
     * class variable to the CALL SITE'S receiver type so
     * emit_abi_register_call mints a per-instantiation spec.  HKT carve-out
     * stays — those keep the uniform-carrier dispatch per Plan M6/M7. */
    if (best_inst && best_inst->typeclass
        && out->as.call_.fn_binding != NULL) {
        TypeClass *tc = best_inst->typeclass;
        bool is_hkt = false;
        if (tc->type_param_kinds) {
            for (uint8_t i = 0; i < tc->n_type_params; i++) {
                if (tc->type_param_kinds[i] != KIND_STAR) { is_hkt = true; break; }
            }
        }
        if (!is_hkt && tc->n_type_params == 1 && tc->type_params[0]) {
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = obj_orig_type;
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = 1;
        }
        /* M7 layer-4 prep (flag-gated): for an HKT class, attach the class var
         * (`g -> Option`) plus the element tyvars collected by layers 1+3
         * (`a -> int`, `b -> int`) as the dispatch call's abi_bindings, so
         * emit_abi_register_call can mint a per-(f, A) by-value instance-method
         * spec instead of falling through to the carrier-double-boxing path. */
        /* M7 layer-4 (flag-gated): only by-value-expressible (pure-Turmeric)
         * instance bodies can be monomorphized by value.  Carrier inline-C
         * instance bodies (the current stdlib HKT instances) must stay on the
         * uniform-carrier dispatch until Phase 4.2 rewrites them; attaching the
         * element bindings to them would mint a by-value spec whose signature
         * contradicts the carrier inline-C body.  Gate on the body kind
         * (m7_body_byvalue_ok, computed up front above). */
        if (g_m7_hkt_enabled && is_hkt && m7_body_byvalue_ok &&
            m7_byvalue_grounded &&
            tc->n_type_params >= 1 && tc->type_params[0]) {
            uint8_t total = (uint8_t)(1 + m7_nb);
            if (total > ABI_TYPE_BINDINGS_MAX) total = ABI_TYPE_BINDINGS_MAX;
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            /* Bind the HKT class var to the CONSTRUCTOR HEAD of the receiver
             * (`Option`), not the full applied receiver (`(Option int)`), so an
             * applied occurrence in the body -- e.g. the monadic continuation's
             * result `(m b)` in `bind` -- resolves to `(Option b)` -> `(Option
             * int)` at emit time instead of the nonsensical `((Option int) int)`
             * (which type_c_name's to the int64 carrier).  For the fmap shape the
             * result `(g b)` is pre-resolved by the elaborator, so this only
             * matters for in-body applied occurrences like the bind tail call. */
            Type hkt_head = obj_orig_type;
            while (hkt_head.kind == TY_APP && hkt_head.as.app.fn)
                hkt_head = *hkt_head.as.app.fn;
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = hkt_head;
            uint8_t bi = 1;
            for (uint8_t k = 0; k < m7_nb && bi < total; k++) {
                bindings[bi].name = m7_bind_names[k]->name;
                bindings[bi].type = m7_bind_types[k];
                bi++;
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = bi;
        }
    }
    return out;
}
