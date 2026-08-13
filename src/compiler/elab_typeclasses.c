/* elab_typeclasses.c -- typeclass declarations, instances, and method-call dispatch. */
#include "elab_internal.h"
#include "refine_discharge.h"     /* RT1: instance/class refinement variance */
#include "refine_solver.h"        /* RT1: refine_model_search, for the variance witness */
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
        for (uint32_t pi = 0; pi < m->n_params; pi++) {
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
    /* CONV-S2: under defstruct-as-defadt a typed-collection element is a lowered
     * record ADT (`Vec`/`Map`/...), so its bare name and TY_APP head are TY_ADT,
     * not TY_STRUCT.  Mirror the struct cases so the comparator-synthesis
     * ascription form (`(:: a (Vec int))`) is built for an ADT element; without
     * it type_to_form returns NULL, the dispatch synthesis declines, and the
     * element comparator collapses to the wrong (int) instance. */
    if (t->kind == TY_ADT && t->as.adt_.def && t->as.adt_.def->name) {
        return form_sym(e->arena, span,
            intern_cstr(e->st, t->as.adt_.def->name));
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
        const char *head_name =
            (head && head->kind == TY_ADT && head->as.adt_.def)
                ? head->as.adt_.def->name
          : NULL;
        if (!head_name) {
            return NULL;
        }
        /* Build (StructName arg1-form arg2-form ...) in original order. */
        uint32_t n_items = 1 + n_args;
        Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
        items[0] = form_sym(e->arena, span, intern_cstr(e->st, head_name));
        /* args were collected innermost-first; reverse to original order. */
        for (uint32_t i = 0; i < n_args; i++) {
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
static const Symbol *helper_eq_symbol_for_struct(Elab *e, const AdtDef *sd,
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
 * docs/archive/history/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md).
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

/* G10: a type argument is "concrete for instance discrimination" when it is a
 * named struct/ADT OR a concrete primitive (int/cstr/bool/float/...).  Two
 * instances over the same applied head differing only in a concrete primitive
 * element -- `Enc [(Option cstr)]` vs `Enc [(Option int)]` -- must be told apart;
 * the prior check considered only TY_STRUCT/TY_ADT concrete, so a primitive
 * element difference was ignored and both instances matched any `(Option X)`
 * receiver (the last-defined silently won).  A tyvar element stays a wildcard
 * (parametric instances match any element), so it is deliberately NOT concrete. */
static bool typeclass_type_arg_concrete(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_ADT:    return t->as.adt_.def != NULL;
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT64: case TY_BOOL: case TY_CSTR: case TY_FLOAT:
        case TY_FLOAT32: case TY_FLOAT64: case TY_NIL: case TY_PTR_VOID:
            return true;
        default: return false;
    }
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
    const AdtDef *sd = (outer_inst->type_args[0].kind == TY_ADT)
                           ? outer_inst->type_args[0].as.adt_.def
                           : NULL;

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

/* M7 HKT: is `sym` a candidate method-level
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
     * docs/archive/history/m7-hkt-fn-returning-applied-type-kind-mismatch.md. */
    Kind *eff_kinds = (Kind *)class_type_param_kinds;
    /* M7: collecting method-level element tyvars is a PARSE concern.  A typed
     * HKT class signature (`(foldr [ta : (t a) ...] : b)`) parses and the
     * by-value emit paths pick up the element types.  Inert for existing
     * classes whose method signatures contain no method-level tyvars (eff_tp ==
     * class params). */
    {
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
    const Form **param_refine_preds = NULL;
    const char **param_refine_vars  = NULL;

    if (n_params > 0) {
        param_names = (const Symbol **)arena_alloc(e->arena, n_params * sizeof(const Symbol *));
        param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
        param_is_fn = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        param_explicit_type = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        param_refine_preds = (const Form **)arena_alloc(e->arena, n_params * sizeof(Form *));
        param_refine_vars  = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        for (uint8_t _i = 0; _i < n_params; _i++) {
            param_refine_preds[_i] = NULL;
            param_refine_vars[_i]  = NULL;
        }
        for (uint32_t i = 0; i < n_params; i++) {
            param_is_fn[i] = false;
            param_explicit_type[i] = false;
        }

        /* actual_p: number of real parameters encountered (keywords don't count). */
        uint8_t actual_p = 0;
        for (uint32_t i = 0; i < n_params; i++) {
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
                    /* CT0/RT1: a `k : #refine{...}` class parameter -- peel to
                     * the base type and remember the predicate.  This is the
                     * branch the spaced-colon form actually takes; the two
                     * nested-vector sites below handle `[k : T]`. */
                    param_types[actual_p - 1] = *rt_peel_contract(
                        ft, &param_refine_preds[actual_p - 1],
                        &param_refine_vars[actual_p - 1]);
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
                        param_types[actual_p] = *rt_peel_contract(
                            ft, &param_refine_preds[actual_p],
                            &param_refine_vars[actual_p]);
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
                    param_types[actual_p] = *rt_peel_contract(
                        ft, &param_refine_preds[actual_p],
                        &param_refine_vars[actual_p]);
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
    const Form *return_refine_pred = NULL;   /* RT1: `: #refine{ r : T | q }` */
    const char *return_refine_var  = NULL;
    uint32_t ret_idx = 2;   /* first element after params vector */
    if (method_form->as.list.len > ret_idx) {
        Form *maybe_row = method_form->as.list.items[ret_idx];
        if (maybe_row->tag == F_MAP) {
            /* #{Effect...} effect-row annotation -- parse and store it. */
            warn_legacy_fx_row(maybe_row);
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
            warn_legacy_fx_row(ret_form);
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
                 * instance must be selected from the call's expected type.
                 * M7 HKT (flag-gated): match against the EFFECTIVE type params
                 * (class params + method-level element tyvars) so a bare
                 * element return `: a` -- the Comonad `extract [w : (f a)] : a`
                 * / Foldable shape -- resolves to a named TY_TYVAR instead of
                 * erroring.  eff_tp == class_type_params when the flag is off,
                 * so this is byte-for-byte inert flag-off. */
                const Symbol *tp = class_type_param_match(kw->name, kw->len,
                                                          eff_tp, n_eff_tp);
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
                        /* Bare nominal return type: a single-symbol `: T` naming
                         * an ordinary user-defined type (defopaque newtype or
                         * defstruct) is neither a builtin keyword, a class type
                         * param, nor an associated-type name -- but it is still a
                         * legitimate return type.  The applied-form path
                         * (`(Result T cstr)`, `(Vec T)`) already routes through
                         * type_expr_from_form; route the bare symbol through the
                         * same resolver before giving up.  Synthesize an F_SYM
                         * form (ret_form was normalized to F_KEYWORD above) so the
                         * nominal-type lookup runs. */
                        Form *sym_f = (Form *)arena_alloc(e->arena, sizeof(Form));
                        *sym_f = *ret_form;
                        sym_f->tag = F_SYM;
                        Type *ft = type_expr_from_form(e, sym_f, NULL,
                                                       eff_tp, eff_kinds,
                                                       n_eff_tp);
                        if (ft) {
                            return_type = *ft;
                        } else {
                            diag_emit(DIAG_ERROR, ret_form->span,
                                      "unsupported return type in typeclass method");
                            return NULL;
                        }
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
            /* RT1: `: #refine{ r : T | q }` on a class method result.  Peel to
             * the base type -- otherwise the contract type reaches codegen and
             * the method does not compile -- and KEEP the predicate, which is
             * the class's promise to callers.  Dropping it silently (which is
             * what happened before) produced a class signature that read like a
             * guarantee and enforced nothing. */
            return_type = *rt_peel_contract(ft, &return_refine_pred,
                                            &return_refine_var);
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
    /* arena_alloc does not zero.  Zero the whole struct before the field
     * assignments so any member NOT set below (refine_class_binding was the
     * one that bit: the RT1 memo slot read junk from a recycled slab and a
     * later dynamic dispatch dereferenced it -- the refined multi-compile
     * SIGSEGV) -- and any field added later -- starts NULL instead of
     * whatever the recycled slab held. */
    memset(method, 0, sizeof(*method));
    method->name = name;
    method->param_names = param_names;
    method->param_types = param_types;
    method->param_is_fn = param_is_fn;
    method->param_refine_preds = param_refine_preds;
    method->param_refine_vars  = param_refine_vars;
    method->param_explicit_type = param_explicit_type;
    method->n_params = n_params;
    method->return_type = return_type;
    method->return_refine_pred = return_refine_pred;
    method->return_refine_var  = return_refine_var;
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
        for (uint32_t j = 0; j < em->n_params; j++) {
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

    /* assoc-types-2 (Part A / MP2): optional functional-dependency clause
     * `| (from... -> to...)` immediately after the type-param vector.  The `|`
     * is a bare symbol; the following form is a parenthesized list with a `->`
     * separating the determining (from) names from the determined (to) names.
     * Example: (defclass Collect [c e] | (c -> e) ...).  A class with no `|`
     * clause has has_fundep == false (every parameter must be fixed at the
     * dispatch site). */
    bool     fundep_has       = false;
    uint16_t fundep_from_mask = 0;
    uint16_t fundep_to_mask   = 0;
    if (methods_start < call->as.list.len) {
        Form *bar = call->as.list.items[methods_start];
        if (bar->tag == F_SYM && strcmp(bar->as.sym->name, "|") == 0) {
            if (methods_start + 1 >= call->as.list.len ||
                call->as.list.items[methods_start + 1]->tag != F_LIST) {
                diag_emit(DIAG_ERROR, bar->span,
                          "functional dependency '|' must be followed by a "
                          "(from... -> to...) list");
                return NULL;
            }
            Form *fd = call->as.list.items[methods_start + 1];
            /* Locate the '->' separator within the fundep list. */
            int arrow_at = -1;
            for (uint32_t i = 0; i < fd->as.list.len; i++) {
                Form *t = fd->as.list.items[i];
                if (t->tag == F_SYM && strcmp(t->as.sym->name, "->") == 0) {
                    arrow_at = (int)i;
                    break;
                }
            }
            if (arrow_at < 0) {
                diag_emit(DIAG_ERROR, fd->span,
                          "functional dependency requires '->' (e.g. (c -> e))");
                return NULL;
            }
            /* Map a fundep name to its type-parameter index, setting the bit. */
            for (uint32_t i = 0; i < fd->as.list.len; i++) {
                if ((int)i == arrow_at) continue;
                Form *t = fd->as.list.items[i];
                if (t->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, t->span,
                              "functional dependency entries must be type-parameter names");
                    return NULL;
                }
                int idx = -1;
                for (uint8_t p = 0; p < n_type_params; p++) {
                    if (type_params[p] && type_params[p] == t->as.sym) { idx = p; break; }
                }
                if (idx < 0) {
                    diag_emit(DIAG_ERROR, t->span,
                              "functional dependency names unknown type parameter '%s'",
                              t->as.sym->name);
                    return NULL;
                }
                if ((int)i < arrow_at) fundep_from_mask |= (uint16_t)(1u << idx);
                else                   fundep_to_mask   |= (uint16_t)(1u << idx);
            }
            if (fundep_from_mask == 0 || fundep_to_mask == 0) {
                diag_emit(DIAG_ERROR, fd->span,
                          "functional dependency needs at least one parameter on "
                          "each side of '->'");
                return NULL;
            }
            if (fundep_from_mask & fundep_to_mask) {
                diag_emit(DIAG_ERROR, fd->span,
                          "functional dependency 'from' and 'to' parameters must be disjoint");
                return NULL;
            }
            fundep_has = true;
            methods_start += 2;
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
    /* assoc-types-2 (MP2): record the functional dependency (if any). */
    tc->has_fundep        = fundep_has;
    tc->fundep_from_mask  = fundep_from_mask;
    tc->fundep_to_mask    = fundep_to_mask;
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
            case TY_TYVAR:
                /* structdef-retirement slice 5 B2 (P3): an unresolved instance
                 * head (an unknown name like `option`/`vec`) is now a named
                 * TY_TYVAR carried with its source symbol in type_arg_syms,
                 * exactly as the old def-less TY_STRUCT was.  Mangle by that
                 * name so two distinct unknown-name instances of the same class
                 * (TestFunctor[option] vs TestFunctor[vec]) get distinct
                 * suffixes -- without this both collapse to `_T` and the
                 * idempotent re-instance guard swallows the second. */
                if (type_arg_syms && type_arg_syms[j]) {
                    uint32_t sym_len = type_arg_syms[j]->len;
                    if (sym_len >= sizeof(ctor_name_buf))
                        sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                    memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                    ctor_name_buf[sym_len] = '\0';
                    tur_mangle_ident(ctor_name_buf, ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else if (type_args[j].as.tyvar_.name) {
                    tur_mangle_ident(type_args[j].as.tyvar_.name,
                                     ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else {
                    type_component = "T";
                }
                break;
            case TY_ADT:
                /* CONV-S1 (defstruct-as-defadt): a record-ADT head -- a lowered
                 * `defstruct` or a hand-written single-variant `(defdata T ...)` --
                 * mangles by its constructor name exactly as TY_STRUCT does.
                 * Without this the switch fell through to `default: "T"`, so every
                 * non-parametric ADT-headed instance of a class collapsed to the
                 * same `_T` suffix and the idempotent re-instance guard silently
                 * swallowed all but the first (e.g. `Backend [CanvasBackend]`,
                 * `[SurfaceBackend]`, `[PngBackend]` -> one surviving instance).
                 * Flag-independent: also fixes hand-written record-ADT instances. */
                if (type_arg_syms && type_arg_syms[j]) {
                    uint32_t sym_len = type_arg_syms[j]->len;
                    if (sym_len >= sizeof(ctor_name_buf))
                        sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                    memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                    ctor_name_buf[sym_len] = '\0';
                    tur_mangle_ident(ctor_name_buf, ctor_mangle_buf, sizeof(ctor_mangle_buf));
                    type_component = ctor_mangle_buf;
                } else if (type_args[j].as.adt_.def && type_args[j].as.adt_.def->name) {
                    type_component = type_args[j].as.adt_.def->name;
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
                    const Type *aarg = type_args[j].as.app.arg;
                    /* ECS E2d-P6: a parametric instance head's element is a
                     * NAMED TY_TYVAR (`A` in `(Dense A)`).  Render it honestly
                     * via type_name (`"tyvar"`); the C identifier only has to be
                     * unique per instance and stable, and the element name half
                     * is normalized so two declarations of the same parametric
                     * instance that pick different tyvar letters still mangle
                     * identically.  A concrete element keeps its real name. */
                    const char *n = type_name(*aarg);
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
        case EX_MATCH:
            /* HKT instance bodies are commonly a `match` over the receiver, with
             * each arm CONSTRUCTING the result family in-body -- e.g.
             * `(definstance Functor [ReF] (fmap [c g] (match c (EmptyF) (EmptyF)
             *  (AltF x y) (AltF (g x) (g y)) ...)))`.  Such a body is
             * by-value-constructible iff every arm's tail is.  Without this arm
             * the by-value spec is never minted and a sub-int64 / float result
             * element silently reads the wrong (carrier int64) ADT layout. */
            if (e->as.match_.n_arms == 0) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (!e->as.match_.arms[i].body ||
                    !m7_body_constructs_byvalue(e->as.match_.arms[i].body))
                    return false;
            }
            return true;
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
            /* An ADT constructor call -- `(AltF (g x) (g y))`, `(EmptyF)` -- builds
             * the result family in-body.  For a by-value (`:copy`) defdata this
             * constructs the by-value ADT layout (`ctor_*__bool`) per element under
             * the active spec, so it is by-value-constructible.  This is the shape
             * of a `match`-bodied `Functor`/`Bifunctor` instance over a sum type. */
            if (e->as.call_.ctor)
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

/* M7 HKT layer-4 (flag-gated): is this instance-method body a by-value-safe
 * BARE-ELEMENT return?  The Comonad `extract [w : (f a)] : a` / Foldable shape
 * returns a bare element (`a`, grounding to a scalar/struct), not an applied
 * `(f b)` -- so there is no `#{Construct}` to recover, and m7_body_constructs_
 * byvalue (which looks for one) correctly rejects it.  A bare-element body is
 * by-value-safe when its tail merely READS a scalar out of the (now by-value)
 * receiver -- a field access `(.value w)` -- or returns a bare element binding
 * directly.  It is NOT safe if it passes the whole `(f a)` receiver to a carrier
 * helper, so we admit only field reads / bare-element vars (recursing through
 * if/do/let to the tail), never a general call. */
static bool m7_body_returns_byvalue_element(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_IF:
            return m7_body_returns_byvalue_element(e->as.if_.then_) &&
                   m7_body_returns_byvalue_element(e->as.if_.else_or_null);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   m7_body_returns_byvalue_element(e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET:
            return m7_body_returns_byvalue_element(e->as.let_.body);
        case EX_MATCH:
            /* A bare-element body may also `match` the receiver, each arm
             * READING a scalar element by value (Comonad/Foldable shapes). */
            if (e->as.match_.n_arms == 0) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (!e->as.match_.arms[i].body ||
                    !m7_body_returns_byvalue_element(e->as.match_.arms[i].body))
                    return false;
            }
            return true;
        case EX_GET_FIELD:
            /* `(.value w)` -- a scalar field read off the by-value receiver. */
            return true;
        case EX_VAR:
            /* returns a bare element value (a param/local) directly, never the
             * applied `(f a)` receiver itself (that is the selection shape). */
            return e->type.kind != TY_APP;
        case EX_CALL:
            /* Foldable `foldr`: the tail folds via a fn PARAMETER --
             * `(g (.value t) z)` -- returning the bare element result by value.
             * Admit a local (parameter) fn callee whose result is a bare element
             * (non-applied), distinguished from a carrier-delegating global
             * helper by `!is_global`.  The receiver only ever reaches the callee
             * as an extracted scalar (`(.value t)`), never whole, so by value is
             * safe. */
            return e->as.call_.fn_binding && !e->as.call_.fn_expr &&
                   !e->as.call_.fn_binding->is_global &&
                   e->type.kind != TY_APP;
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
                uint32_t ar = decl.as.fn.arity < act.as.fn.arity
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
 * that erases to `ptr<void>` (docs/archive/history/m7-hkt-ap-fn-element-carrier-
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
                for (uint32_t i = 0; i < t.as.fn.arity; i++)
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

/* method-result-functor-inference: is this fully-ground applied result type
 * representationally the int64 carrier?  True for an opaque newtype head
 * (`(defopaque Box [a] :int)` -- n_ctors == 0, is_opaque) and a transparent
 * int-record newtype -- BOTH lower to `int64_t`, so committing the grounded
 * `(Box int)` as the call's static result type is ABI-identical to the
 * carrier the (carrier-bodied) instance method actually returns.  This lets a
 * downstream `(un-box r)` recover `f := Box` from the receiver's `(Box int)`
 * even when the instance body delegates to a carrier helper and so is NOT
 * by-value-constructible (m7_body_byvalue_ok == false).  Kept narrow to the
 * int64-carrier representations: a genuine by-value aggregate (Option/Result
 * with multiple fields) must still mint a by-value spec before its precise
 * result type may be committed, or the consumer reads the aggregate layout
 * off a carrier int64 (the carrier-vs-by-value mismatch). */
static bool m7_result_is_int_carrier(Type t) {
    if (type_is_transparent_int_newtype(t)) return true;
    if (t.kind == TY_APP) {
        AdtDef *adef = NULL;
        Type args[16];
        uint8_t na = 0;
        if (type_extract_adt_app(&t, &adef, args, &na) && adef)
            return adef->is_opaque;
    }
    if (t.kind == TY_ADT)
        return t.as.adt_.def && t.as.adt_.def->is_opaque;
    return false;
}

/* hkt-fmap-result-is-not-droppable: collapse an applied HKT result whose head is
 * a POINTER-FAMILY builtin type constructor -- `(type-app rc<?> int)` -- down to
 * the concrete `rc<int>` the rest of the compiler recognizes.
 *
 * An instance over the built-in `rc` constructor (stdlib/rc.tur's `Functor [rc]`)
 * gets its result substituted correctly to `(type-app rc<?> int)`, but nothing
 * consumed that shape: `rc/drop` and friends test for TY_RC, so the fmap result
 * could not be released and every `(fmap r f)` leaked a control block plus a
 * payload slot.  The head substitution builds a TY_APP because that is what an
 * HKT class result `(f b)` is; `rc<T>` is not spelled as a TY_APP anywhere else,
 * so the two representations have to be reconciled here.
 *
 * Sound to commit WITHOUT minting a by-value spec, for exactly the reason the
 * opaque-newtype arm beside it is: an rc/weak is an `RcControlBlock *` and a ref
 * is a plain pointer, so the by-value representation IS the int64 carrier the
 * method returns -- 8 bytes, same bits, no aggregate layout to misread.  (The
 * emitter already relies on this: TY_RC sits in emit_fns.c's typed-pointer
 * return escape-hatch list, pinned by tests/fixtures/inline-c-rc-return-typed.)
 *
 * Returns false for any other head, leaving the neighbouring arms untouched. */
static bool m7_app_to_ptr_family(Type t, Type *out) {
    if (t.kind != TY_APP || !t.as.app.fn || !t.as.app.arg) return false;
    TypeKind head = t.as.app.fn->kind;
    if (head != TY_RC && head != TY_WEAK && head != TY_REF && head != TY_LREF)
        return false;
    const Type *arg = t.as.app.arg;
    /* An aggregate element carries its def so field access through the handle
     * still resolves, mirroring type_rc_adt on the struct-field path. */
    if (head == TY_RC && arg->kind == TY_ADT && arg->as.adt_.def) {
        *out = type_rc_adt(arg->as.adt_.def);
        return true;
    }
    if (head == TY_WEAK && arg->kind == TY_ADT && arg->as.adt_.def) {
        *out = type_weak_adt(arg->as.adt_.def);
        return true;
    }
    switch (head) {
        case TY_RC:   *out = type_rc(arg->kind);   return true;
        case TY_WEAK: *out = type_weak(arg->kind); return true;
        case TY_REF:  *out = type_ref(arg->kind);  return true;
        case TY_LREF: *out = type_lref(arg->kind); return true;
        default:      return false;
    }
}

/* Parse a single instance-head argument form into a Type.  A primitive type
 * keyword (`int`, `cstr`, ...) or a known struct/ADT name resolves to its
 * concrete type; any other bare name is treated as a head *type variable*
 * (e.g. `V` in `(Map cstr V)`), recorded as a named TY_TYVAR so its identity
 * survives to the dispatch site where it unifies against the receiver's
 * matching slot.  Mirrors the inline arg parser in the single-`_` partial-
 * application path.  Returns false (after emitting a diagnostic) when the form
 * is not a symbol or keyword. */
static bool parse_instance_head_arg(Elab *e, const Form *f, Type *out) {
    if (f->tag != F_SYM && f->tag != F_KEYWORD) {
        diag_emit(DIAG_ERROR, f->span,
                  "type application argument must be a type keyword or symbol");
        return false;
    }
    const Symbol *akw = f->as.sym;
    if (akw->len == 3 && memcmp(akw->name, "int", 3) == 0) {
        *out = TYPE_INT; return true;
    }
    if (akw->len == 4 && memcmp(akw->name, "bool", 4) == 0) {
        *out = TYPE_BOOL; return true;
    }
    if (akw->len == 4 && memcmp(akw->name, "cstr", 4) == 0) {
        *out = TYPE_CSTR; return true;
    }
    if ((akw->len == 4 && memcmp(akw->name, "void", 4) == 0) ||
        (akw->len == 3 && memcmp(akw->name, "nil", 3) == 0)) {
        *out = TYPE_NIL; return true;
    }
    TypeKind ank = typekind_from_symbol(akw->name);
    if (ank != TY_UNKNOWN) {
        *out = type_simple(ank, CK_COPY); return true;
    }
    Binding *asb = scope_lookup(e->scope, akw);
    if (asb && asb->type.kind == TY_ADT && asb->type.as.adt_.def) {
        *out = asb->type; return true;
    }
    *out = type_tyvar_named(akw->name);
    return true;
}

/* Resolve a PRIMITIVE type name appearing in a `definstance` constraint
 * (`[TC float]`, `[(TC cstr)]`).  Returns false for anything that is not a
 * built-in scalar so the caller can go on to try type parameters and
 * user-defined type names.
 *
 * This is the same name set `parse_instance_head_arg` accepts, factored out so
 * the constraint parsers cannot drift back to recognising a hand-written subset
 * of it -- they used to accept exactly `int`/`bool`/`cstr` and silently keep
 * their `TYPE_INT` initializer for every other spelling, which made `[TC float]`
 * mean `[TC int]`.  See docs/archive/definstance-constraint-type-defaults-to-int.md. */
static bool constraint_prim_type(const Symbol *kw, Type *out) {
    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0)  { *out = TYPE_INT;  return true; }
    if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) { *out = TYPE_BOOL; return true; }
    if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) { *out = TYPE_CSTR; return true; }
    if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
        (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) { *out = TYPE_NIL; return true; }
    if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) { *out = TYPE_PTR_VOID; return true; }
    TypeKind nk = typekind_from_symbol(kw->name);
    if (nk != TY_UNKNOWN) { *out = type_simple(nk, CK_COPY); return true; }
    return false;
}

/* Resolve a USER-DEFINED type name appearing in a `definstance` constraint
 * (`[TC MyStruct]`).  Mirrors the instance head's own resolution: the type
 * namespace first (so the owning module is credited), then the value binding,
 * which is where a lowered `defstruct` whose constructor shadows the type name
 * is found.  Returns false when the name is not a known type -- the caller
 * reports that rather than defaulting. */
/* The ADT a `definstance` head argument is built from.  A bare head (`[Cons]`)
 * is the ADT itself; an applied head (`[(Option A)]`, `[(Map cstr V)]`) is a
 * TY_APP spine whose innermost `fn` is the constructor.  Used to decide whether
 * a constraint variable names one of the head's type parameters. */
static AdtDef *constraint_head_adt(const Type *t) {
    while (t && t->kind == TY_APP) t = t->as.app.fn;
    if (t && t->kind == TY_ADT) return t->as.adt_.def;
    return NULL;
}

static bool constraint_named_type(Elab *e, const Symbol *kw, Type *out) {
    Type *ty = elab_lookup_type_by_name(e, kw);
    if (ty && ty->kind == TY_ADT && ty->as.adt_.def) { *out = *ty; return true; }
    Binding *b = scope_lookup(e->scope, kw);
    if (b && b->type.kind == TY_ADT && b->type.as.adt_.def) { *out = b->type; return true; }
    return false;
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
    /* M7 partial-app wildcard head: the `_` hole slot index parsed from a
     * `(Ctor _ B)` / `(Ctor A _)` instance head, recorded onto the instance
     * so the by-value HKT grounding can fix the non-hole slots. 0xFF = none. */
    uint8_t hkt_hole_pos = 0xFF;
    
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
                                /* The instance head names a TYPE.  scope_lookup is
                                 * value-preferring, and for a single-variant record
                                 * ADT whose constructor shares the type's name --
                                 * every lowered `defstruct`, and a hand-written
                                 * `(defdata T [A] (T [...]))` -- it returns the
                                 * constructor FUNCTION (TY_FN), shadowing the type.
                                 * Consult the authoritative type-namespace lookup
                                 * to recover the ADT/struct type in that case.  We
                                 * still prefer a struct binding from scope_lookup
                                 * first, preserving the GADT/struct coexistence
                                 * (MF4) struct-preference. */
                                /* structdef-retirement DS-C: the TY_STRUCT
                                 * instance-head branches (scope binding and type
                                 * lookup) are dead -- a defstruct head is a
                                 * TY_ADT now.  Resolve via the type namespace
                                 * first (a defdata/defgadt/lowered-defstruct type
                                 * constructor -- preferred so the orphan check
                                 * credits the owning module), then the value
                                 * binding, else an unresolved tyvar. */
                                Type *head_ty = elab_lookup_type_by_name(e, kw);
                                if (head_ty && head_ty->kind == TY_ADT &&
                                    head_ty->as.adt_.def) {
                                    type_args[i] = *head_ty;
                                    type_arg_syms[i] = kw;
                                } else if (sb && sb->type.kind == TY_ADT && sb->type.as.adt_.def) {
                                    type_args[i] = sb->type;
                                    type_arg_syms[i] = kw;
                                } else {
                                    /* Phase HKT H3 / P3: unknown name -- an
                                     * unresolved type variable.  Emit a named
                                     * TY_TYVAR marked as an opaque kind-'* -> *'
                                     * constructor (a genuine kind-* tyvar keeps
                                     * hkt_kind == KIND_STAR, so the kind checker
                                     * can tell them apart); the symbol is tracked
                                     * via type_arg_syms[i] for method mangling. */
                                    type_args[i] = type_tyvar_named(kw->name);
                                    type_args[i].copy_kind = CK_MOVE;
                                    type_args[i].hkt_kind = KIND_ARROW;
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
                         * docs/archive/history/result-param-order-blocks-functor-monad.md. */
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
                            /* Fully-applied 2-parameter head `(Ctor a b)` with NO
                             * `_` hole: BOTH arguments are bound -- each a concrete
                             * type or a head type variable (e.g. `cstr` and `V` in
                             * `(Map cstr V)`).  Build a nested, fully-applied TY_APP
                             * -- app(app(Ctor, a), b) -- of kind *, so a kind-*
                             * class stays kind * while a head tyvar binds to the
                             * receiver's matching slot at dispatch
                             * (m7_collect_tyvar_bindings unifies the nested head
                             * against the concrete receiver).  This is the kind-*
                             * counterpart to the single-`_` partial-application
                             * path below, which serves kind-(* -> *) classes.  See
                             * docs/archive/history/kind-star-instance-two-param-type-cannot-bind-constraint-var.md */
                            if (!h1_hole && !h2_hole) {
                                if (ctor_form->tag != F_SYM &&
                                    ctor_form->tag != F_KEYWORD) {
                                    diag_emit(DIAG_ERROR, ctor_form->span,
                                              "type application constructor must be a symbol");
                                    return NULL;
                                }
                                const Symbol *ctor_sym2 = ctor_form->as.sym;
                                Type a0, a1;
                                if (!parse_instance_head_arg(e, h1, &a0)) return NULL;
                                if (!parse_instance_head_arg(e, h2, &a1)) return NULL;
                                /* Constructor fn type carries its real def +
                                 * arity-derived kind (so dispatch matching and the
                                 * orphan check see the right constructor identity);
                                 * an unresolved ctor falls back to an opaque binary
                                 * constructor. */
                                Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                                memset(fn_type, 0, sizeof(Type));
                                Binding *ctor_b2 = scope_lookup(e->scope, ctor_sym2);
                                /* CONV-S1 (byvalue-field-ascribed-carrier-receiver):
                                 * scope_lookup is value-preferring, so for a lowered
                                 * record ADT (and any `(defdata T [A] (T [...]))`)
                                 * the type name `Duo` resolves to the constructor
                                 * FUNCTION (TY_FN), shadowing the type.  The else
                                 * branch then built a def-less TY_STRUCT base, so
                                 * `(.snd x)` on a `(Duo cstr int)` receiver in the
                                 * instance body unwrapped the app to a def-less
                                 * struct and the field never resolved.  Recover the
                                 * ADT/struct type from the type namespace, mirroring
                                 * the single-arg keyword head path above (struct
                                 * from scope_lookup still wins first, preserving the
                                 * MF4 struct/GADT coexistence preference). */
                                Type *head_ty2 = elab_lookup_type_by_name(e, ctor_sym2);
                                if (ctor_b2 && ctor_b2->type.kind == TY_ADT) {
                                    *fn_type = ctor_b2->type;
                                    uint32_t ca = ctor_b2->type.as.adt_.def->n_type_params;
                                    fn_type->hkt_kind = (ca > 0)
                                        ? kind_for_arity(ca) : KIND_ARROW2;
                                } else if (head_ty2 && head_ty2->kind == TY_ADT &&
                                           head_ty2->as.adt_.def) {
                                    *fn_type = *head_ty2;
                                    uint32_t ca = head_ty2->as.adt_.def->n_type_params;
                                    fn_type->hkt_kind = (ca > 0)
                                        ? kind_for_arity(ca) : KIND_ARROW2;
                                } else {
                                    /* structdef-retirement slice 5 B2: an
                                     * unresolved partial-app constructor head is
                                     * a named TY_TYVAR (kind '* -> * -> *' in
                                     * hkt_kind) rather than a def-less
                                     * TY_STRUCT; the name is carried for dispatch
                                     * / orphan-check identity. */
                                    *fn_type = type_tyvar_named(ctor_sym2->name);
                                    fn_type->copy_kind = CK_MOVE;
                                    fn_type->hkt_kind = KIND_ARROW2;
                                }
                                /* inner = app(Ctor, a0);  outer = app(inner, a1).
                                 * Each application steps the kind one rung down the
                                 * ladder, so a binary (ARROW2) constructor applied
                                 * to two args lands at kind * -- keeping a kind-*
                                 * class from being promoted to higher kind. */
                                Type *a0p = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *a0p = a0;
                                Type *a1p = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *a1p = a1;
                                Type *inner = (Type *)arena_alloc(e->arena, sizeof(Type));
                                memset(inner, 0, sizeof(Type));
                                inner->kind = TY_APP;
                                inner->copy_kind = CK_MOVE;
                                inner->hkt_kind = kind_apply_one(fn_type->hkt_kind);
                                inner->as.app.fn = fn_type;
                                inner->as.app.arg = a0p;
                                memset(&type_args[i], 0, sizeof(type_args[i]));
                                type_args[i].kind = TY_APP;
                                type_args[i].copy_kind = CK_MOVE;
                                type_args[i].hkt_kind = kind_apply_one(inner->hkt_kind);
                                type_args[i].as.app.fn = inner;
                                type_args[i].as.app.arg = a1p;
                                type_arg_syms[i] = ctor_sym2;
                                continue;
                            }
                            if (h1_hole == h2_hole) {
                                diag_emit(DIAG_ERROR, arg->span,
                                          "instance head has two '_' holes; exactly "
                                          "one parameter may be left free (e.g. (Result _ B))");
                                return NULL;
                            }
                            /* The fixed (non-hole) arm is the type argument. */
                            aarg_form = h1_hole ? h2 : h1;
                            /* Record the hole slot index (0 = first ctor param,
                             * 1 = second) so the by-value HKT grounding fixes the
                             * other slot from the concrete receiver. */
                            hkt_hole_pos = h1_hole ? 0 : 1;
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
                                } else if (asb && asb->type.kind == TY_ADT &&
                                           asb->type.as.adt_.def) {
                                    app_arg_type = asb->type;
                                } else {
                                    /* Unknown name in an applied instance head
                                     * (`A` in `(Dense A)`) is the instance's own
                                     * type parameter -- a type *variable*, not a
                                     * concrete opaque struct.  Record it as a
                                     * NAMED TY_TYVAR (the same representation
                                     * type_expr_from_form uses for class/sig type
                                     * params) so its identity survives to the
                                     * call site: the parametric associated-type
                                     * projection (`(type Elem = A)`) and the
                                     * abi-binding grounding both unify this head
                                     * tyvar against the concrete receiver
                                     * (`(Dense Pos)` => `A -> Pos`).  A nameless
                                     * null-def struct here would erase `A` and
                                     * carrier-collapse a struct element.  Many
                                     * dispatch sites already accept either shape
                                     * (the open-binder-skolems named-TYVAR
                                     * migration); a concrete head arg still
                                     * resolves to its real def via the branches
                                     * above. */
                                    app_arg_type = type_tyvar_named(akw->name);
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
                        /* The applied head names a TYPE constructor.  scope_lookup
                         * is value-preferring and for a single-variant record ADT
                         * whose constructor shares the type's name (every lowered
                         * defstruct -- e.g. `(Result _ B)` once Result lowers)
                         * returns the constructor FUNCTION (TY_FN), so the
                         * TY_ADT/TY_STRUCT branch below would miss and fall to the
                         * opaque `<struct>` fallback -- erasing the ADT identity and
                         * breaking `.is-ok` field access in the method body.  Resolve
                         * the head type authoritatively via the type namespace,
                         * preferring a struct binding from scope_lookup first (MF4
                         * struct-preference). */
                        Type head_ct;
                        bool have_head_ct = false;
                        {
                            Type *ht = elab_lookup_type_by_name(e, ctor_sym);
                            if (ht && ht->kind == TY_ADT && ht->as.adt_.def) {
                                head_ct = *ht; have_head_ct = true;
                            } else if (ctor_b && ctor_b->type.kind == TY_ADT) {
                                head_ct = ctor_b->type; have_head_ct = true;
                            }
                        }
                        /* The constructor's kind follows its real arity: a unary
                         * constructor (Option : * -> *) applied to one arg is a
                         * fully-applied type of kind *, while a binary one
                         * (Result : * -> * -> *) applied to one arg is still a
                         * (* -> *) constructor.  Derive the kind from the def's
                         * n_type_params when known; fall back to the legacy binary
                         * assumption only for an opaque (def == NULL) constructor. */
                        if (have_head_ct) {
                            *fn_type = head_ct;
                            uint32_t ctor_arity = head_ct.as.adt_.def->n_type_params;
                            fn_type->hkt_kind = (ctor_arity > 0)
                                ? kind_for_arity(ctor_arity)
                                : KIND_ARROW2;
                        } else {
                            /* structdef-retirement slice 5 B2: an unresolved
                             * partial-app constructor head is a named TY_TYVAR
                             * (kind '* -> * -> *' in hkt_kind) rather than a
                             * def-less TY_STRUCT; carry the name for dispatch /
                             * orphan-check identity. */
                            *fn_type = type_tyvar_named(ctor_sym->name);
                            fn_type->copy_kind = CK_MOVE;
                            fn_type->hkt_kind = KIND_ARROW2;
                        }
                        /* Build arg type on arena */
                        Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *arg_type_ptr = app_arg_type;
                        /* Assemble TY_APP */
                        memset(&type_args[i], 0, sizeof(type_args[i]));
                        type_args[i].kind = TY_APP;
                        type_args[i].copy_kind = CK_MOVE;
                        /* Result kind = constructor kind with one arg applied
                         * (ARROW2 -> ARROW for a binary head; ARROW -> STAR for a
                         * fully-applied unary head). */
                        type_args[i].hkt_kind = kind_apply_one(fn_type->hkt_kind);
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
                        if (type_args[i].kind == TY_TYVAR) {
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
     * docs/archive/history/m5-suite-residual-6-failures-2026-06-14.md (root cause A). */
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
                            /* A keyword spelling (`[TC :cstr]`) names a type
                             * exactly as the instance head's own parser
                             * accepts it.  Anything else is not a type at
                             * all -- reported below rather than left on the
                             * `TYPE_INT` initializer. */
                            if (type_arg_form->tag == F_SYM ||
                                type_arg_form->tag == F_KEYWORD) {
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
                                if (!found)
                                    found = constraint_prim_type(type_param_name,
                                                                 &constrained_type);
                                /* CONV-S2: under defstruct-as-defadt the instance
                                 * head is a lowered record ADT (`[Cons]`/`[Vec]`),
                                 * so the constraint var (`A` in `[(Tag A)]`) is one
                                 * of the ADT's type params, not a struct's.  Mirror
                                 * the struct lookup so its param_idx is recorded;
                                 * without it p_idx stays -1 and the emit-side
                                 * constraint-var->element mapping (a helper call or
                                 * lifted closure inside the instance body) never
                                 * grounds A, baking the carrier representative. */
                                if (!found) {
                                    for (uint8_t j = 0; j < n_type_args && p_idx < 0; j++) {
                                        /* An APPLIED head (`[(Option A)]`) is a
                                         * TY_APP over the same ADT, and binds
                                         * its type params exactly as a bare one
                                         * does -- peel to the constructor so `A`
                                         * is recognised there too, instead of
                                         * falling through to the type-name
                                         * lookup and being reported unknown. */
                                        AdtDef *adef = constraint_head_adt(&type_args[j]);
                                        if (adef) {
                                            for (uint8_t k = 0; k < adef->n_type_params; k++) {
                                                if (adef->type_params[k] &&
                                                    strcmp(adef->type_params[k],
                                                           type_param_name->name) == 0) {
                                                    p_idx = (int8_t)k;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                /* A user-defined type name (`[TC MyStruct]`).
                                 * Resolved last so a name that is also a head
                                 * type arg or an ADT type parameter keeps
                                 * meaning the parameter, as it did before. */
                                if (!found && p_idx < 0)
                                    found = constraint_named_type(e, type_param_name,
                                                                  &constrained_type);
                                /* Never default silently: a constraint type we
                                 * cannot resolve used to keep the `TYPE_INT`
                                 * initializer, so the constraint was checked --
                                 * or silently satisfied -- against a type that
                                 * appears nowhere in the source. */
                                if (!found && p_idx < 0) {
                                    diag_emit(DIAG_ERROR, type_arg_form->span,
                                              "definstance: constraint type '%s' is not a known "
                                              "type or type parameter",
                                              type_param_name->name);
                                    return NULL;
                                }
                            } else {
                                diag_emit(DIAG_ERROR, type_arg_form->span,
                                          "definstance: constraint type must be a type name, "
                                          "got a %s literal",
                                          form_tag_name(type_arg_form->tag));
                                return NULL;
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
                            /* A keyword spelling (`[TC :cstr]`) names a type
                             * exactly as the instance head's own parser
                             * accepts it.  Anything else is not a type at
                             * all -- reported below rather than left on the
                             * `TYPE_INT` initializer. */
                            if (type_arg_form->tag == F_SYM ||
                                type_arg_form->tag == F_KEYWORD) {
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
                                if (!found)
                                    found = constraint_prim_type(type_param_name,
                                                                 &constrained_type);
                                /* CONV-S2: lowered record ADT instance head -- see
                                 * the paren-format block above. */
                                if (!found) {
                                    for (uint8_t j = 0; j < n_type_args && p_idx < 0; j++) {
                                        /* An APPLIED head (`[(Option A)]`) is a
                                         * TY_APP over the same ADT, and binds
                                         * its type params exactly as a bare one
                                         * does -- peel to the constructor so `A`
                                         * is recognised there too, instead of
                                         * falling through to the type-name
                                         * lookup and being reported unknown. */
                                        AdtDef *adef = constraint_head_adt(&type_args[j]);
                                        if (adef) {
                                            for (uint8_t k = 0; k < adef->n_type_params; k++) {
                                                if (adef->type_params[k] &&
                                                    strcmp(adef->type_params[k],
                                                           type_param_name->name) == 0) {
                                                    p_idx = (int8_t)k;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                /* A user-defined type name (`[TC MyStruct]`).
                                 * Resolved last so a name that is also a head
                                 * type arg or an ADT type parameter keeps
                                 * meaning the parameter, as it did before. */
                                if (!found && p_idx < 0)
                                    found = constraint_named_type(e, type_param_name,
                                                                  &constrained_type);
                                /* Never default silently: a constraint type we
                                 * cannot resolve used to keep the `TYPE_INT`
                                 * initializer, so the constraint was checked --
                                 * or silently satisfied -- against a type that
                                 * appears nowhere in the source. */
                                if (!found && p_idx < 0) {
                                    diag_emit(DIAG_ERROR, type_arg_form->span,
                                              "definstance: constraint type '%s' is not a known "
                                              "type or type parameter",
                                              type_param_name->name);
                                    return NULL;
                                }
                            } else {
                                diag_emit(DIAG_ERROR, type_arg_form->span,
                                          "definstance: constraint type must be a type name, "
                                          "got a %s literal",
                                          form_tag_name(type_arg_form->tag));
                                return NULL;
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
            /* A non-parametric user-defined type (`[TC MyStruct]`) names one
             * concrete type, so its instance can be looked up here on the same
             * terms as a primitive's.  A parametric one (`Vec<A>` named bare)
             * still defers -- the element type is not known yet. */
            if (!is_primitive && constrained_type.kind == TY_ADT &&
                constrained_type.as.adt_.def &&
                constrained_type.as.adt_.def->n_type_params == 0)
                is_primitive = true;

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
     * model for repeated loads.  See docs/archive/history/load-not-idempotent-typeclass.md. */
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

    /* assoc-types-2 (Part A / MP5): functional-dependency coherence.  When the
     * class declares `| (from -> to)`, two instances that agree on every `from`
     * parameter must agree on every `to` parameter -- otherwise `to` is not
     * functionally determined by `from`.  Reject the new instance if an existing
     * one shares its `from` projection but disagrees on `to`.  (An exact
     * duplicate would already have been swallowed by the idempotent guard
     * above, so any match reaching here is a genuine conflict.) */
    if (tc->has_fundep && n_type_args == tc->n_type_params) {
        for (TypeClassInstance *prev = e->typeclass_env.instances; prev; prev = prev->next) {
            if (prev->typeclass != tc || prev->n_type_args != n_type_args) continue;
            bool from_eq = true;
            for (uint8_t i = 0; i < n_type_args; i++) {
                if (!(tc->fundep_from_mask & (uint16_t)(1u << i))) continue;
                if (!type_eq(prev->type_args[i], type_args[i])) { from_eq = false; break; }
            }
            if (!from_eq) continue;
            for (uint8_t i = 0; i < n_type_args; i++) {
                if (!(tc->fundep_to_mask & (uint16_t)(1u << i))) continue;
                if (!type_eq(prev->type_args[i], type_args[i])) {
                    diag_emit(DIAG_ERROR, call->span,
                              "functional dependency violated: '%s' already has an "
                              "instance with this determining type but a different "
                              "determined type",
                              tc_name->name);
                    return NULL;
                }
            }
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
        /* Gap 1 (instance-method-return-not-unified): the declared return's
         * concrete nominal def (after Phase RT substitution), so pass 2 can
         * reject a body that yields a different nominal type.  (structdef-
         * retirement DS-D: the former StructDef ret_struct is gone -- every
         * former struct is a record ADT.) */
        const AdtDef    *ret_adt;
        /* float-register-class-returns: the declared return's TypeKind (after
         * Phase RT substitution), so pass 2 can reject a float-vs-non-float
         * (xmm-vs-GP) register-class clash in the method body. */
        TypeKind         ret_kind;
        /* carrier-aware-return-unification Phase 0: the full declared return
         * Type (after Phase RT substitution), retained so the Phase 3 classifier
         * can tell a grounded concrete commit from a carrier-participating
         * (free-tyvar / applied) return.  ret_kind / ret_adt are
         * its decomposition; ret_full keeps the whole type for that future
         * carrier-vs-committed decision. */
        Type             ret_full;
        /* carrier-aware-return-unification Phase 3: true iff the method's
         * CLASS-DECLARATION return was the class type variable (substituted to
         * this instance's concrete type at Phase RT).  Only such a return is a
         * genuine per-instance commit -- a fixed concrete class-decl return
         * (e.g. `len : int`) is a carrier slot for every instance and must stay
         * tolerant.  Combined with a grounded ret_full, this gates
         * RET_CLASS_COMMITTED for the method. */
        bool             ret_was_class_var;
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
    /* M7: record the partial-app wildcard hole slot (0xFF when absent). */
    inst->partial_hole_pos = hkt_hole_pos;

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
        /* ECS E2d-P6 (parametric associated-type element): collect the free
         * tyvar names appearing in the instance head (e.g. `A` in `(Dense A)`)
         * so a binding RHS that names one (`(type Elem = A)`) resolves to that
         * NAMED TY_TYVAR rather than a nameless null-def struct.  Without this,
         * `A` is an unknown symbol to type_expr_from_form (no type_params), the
         * binding becomes an anonymous abstract struct, and the call-site
         * projection can never correlate `Elem` with the receiver's element. */
        const Symbol *head_tv_syms[16];
        Kind head_tv_kinds[16];
        uint8_t n_head_tv = 0;
        for (uint8_t ta = 0; ta < n_type_args && n_head_tv < 16; ta++) {
            const Type *sp = &type_args[ta];
            while (sp && sp->kind == TY_APP) {
                const Type *arg = sp->as.app.arg;
                if (arg && arg->kind == TY_TYVAR && arg->as.tyvar_.name) {
                    bool dup = false;
                    for (uint8_t d = 0; d < n_head_tv; d++)
                        if (head_tv_syms[d] &&
                            strcmp(head_tv_syms[d]->name, arg->as.tyvar_.name) == 0) {
                            dup = true; break;
                        }
                    if (!dup && n_head_tv < 16) {
                        head_tv_syms[n_head_tv] = symtab_intern(e->st,
                            strslice(arg->as.tyvar_.name,
                                     (uint32_t)strlen(arg->as.tyvar_.name)));
                        head_tv_kinds[n_head_tv] = KIND_STAR;
                        n_head_tv++;
                    }
                }
                sp = sp->as.app.fn;
            }
        }
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
            Type *rt = type_expr_from_form(e, assoc_bind_forms[bi], NULL,
                                           n_head_tv > 0 ? head_tv_syms : NULL,
                                           n_head_tv > 0 ? head_tv_kinds : NULL,
                                           n_head_tv);
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
        /* RT1: the class's promise about this method's result, if any. */
        const Form *m_class_ret_pred = tc->methods[i].return_refine_pred;
        const char *m_class_ret_var  = tc->methods[i].return_refine_var;
        /* carrier-aware-return-unification Phase 3: did the class-decl return name
         * the class type variable (so the substitution below grounds it to this
         * instance's concrete type)?  Only then is the method a genuine
         * per-instance commit; a fixed concrete class-decl return stays a carrier
         * slot.  An explicit instance annotation (further below) does NOT set
         * this -- that path keeps the conservative carrier classification. */
        bool ret_was_class_var = false;

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
                    ret_was_class_var = true;
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
        /* M7 layer 0: an HKT-applied return like `(g b)` arrives as
         * TY_APP(TY_TYVAR g, TY_TYVAR b).  Substitute the HKT class param in the
         * application HEAD (`g`) with this instance's constructor (type_args[ti],
         * e.g. Option), leaving the element tyvar (`b`) abstract for per-call
         * refinement.  Result: `(Option b)`.  Only the outermost head is
         * rewritten (the common `(f a)` / `(f b)` shape). */
        else if (return_type.kind == TY_APP &&
                 return_type.as.app.fn &&
                 return_type.as.app.fn->kind == TY_TYVAR &&
                 return_type.as.app.fn->as.tyvar_.name) {
            const char *head = return_type.as.app.fn->as.tyvar_.name;
            for (uint8_t ti = 0; ti < tc->n_type_params && ti < n_type_args; ti++) {
                if (tc->type_params[ti] &&
                    strcmp(tc->type_params[ti]->name, head) == 0) {
                    /* M7 partial-app wildcard head (`(Result _ B)`): rebuild the
                     * full ctor application so the result element `b` lands in the
                     * HOLE slot and the fixed arm is a tyvar named after the
                     * struct param (grounded at the call site).  A naive head
                     * subst would give `((Result B) b)` -- wrong slot order, and
                     * the opaque fixed arm collapses the result to the carrier. */
                    {
                        Type *new_fn = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *new_fn = type_args[ti];
                        return_type.as.app.fn = new_fn;
                    }
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
        /* RT1: the refinement this impl promises about its RESULT.  Starts as
         * the class's promise, which an impl that writes a plain return type
         * INHERITS -- otherwise a class could declare a guarantee that no
         * instance ever enforces. */
        const Form *impl_ret_pred = m_class_ret_pred;
        const char *impl_ret_var  = m_class_ret_var;
        bool        impl_ret_pred_own = false;
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
            /* RT1: `: #refine{ r : T | q }` on an impl result.  This branch did
             * not exist, so the annotation fell through to the body and came
             * back as "type annotation ': type' is only valid after a parameter
             * name or as a return type" -- a class could declare a result
             * refinement that an instance was syntactically forbidden to
             * restate.  Peel to the base type and keep the predicate. */
            if (!kw && ret_or_body->tag == F_TYPE_ANN &&
                ret_or_body->as.list.len == 1 &&
                ret_or_body->as.list.items[0]->tag == F_CONTRACT_TYPE) {
                Type *ft = type_expr_from_form(e, ret_or_body->as.list.items[0],
                                               NULL, NULL, NULL, 0);
                if (ft) {
                    ret_was_class_var = false;
                    const Form *rp = NULL; const char *rv = NULL;
                    return_type = *rt_peel_contract(ft, &rp, &rv);
                    if (rp) {
                        impl_ret_pred = rp;
                        impl_ret_var  = rv;
                        impl_ret_pred_own = true;
                    }
                    impl_body_start = 3;
                }
            }
            if (kw) {
                /* carrier-aware-return-unification Phase 3: an explicit instance
                 * return annotation replaces the substituted class-var return, so
                 * it is no longer the "class type variable grounded for this
                 * instance" commit.  Conservatively drop back to the carrier
                 * classification rather than reason about annotation-vs-class-var
                 * agreement. */
                ret_was_class_var = false;
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
        /* CT0/CT1: contract-typed parameters of this instance method.  Collected
         * while the annotations are resolved, injected as entry checks once the
         * body exists -- the same treatment a `defn` or `fn` parameter gets. */
        const Form *m_ct_preds[MAX_FN_ARITY];
        const char *m_ct_vars[MAX_FN_ARITY];
        uint32_t    m_ct_param_idx[MAX_FN_ARITY];
        uint32_t    n_m_ct_preds = 0;
        /* RT1: which parameters the INSTANCE annotated for itself.  An
         * unannotated parameter inherits the class's refinement, exactly as an
         * unannotated result inherits the class's promise; writing an explicit
         * annotation -- including a bare `: int`, which carries no predicate --
         * is how an instance opts out and demands less.  Without this an
         * omitted annotation inherited NOTHING, so a class demand was enforced
         * only by an instance that happened to restate it. */
        bool m_param_annotated[MAX_FN_ARITY];
        memset(m_param_annotated, 0, sizeof(m_param_annotated));
        
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
                        if (prev < MAX_FN_ARITY) m_param_annotated[prev] = true;
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
                            /* CT0: peel a contract annotation to its base type and
                             * keep the predicate for the entry check below.
                             * Without this the parameter's type stayed the
                             * contract type and the method body could not use
                             * the value at all. */
                            const Form *ct_pred = NULL;
                            const char *ct_var  = NULL;
                            ann = rt_peel_contract(ann, &ct_pred, &ct_var);
                            if (ct_pred && n_m_ct_preds < MAX_FN_ARITY) {
                                m_ct_preds[n_m_ct_preds]    = ct_pred;
                                m_ct_vars[n_m_ct_preds]     = ct_var;
                                m_ct_param_idx[n_m_ct_preds] = prev;
                                n_m_ct_preds++;
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
                        /* M7 fix direction 1: box any fn that sits in
                         * HKT-element position of the body param type, so
                         * calling an HKT-wrapped function (`((.value ff) x)` in
                         * the Applicative `ap` shape) fat-dispatches through the
                         * box instead of bare-calling the box address. */
                        elab_param_type = m7_box_hkt_element_fns(e->arena,
                                                                 elab_param_type);
                        /* The substituted full type (elab_param_type) is what the
                         * method body sees; lower the ABI/signature type to the
                         * int64 carrier for applied/parametric types, matching the
                         * dispatch ABI used for concrete instances elsewhere. */
                        /* hkt-foldable-rc-param: an instance over a pointer-family
                         * builtin sees its receiver as the applied `(t a)` ->
                         * `(type-app rc<?> tyvar 'a')`, which unifies with nothing
                         * -- so an instance body could not pass its own receiver to
                         * anything typed `rc<A>` ("expected rc<?>, got (type-app
                         * rc<?> tyvar 'a')") and had to reach for inline-C.
                         * Collapse it to the concrete `rc<a>` the rest of the
                         * compiler recognizes, the parameter-side mirror of the
                         * result-side collapse in the dispatch path.
                         *
                         * The ABI/signature type stays TYPE_INT: this changes only
                         * what the BODY sees, never the dict's uniform carrier
                         * calling convention. */
                        Type ptr_family_param;
                        if (m7_app_to_ptr_family(elab_param_type, &ptr_family_param)) {
                            elab_param_type = ptr_family_param;
                            param_type = TYPE_INT;
                        } else if (elab_param_type.kind == TY_APP) {
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
                        if (elab_param_type.kind == TY_APP) {
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
                    /* M7 capturing-closure gate: a typed `(fn [a] b)` element param
                     * (the mapper handed to fmap/bimap/ap/<|>) must use the
                     * `tur_poly_fn_t` {env, fn} carrier -- like the regular defn
                     * path -- so a CAPTURING closure's env survives `(g x)` instead
                     * of being dropped by a bare raw-fn-pointer call (segfault).
                     * SCOPED to element fns whose RESULT is a plain element (a bare
                     * tyvar `b`): a continuation returning an HKT-applied `(m b)`
                     * (Monad `bind`, Traversable `traverse`) unpacks its wrapped
                     * result through the carrier and regresses under the
                     * tur_poly_fn_t switch, so it stays as-is. */
                    if (!param_is_poly &&
                        param_type.kind == TY_FN) {
                        param_is_poly = true;
                    }
                    Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                    if (p->tag == F_SYM) {
                        /* Simple parameter name */
                        method_params[n_method_params] = binding_new(e, p->as.sym, elab_param_type, false, false, p->span);
                        method_params[n_method_params]->is_param = true;
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
                            method_params[n_method_params]->is_param = true;
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
            if (method_params[j]->is_fat) FN_ARG_SET(fn_type.as.fn, j, FA_FAT, true);
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
        else if (return_type.kind == TY_ADT && return_type.as.adt_.def &&
                 return_type.as.adt_.def->n_type_params == 0) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* M7 layer 2: carry an HKT-applied TY_APP return (`(Option b)` after
         * layer-0 head substitution) through result_full_type so the call site
         * receives the named applied head + element tyvar to refine, instead of
         * an anonymous `(type-app ? ?)`. */
        else if (return_type.kind == TY_APP) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = return_type;
            fn_type.as.fn.result_full_type = rft;
        }
        /* ECS E2d-P6 (parametric associated-type element): a PARAMETRIC instance
         * such as `(definstance StorageOps [(Dense A)] (type Elem = A) ...)`
         * projects the method's `: Elem` return to the instance head's own tyvar
         * `A` (Phase RT's assoc-type substitution above resolves `Elem -> A`, a
         * *named* TY_TYVAR -- not a concrete struct, so the by-value branch above
         * does not fire).  Carry that named tyvar through result_full_type so the
         * call site can recover the concrete element type by instantiating `A`
         * through the receiver's bindings (`(Dense Pos)` => `A -> Pos`), exactly
         * the way emit_abi_register_call's bare-tyvar-result recovery expects a
         * NAMED generic_result.  Without the name, result_kind=TY_TYVAR lowers to
         * an anonymous tyvar that no binding can substitute, so a struct element
         * stays carrier-collapsed.  Concrete instances (`Elem = Pos`) already took
         * the by-value branch; this only adds the parametric case. */
        else if (return_type.kind == TY_TYVAR && return_type.as.tyvar_.name) {
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
        method_binding->is_instance_method = true;   /* B6: internal export, CPS-eligible */
        /* The user wrote `(definstance Eq int ...)`, not
         * `__inst_Eq_eq_qu_int` -- keep the mangled name out of every
         * human-facing symbol listing. */
        method_binding->is_synthesized = true;

        /* RT1 VARIANCE: an instance may accept MORE than its class signature
         * promises, never less.  The class signature is the contract callers
         * program against, so an instance that demands more would reject an
         * argument a generic caller was entitled to pass -- and find out at
         * run time, in the method's own entry check.
         *
         * The obligation is `class_pred(p) |- instance_pred(p)` over a fresh
         * parameter `p`, which is an ordinary query through the same seam.  A
         * class parameter with NO refinement promises nothing, so any instance
         * refinement on it is a strengthening unless the predicate is a
         * tautology -- and asking the solver is exactly how to tell those
         * apart.  Reported only on a REFUTATION; an undecidable pair keeps the
         * runtime check, as everywhere else. */
        if (n_m_ct_preds > 0) {
            for (uint32_t _ci = 0; _ci < n_m_ct_preds; _ci++) {
                uint32_t _pi = m_ct_param_idx[_ci];
                if (_pi >= n_method_params || !method_params[_pi] ||
                    !method_params[_pi]->name) continue;
                const char *pname = method_params[_pi]->name->name;

                const Form *cls_pred = NULL;
                const char *cls_var  = NULL;
                if (tc->methods[i].param_refine_preds &&
                    _pi < tc->methods[i].n_params) {
                    cls_pred = tc->methods[i].param_refine_preds[_pi];
                    cls_var  = tc->methods[i].param_refine_vars[_pi];
                }
                /* Identical predicates are the overwhelmingly common case
                 * (an instance restating its class signature); skip the
                 * solver entirely for them. */
                if (cls_pred == m_ct_preds[_ci]) continue;

                RefineEnv *venv = refine_env_new(e->arena);
                refine_env_set_resolver(venv, rt_refine_resolver(e), e);
                refine_env_declare(venv, pname,
                                   rt_sort_of_kind(method_params[_pi]->type.kind));
                if (cls_pred) refine_env_push(venv, cls_pred, cls_var, pname);

                Form *subj = form_sym(e->arena, impl_form->span,
                                      symtab_intern(e->st,
                                          strslice(pname, (uint32_t)strlen(pname))));
                char what[192];
                snprintf(what, sizeof(what),
                         "parameter '%s' of instance method '%s'", pname,
                         tc->methods[i].name ? tc->methods[i].name->name : "?");
                RefineObligation *vob = refine_collect_obligation(
                    &e->refine_obs, m_ct_preds[_ci], m_ct_vars[_ci], subj,
                    rt_sort_of_kind(method_params[_pi]->type.kind),
                    type_name(method_params[_pi]->type),
                    impl_form->span, venv,
                    arena_strdup(e->arena, what, strlen(what)), NULL);
                if (!vob) continue;
                /* Decide it silently, then ask separately for a witness.  The
                 * ordinary reporting path is wrong for this obligation: its
                 * failure is a declaration-vs-declaration inconsistency with
                 * its own diagnostic, not a `TUR-E0371` about a value. */
                vob->speculative = true;
                bool ok = refine_discharge_one(vob, e->arena);
                if (!ok && vob->vc && refine_model_search(vob->vc, e->arena)) {
                    diag_emit_with_code(DIAG_ERROR, impl_form->span,
                        TUR_E0374_REFINE_INSTANCE_STRONGER,
                        "instance method '%s' demands more of parameter '%s' "
                        "than the '%s' class signature promises",
                        tc->methods[i].name ? tc->methods[i].name->name : "?",
                        pname, tc->name ? tc->name->name : "?");
                    if (!cls_pred)
                        diag_emit(DIAG_NOTE, impl_form->span,
                                  "the class signature places no refinement on "
                                  "'%s', so callers may pass any value of its type",
                                  pname);
                }
            }
        }
        /* RT1: RESULT variance, which runs the OPPOSITE way to parameters.
         * A caller programming against the class signature relies on the
         * result predicate, so an instance must deliver at least as much as
         * the class promises: `instance_pred(r) |- class_pred(r)`.  (For
         * parameters it is `class_pred(p) |- instance_pred(p)` -- the class is
         * the hypothesis there and the goal here.)
         *
         * Only checked when the instance RESTATED a predicate.  An instance
         * that writes a plain return type inherits the class's, which cannot
         * be a weakening. */
        bool ret_variance_proved = false;
        if (impl_ret_pred_own && m_class_ret_pred &&
            impl_ret_pred != m_class_ret_pred) {
            const char *rvar = impl_ret_var ? impl_ret_var
                             : (m_class_ret_var ? m_class_ret_var : "r");
            RefineEnv *venv = refine_env_new(e->arena);
            refine_env_set_resolver(venv, rt_refine_resolver(e), e);
            refine_env_declare(venv, rvar, rt_sort_of_kind(return_type.kind));
            refine_env_push(venv, impl_ret_pred, impl_ret_var, rvar);

            Form *subj = form_sym(e->arena, impl_form->span,
                                  symtab_intern(e->st,
                                      strslice(rvar, (uint32_t)strlen(rvar))));
            char what[192];
            snprintf(what, sizeof(what), "the result of instance method '%s'",
                     tc->methods[i].name ? tc->methods[i].name->name : "?");
            RefineObligation *vob = refine_collect_obligation(
                &e->refine_obs, m_class_ret_pred, m_class_ret_var, subj,
                rt_sort_of_kind(return_type.kind), type_name(return_type),
                impl_form->span, venv,
                arena_strdup(e->arena, what, strlen(what)), NULL);
            if (vob) {
                vob->speculative = true;
                bool ok = refine_discharge_one(vob, e->arena);
                ret_variance_proved = ok;
                if (!ok && vob->vc && refine_model_search(vob->vc, e->arena)) {
                    diag_emit_with_code(DIAG_ERROR, impl_form->span,
                        TUR_E0374_REFINE_INSTANCE_STRONGER,
                        "instance method '%s' promises less about its result "
                        "than the '%s' class signature does",
                        tc->methods[i].name ? tc->methods[i].name->name : "?",
                        tc->name ? tc->name->name : "?");
                    diag_emit(DIAG_NOTE, impl_form->span,
                              "a caller programming against the class signature "
                              "relies on the class's result refinement, so an "
                              "instance must deliver at least as much");
                }
            }
        }
        /* CT0/RT1: carry this method's contract parameters on its binding.  The
         * parameters are resolved in THIS pass but the body is elaborated in
         * pass 2, so the binding is the record that spans both -- and it is
         * also where a dispatch site would look for them. */
        /* An UNANNOTATED parameter inherits the class's refinement, mirroring
         * the result direction below.  So the arrays are published whenever
         * either side has a predicate, not only when the instance restated
         * one -- otherwise inheritance would produce a predicate the entry
         * check (which reads exactly these arrays) never sees. */
        bool _inherits_any = false;
        if (tc->methods[i].param_refine_preds) {
            for (uint8_t _pi = 0; _pi < n_method_params &&
                                  _pi < tc->methods[i].n_params; _pi++) {
                if (_pi < MAX_FN_ARITY && !m_param_annotated[_pi] &&
                    tc->methods[i].param_refine_preds[_pi]) {
                    _inherits_any = true;
                    break;
                }
            }
        }
        if ((n_m_ct_preds > 0 || _inherits_any) && n_method_params > 0) {
            const Form **rp = (const Form **)arena_alloc(e->arena, n_method_params * sizeof(Form *));
            const char **rv = (const char **)arena_alloc(e->arena, n_method_params * sizeof(char *));
            const char **rn = (const char **)arena_alloc(e->arena, n_method_params * sizeof(char *));
            for (uint8_t _pi = 0; _pi < n_method_params; _pi++) {
                rp[_pi] = NULL;
                rv[_pi] = NULL;
                rn[_pi] = (method_params[_pi] && method_params[_pi]->name)
                        ? method_params[_pi]->name->name : NULL;
            }
            for (uint32_t _ci = 0; _ci < n_m_ct_preds; _ci++) {
                uint32_t _pi = m_ct_param_idx[_ci];
                if (_pi >= n_method_params) continue;
                rp[_pi] = m_ct_preds[_ci];
                rv[_pi] = m_ct_vars[_ci];
            }
            if (_inherits_any) {
                for (uint8_t _pi = 0; _pi < n_method_params &&
                                      _pi < tc->methods[i].n_params; _pi++) {
                    if (_pi >= MAX_FN_ARITY || m_param_annotated[_pi]) continue;
                    if (rp[_pi]) continue;   /* instance restated it: keep that */
                    rp[_pi] = tc->methods[i].param_refine_preds[_pi];
                    rv[_pi] = tc->methods[i].param_refine_vars
                            ? tc->methods[i].param_refine_vars[_pi] : NULL;
                }
            }
            method_binding->refine_param_preds = rp;
            method_binding->refine_param_vars  = rv;
            method_binding->refine_param_names = rn;
            method_binding->n_refine_params    = n_method_params;
        }
        /* RT1/RT4: and the result refinement -- its own if it restated one, the
         * class's otherwise.  The binding is the only record that reaches pass
         * 2, where the body exists and the check can be injected.
         *
         * Publishing it is gated on the check actually being emitted, matching
         * `rt_ret_guaranteed` on the `defn` path.  `refine_return_pred` is read
         * by RT4 as a FACT about the value a call produced, so a build that
         * strips contracts (`--no-contracts`, or a release build without
         * --keep-contracts) must not leave the fact behind after removing the
         * thing that enforced it.  Nothing reads this binding today -- a
         * dispatch does not resolve to it -- but the field's contract is
         * "published only when enforced", and a latent violation of it is a
         * trap for whoever wires the propagation up. */
        if (rt_contracts_emitted()) {
            method_binding->refine_return_pred = impl_ret_pred;
            method_binding->refine_return_var  = impl_ret_var;
            /* An instance that RESTATED its own promise only enforces that
             * one.  A dispatch site is handed the CLASS's promise (it cannot
             * know which instance runs), so the class predicate has to be
             * enforced here too -- unless the variance obligation actually
             * proved that the instance's implies it, in which case the
             * instance's own check already covers it.
             *
             * Reporting only on a refutation is right for the diagnostic and
             * not enough for this: an UNDECIDABLE pair emits no error, so
             * without the extra check the class promise would be enforced by
             * nothing while callers relied on it. */
            if (impl_ret_pred_own && m_class_ret_pred &&
                impl_ret_pred != m_class_ret_pred && !ret_variance_proved) {
                method_binding->refine_class_ret_pred = m_class_ret_pred;
                method_binding->refine_class_ret_var  = m_class_ret_var;
            }
        }
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
         * docs/archive/m4-typeclass-per-method-abi-plan.md. */
        method_fd->owner_instance = inst;

        /* Stash what pass 2 needs to elaborate this method's body. */
        passes[i].impl_form       = impl_form;
        passes[i].impl_body_start = impl_body_start;
        passes[i].method_params   = method_params;
        passes[i].n_method_params = n_method_params;
        passes[i].method_fd       = method_fd;
        passes[i].arrow_return    = arrow_return;
        passes[i].ret_adt    = (return_type.kind == TY_ADT)    ? return_type.as.adt_.def    : NULL;
        passes[i].ret_kind   = return_type.kind;
        passes[i].ret_full   = return_type;  /* Phase 0: retain the whole type */
        passes[i].ret_was_class_var = ret_was_class_var;  /* Phase 3 */
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
        if (!dup) {
            e->sig_tyvar_kinds[e->n_sig_tyvars] = KIND_STAR;
            e->sig_tyvars[e->n_sig_tyvars++] = nm;
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

        /* CT1: inject this instance method's parameter contract checks, while
         * the method scope is still current (the predicate elaborates in it).
         * Shared with `defn` and `fn` so a refined method parameter is enforced
         * rather than decorative. */
        {
            const Binding *mb = mp->method_fd ? mp->method_fd->binding : NULL;
            if (mb && mb->refine_param_preds && method_body && rt_contracts_emitted()) {
                /* The arrays are indexed by parameter, with NULL where there is
                 * no refinement, so the index list is just 0..n-1. */
                uint32_t idx[MAX_FN_ARITY];
                uint32_t n_idx = mb->n_refine_params < MAX_FN_ARITY
                               ? mb->n_refine_params : MAX_FN_ARITY;
                for (uint32_t _i = 0; _i < n_idx; _i++) idx[_i] = _i;
                Binding *m_check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);
                method_body = rt_inject_param_checks(
                    e, method_body, m_check_fn,
                    mp->method_params, mp->n_method_params,
                    mb->refine_param_preds, mb->refine_param_vars,
                    idx, n_idx, impl_form->span);
            }
        }

        /* RT1: check this method's RESULT against the refinement it promises --
         * its own if it restated one, otherwise the class's, which it inherits.
         * Before this, a `defclass` result refinement produced no check
         * anywhere: the identical predicate on a plain `defn` panicked, while a
         * class method returning -9 under `: #refine{ r : int | (>= r 0) }`
         * printed -9 and exited 0. */
        {
            const Binding *rb = mp->method_fd ? mp->method_fd->binding : NULL;
            if (rb && rb->refine_return_pred && method_body &&
                rt_contracts_emitted()) {
                Binding *m_check_fn =
                    scope_lookup(&e->global, e->sym_tur_contract_check);
                method_body = rt_wrap_return_check(
                    e, method_body, m_check_fn, rb->refine_return_pred,
                    rb->refine_return_var, "Return contract violated",
                    impl_form->span);
                /* ...and the class's promise on top, when the instance's own
                 * was not proved to imply it.  See the comment where this is
                 * set: a dispatch site relies on the class predicate. */
                if (rb->refine_class_ret_pred)
                    method_body = rt_wrap_return_check(
                        e, method_body, m_check_fn, rb->refine_class_ret_pred,
                        rb->refine_class_ret_var,
                        "Class result contract violated", impl_form->span);
            }
        }

        /* Pop method scope */
        e->scope = method_scope.parent;
        scope_free(&method_scope);

        FnDef *method_fd = mp->method_fd;
        method_fd->body = method_body;

        /* constrained-generic-dispatch-tyvar-name-and-inlinec (Bug 2): stamp the
         * binding's `body_is_inline_c` flag here, the same way elab_fns.c does
         * for ordinary defns.  Instance-method FnDefs are built in this pass, so
         * without this the flag stays false on every instance method -- and the
         * call-site ABI logic that keys off it (the Phase D `&temp` pass-by-ptr
         * spill in emit_expr.c, and #439's emit_reresolved_receiver_is_by_ptr
         * bridge) would wrongly take the address of an inline-C instance's
         * by-value struct receiver, passing a `T *` to a by-value `T self`
         * formal -- a hard cc type error at both direct and generic call sites. */
        if (method_fd->binding) {
            method_fd->binding->body_is_inline_c =
                (method_body && method_body->kind == EX_INLINE_C);
        }

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

        /* carrier-aware-return-unification: reject a genuine return-position
         * conflict between the method's declared return (after Phase RT
         * substitution) and its elaborated body via the shared
         * `return_position_conflict` dispatcher.  An instance method is normally a
         * CARRIER_METHOD position (the dispatcher only flags the float-COMMIT
         * direction, since a non-float-declared carrier method with a float
         * instance body is the deliberate per-instance bridge the typeclass ABI
         * resolves to the real register class) -- EXCEPT when Phase 3 classifies
         * it COMMITTED (see meth_cls below).  The arrow-head refinement above
         * already handled TY_FN results.  inline-C bodies (fiat TY_NIL) are
         * skipped; the int-literal -> float coercion is widened in place first.
         * e->scope and fn_body_depth are already restored here, so on error we
         * mirror the surrounding return-NULL. */
        if (method_body && method_body->kind != EX_INLINE_C) {
            rc_widen_int_literal_to_float_return(mp->ret_kind, method_body);
            /* carrier-aware-return-unification Phase 3: a method whose class-decl
             * return was the class type variable, grounded to a concrete
             * (free-tyvar-free) type for this instance, is a genuine per-instance
             * commit -- as strict as the equivalent defn.  Otherwise (a fixed
             * concrete class-decl slot, an explicit annotation, or a still-applied
             * HKT return like bind/ap's `(f b)` carrying a free element) the
             * method participates in the carrier and stays tolerant. */
            ReturnClass meth_cls =
                (mp->ret_was_class_var && !m7_type_has_free_tyvar(mp->ret_full))
                    ? RET_CLASS_COMMITTED
                    : RET_CLASS_CARRIER_METHOD;
            ReturnConflict rc = return_position_conflict(
                mp->ret_adt, mp->ret_kind, method_body->type,
                meth_cls);
            if (rc != RET_CONFLICT_NONE) {
                const char *want = mp->ret_adt ? mp->ret_adt->name
                                 : typekind_to_string(mp->ret_kind);
                const char *meth = (tc->methods[i].name) ? tc->methods[i].name->name : "?";
                Buf gb; buf_init(&gb);
                type_print(&gb, method_body->type);
                buf_putc(&gb, '\0');
                switch (rc) {
                    case RET_CONFLICT_NOMINAL:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0001_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_REGISTER_CLASS:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- a float and a non-float live "
                            "in different register classes (xmm vs general-purpose),"
                            " so this is a register-class miscompile, not a "
                            "tolerable carrier bridge",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_POINTER_SCALAR:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
                            "instance method '%s' declares return type 'cstr' but "
                            "its body returns %s -- a bare integer is never a "
                            "valid string pointer, so this is a type-erasure bug, "
                            "not a tolerable carrier bridge",
                            meth, gb.data);
                        break;
                    /* TYPE_REVERSE / BOOL_INTEGER (TUR-E0709): reachable only when
                     * Phase 3 classified this method RET_CLASS_COMMITTED (its
                     * class-decl return was the class type variable, grounded to a
                     * concrete type for this instance), so a divergent concrete
                     * body is a real per-instance mismatch, not a carrier bridge. */
                    case RET_CONFLICT_TYPE_REVERSE:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0709_RETURN_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- a string pointer is never a "
                            "valid integer, and this instance commits to the "
                            "class type variable's grounding, so there is no "
                            "carrier to bridge it",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_BOOL_INTEGER:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0709_RETURN_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- bool and the integer family "
                            "are distinct types (boolean constants are "
                            "true/false, not 0/1), and this instance commits to "
                            "the class type variable's grounding, so there is no "
                            "carrier to bridge them",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_CARRIER_AGGREGATE:
                        diag_emit_with_code(DIAG_ERROR, method_body->span,
                            TUR_E0709_RETURN_TYPE_MISMATCH,
                            "instance method '%s' declares return type '%s' but "
                            "its body returns %s -- an aggregate is a real C "
                            "type (a struct, or a typed pointer to one), not the "
                            "int64 carrier, so there is no representation these "
                            "two share and nothing to bridge them",
                            meth, want, gb.data);
                        break;
                    case RET_CONFLICT_NONE: break;  /* unreachable */
                }
                buf_free(&gb);
                return NULL;
            }
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
            /* An ADT (defdata/defgadt) type-arg, like Functor [Either], is owned
             * by the module that declares it. */
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
/* RT1: the Binding standing for a class method's CLASS-LEVEL signature, used to
 * record a crossing at a dispatch that never resolved to an instance.
 *
 * Checking the caller's argument against the class predicate is sound by the
 * same variance argument that already licenses result propagation, run in the
 * other direction.  `TUR-E0374` rejects an instance that demands MORE than its
 * class, so every instance's parameter predicate is implied by the class's --
 * which makes the class predicate the strongest demand true of EVERY instance,
 * and an argument satisfying it acceptable to whichever instance runs.
 *
 * It is also strictly stronger than what a dynamic site could otherwise use.
 * With no resolved instance the dispatch falls back to an arbitrary
 * carrier-compatible one, and that instance's predicate is *weaker* than the
 * class's (or absent), so checking against it would demand less than the
 * contract while claiming to check the contract.
 *
 * Returns NULL -- meaning "record nothing" -- when no parameter carries a
 * refinement, which is the overwhelmingly common case. */
static const Binding *rt_class_method_refine_binding(Elab *e, TypeClass *tc,
                                                     uint8_t mi) {
    if (!e || !tc || mi >= tc->n_methods) return NULL;
    TypeClassMethod *m = &tc->methods[mi];
    if (m->refine_class_binding) return m->refine_class_binding;
    if (!m->param_refine_preds || m->n_params == 0) return NULL;

    bool any = false;
    for (uint8_t p = 0; p < m->n_params; p++)
        if (m->param_refine_preds[p]) { any = true; break; }
    if (!any) return NULL;

    Binding *b = (Binding *)arena_alloc(e->arena, sizeof(Binding));
    memset(b, 0, sizeof(*b));
    b->name      = m->name;
    b->is_global = true;

    /* Arg kinds matter: refine_resolve_call_sites falls back to TY_INT for a
     * parameter it cannot type, which would encode a float argument into the
     * wrong sort. */
    TypeKind kinds[MAX_FN_ARITY];
    uint32_t arity = m->n_params < MAX_FN_ARITY ? m->n_params : MAX_FN_ARITY;
    for (uint32_t p = 0; p < arity; p++)
        kinds[p] = m->param_types ? m->param_types[p].kind : TY_INT;
    b->type = type_fn(kinds, arity, m->return_type.kind);

    const Form **preds = (const Form **)arena_alloc(e->arena,
                              m->n_params * sizeof(const Form *));
    const char **vars  = (const char **)arena_alloc(e->arena,
                              m->n_params * sizeof(const char *));
    const char **names = (const char **)arena_alloc(e->arena,
                              m->n_params * sizeof(const char *));
    for (uint8_t p = 0; p < m->n_params; p++) {
        preds[p] = m->param_refine_preds[p];
        vars[p]  = m->param_refine_vars ? m->param_refine_vars[p] : NULL;
        names[p] = (m->param_names && m->param_names[p])
                 ? m->param_names[p]->name : NULL;
    }
    b->refine_param_preds = preds;
    b->refine_param_vars  = vars;
    b->refine_param_names = names;
    b->n_refine_params    = m->n_params;

    m->refine_class_binding = b;
    return b;
}

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
            case TY_ADT:
                /* CONV-S1 (defstruct-as-defadt): match emit_dict_name's TY_ADT arm
                 * so the DICT expr's dict_name agrees with the emitted struct. */
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.adt_.def &&
                         inst->type_args[i].as.adt_.def->name)
                    component = inst->type_args[i].as.adt_.def->name;
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

/* Predicate mirroring the method-finding phase of elab_try_return_dispatch:
 * true when `name` resolves to a return-only-dispatch typeclass method (a class
 * type param appears in the method's return type but in none of its parameter
 * types) and no ordinary binding shadows it.  Used by elab_if to recognise an
 * arm whose instance can only be picked from an expected result type, so a
 * concrete sibling arm can supply that type (return-directed-methods-pure-empty-
 * inference, fix direction #2). */
bool elab_symbol_is_return_dispatch_method(Elab *e, const Symbol *name) {
    if (!name) return false;
    /* A user/local defn of the same name wins (mirrors the `!fn_binding` gate
     * at the elab_try_return_dispatch call site); it is not return-directed. */
    if (scope_lookup(e->scope, name)) return false;

    TypeClassEnv *env = &e->typeclass_env;
    for (TypeClass *c = env->typeclasses; c != NULL; c = c->next) {
        for (uint8_t mi = 0; mi < c->n_methods; mi++) {
            const TypeClassMethod *m = &c->methods[mi];
            if (!(m->name->len == name->len &&
                  memcmp(m->name->name, name->name, name->len) == 0)) {
                continue;
            }
            /* Some class type param must appear ONLY in the return type. */
            bool any_tp_in_param = false;
            for (uint8_t ti = 0; ti < c->n_type_params && !any_tp_in_param; ti++) {
                const Symbol *tp = c->type_params[ti];
                if (!tp) continue;
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
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
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
                    if (rt_type_mentions_tyvar(&m->param_types[pi], tp->name)) {
                        in_param = true;
                        break;
                    }
                }
                if (in_param) continue;
                return true;
            }
        }
    }
    return false;
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
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
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
                for (uint32_t pi = 0; pi < m->n_params; pi++) {
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
        /* return-dispatch-tyvar (docs/archive/history/return-dispatch-tyvar-silent-
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
            bound.kind == TY_TYVAR;
        if (bound_is_abstract_tyvar) {
            for (TypeClassInstance *it = env->instances; it; it = it->next) {
                if (it->typeclass != tc) continue;
                if (midx >= it->n_method_impls || !it->method_impls[midx]) continue;
                if (it->n_type_args > 0 && it->type_args[0].kind == TY_INT) {
                    inst = it;
                    break;
                }
            }
            /* constrained-hkt-pure-and-byvalue-carriers (gap 1): the search above
             * looks for a kind-`*` `int`-headed representative, which a
             * higher-kinded class never has -- every `Applicative`/`Monad`
             * instance head is a type CONSTRUCTOR.  So `pure`/`empty` on an
             * abstract `m` inside a constrained poly fn found no representative
             * and hard-errored ("no instance 'Applicative tyvar'"), even though
             * the receiver-directed methods of the very same constraint (`ap`,
             * `bind`) resolve fine through the representative path in
             * elab_method_call.
             *
             * Mirror that path for the higher-kinded case: when the abstract
             * tyvar IS this function's constraint variable and `tc` is the
             * constraint's class, use the constraint's ambient representative
             * instance as the polymorphic base.  Emit-side re-resolution then
             * specializes it per monomorphization, exactly as for the kind-`*`
             * representative above.  Gated on the ambient constraint so an
             * unconstrained `(pure 42)` with no expected type still reports the
             * ascription-required diagnostic rather than silently picking an
             * instance.
             *
             * The gate is that `bound` names THIS body's abstract constraint
             * variable -- not that `tc` is the ambient class.  A body may
             * constrain several classes on one type constructor (`[^Monad m
             * ^Applicative m ...]`), and only the first is recorded as ambient;
             * keying on the tyvar covers the rest.  This is the same latitude
             * the receiver-directed path already takes, which picks a
             * representative for `ap`/`bind` without consulting the constraint
             * set at all. */
            if (!inst && e->cur_hkt_constraint_tyvar && bound.as.tyvar_.name &&
                strcmp(bound.as.tyvar_.name, e->cur_hkt_constraint_tyvar) == 0) {
                TypeClassInstance *repr =
                    (e->cur_hkt_dict_binding && e->cur_hkt_dict_binding->ambient_repr)
                        ? e->cur_hkt_dict_binding->ambient_repr : NULL;
                /* The ambient repr belongs to the FIRST constraint's class; it is
                 * only usable here when that is also `tc`. */
                if (repr && (repr->typeclass != tc ||
                             midx >= repr->n_method_impls || !repr->method_impls[midx]))
                    repr = NULL;
                if (!repr) {
                    for (TypeClassInstance *it = env->instances; it; it = it->next) {
                        if (it->typeclass != tc) continue;
                        if (midx >= it->n_method_impls || !it->method_impls[midx])
                            continue;
                        repr = it;
                        break;
                    }
                }
                inst = repr;
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
     * (docs/archive/m4c-path-a-result-side-needs-return-dispatch-elab-hook.md):
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
            /* nested-construct/constrained-instance return dispatch: besides
             * binding the class var (`a -> (Option cstr)`), also bind the matched
             * instance's OWN head tyvars by unifying its head (`(Option A)`)
             * against the pinned dispatch value (`(Option cstr)`) -> `A -> cstr`.
             * A constrained instance body (`(definstance Dec [Option] [(Dec A)]
             * ...)`) writes its nested construct/return-dispatch seams in terms
             * of `A`; without grounding it the spec collapses every element to the
             * int64-carrier representative (`dec_int`, `Option__int`) and a
             * cstr/float/struct consumer misreads the carrier int.  Mirrors the
             * receiver-dispatch path's head-tyvar collection. */
            /* The instance's constraint vars (`A` of `[(Dec A)]`) are NOT in the
             * bare head `Option`; recover them from the pinned dispatch value's
             * app args at each constraint's param_idx -- `a = (Option cstr)` ->
             * A = arg[0] = cstr. */
            Type barg_spine[8]; uint8_t n_barg = 0;
            {
                Type tcur = bound; Type stack[8]; uint8_t ns = 0;
                while (tcur.kind == TY_APP && tcur.as.app.fn && tcur.as.app.arg) {
                    if (ns < 8) stack[ns++] = *tcur.as.app.arg;
                    tcur = *tcur.as.app.fn;
                }
                for (int s = (int)ns - 1; s >= 0 && n_barg < 8; s--)
                    barg_spine[n_barg++] = stack[s];
            }
            const Symbol *hb_names[ABI_TYPE_BINDINGS_MAX];
            Type hb_types[ABI_TYPE_BINDINGS_MAX];
            uint8_t hb_n = 0;
            uint8_t neg_pos = 0;  /* positional index for standalone (param_idx<0) constraints */
            for (uint8_t ci = 0;
                 ci < inst->n_type_param_constraints &&
                 hb_n < ABI_TYPE_BINDINGS_MAX - 1; ci++) {
                const TypeConstraint *cstr = &inst->type_param_constraints[ci];
                if (!cstr->tyvar || !cstr->tyvar->name) continue;
                /* A constraint tied to a head type-param position uses param_idx;
                 * a STANDALONE constraint (`[Option] [(Dec A)]`, param_idx == -1)
                 * binds positionally against the dispatch value's app args -- the
                 * `[(Dec A)]` element corresponds to `(Option cstr)`'s arg[0]. */
                int pidx = cstr->param_idx >= 0 ? cstr->param_idx : (int)neg_pos;
                if (cstr->param_idx < 0) neg_pos++;
                if (pidx < 0 || pidx >= (int)n_barg) continue;
                hb_names[hb_n] = cstr->tyvar;
                hb_types[hb_n] = barg_spine[pidx];
                hb_n++;
            }
            uint8_t total = (uint8_t)(1 + hb_n);
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = bound;
            for (uint8_t k = 0; k < hb_n; k++) {
                bindings[1 + k].name = hb_names[k]->name;
                bindings[1 + k].type = hb_types[k];
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = total;
        }
        /* M7 layer-4: a return-directed HKT method -- Applicative `pure`/`wrap`,
         * `[x : a] : (f a)` -- has its class var `f` ONLY in the result, so the
         * instance is picked from the ascribed return type (`bound` = the
         * constructor, e.g. Option) and the element `a` is recovered from the
         * argument types.  Mirror the receiver-dispatch path's by-value spec
         * interning (elab_method_call) so the instance method emits a by-value
         * `(Option int)` return instead of the int64 carrier (which the by-value
         * consumer would then misread -> silent miscompile).  Gated on a
         * by-value-constructible body, exactly as the receiver path is. */
        if (is_hkt && tc->n_type_params >= 1 &&
            tc->type_params[0] && impl->body &&
            impl->body->kind != EX_INLINE_C &&
            m7_body_constructs_byvalue(impl->body)) {
            const Symbol *m7_names[16];
            Type m7_types[16];
            uint8_t m7_n = 0;
            for (uint32_t i = 0; i < n_args; i++) {
                if (i < meth->n_params && args[i])
                    m7_collect_tyvar_bindings(e, meth->param_types[i],
                                              args[i]->type, m7_names, m7_types,
                                              &m7_n, 16);
            }
            /* Bind the HKT class var to the CONSTRUCTOR HEAD of the ascribed
             * result (`Option`), not the full applied `(Option int)`, matching
             * the receiver-dispatch path (an in-body applied occurrence `(f a)`
             * then resolves to `(Option a)` -> `(Option int)` at emit). */
            Type hkt_head = bound;
            while (hkt_head.kind == TY_APP && hkt_head.as.app.fn)
                hkt_head = *hkt_head.as.app.fn;
            uint8_t total = (uint8_t)(1 + m7_n);
            if (total > ABI_TYPE_BINDINGS_MAX) total = ABI_TYPE_BINDINGS_MAX;
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = hkt_head;
            uint8_t bi = 1;
            for (uint8_t k = 0; k < m7_n && bi < total; k++) {
                bindings[bi].name = m7_names[k]->name;
                bindings[bi].type = m7_types[k];
                bi++;
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = bi;
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

/* unascribed-carrier-helper-read-collapses-element-tyvar: structurally match a
 * carrier helper's declared parameter type (carrying the helper's type-param
 * `R`) against the actual argument type, returning the subtype at `R`'s
 * position.  Mirror of emit_core.c:emit_pattern_extract_classvar for the elab
 * side. */
static bool tc_pattern_extract_var(const Type *pattern, const Type *concrete,
                                   const char *varname, Type *out) {
    if (!pattern || !concrete || !varname) return false;
    if (pattern->kind == TY_TYVAR && pattern->as.tyvar_.name &&
        strcmp(pattern->as.tyvar_.name, varname) == 0) {
        *out = *concrete;
        return true;
    }
    if (pattern->kind == TY_APP && concrete->kind == TY_APP) {
        if (tc_pattern_extract_var(pattern->as.app.fn, concrete->as.app.fn,
                                   varname, out))
            return true;
        if (tc_pattern_extract_var(pattern->as.app.arg, concrete->as.app.arg,
                                   varname, out))
            return true;
    }
    return false;
}

/* unascribed-carrier-helper-read-collapses-element-tyvar: true when `obj` is an
 * UNASCRIBED generic carrier-helper read used as a class-method receiver --
 * `(tag (vec-get v i))` -- whose recovered element type is still an abstract
 * tyvar.  Such a receiver arises inside a constrained generic instance body,
 * where the container's element is the constraint var; the helper's `:A` return
 * collapses to the int64 carrier (TY_INT) at elaboration, so without this it
 * would match a fixed `int` instance (silent mis-dispatch) or, with no `int`
 * instance, report TUR_E0020.  Treating it as an abstract-tyvar receiver routes
 * it to the carrier representative + dict tagging, and emit-side re-resolution
 * (emit_reresolve_disp_type) specializes the element call per ABI specialization
 * -- the same outcome the documented `(:: e A)` ascription idiom already gets. */
static bool obj_is_unascribed_carrier_elem(const Expr *obj) {
    while (obj && obj->kind == EX_ASCRIBE) obj = obj->as.ascribe_.inner;
    if (!obj || obj->kind != EX_CALL || !obj->as.call_.fn_binding ||
        !obj->as.call_.args)
        return false;
    const Type *ft = &obj->as.call_.fn_binding->type;
    if (ft->kind != TY_FN || !ft->as.fn.arg_full_types ||
        !ft->as.fn.result_full_type ||
        ft->as.fn.result_full_type->kind != TY_TYVAR ||
        !ft->as.fn.result_full_type->as.tyvar_.name)
        return false;
    const char *rname = ft->as.fn.result_full_type->as.tyvar_.name;
    uint32_t np = ft->as.fn.arity;
    for (uint32_t pi = 0; pi < np && pi < obj->as.call_.n_args; pi++) {
        const Type *pft = ft->as.fn.arg_full_types[pi];
        const Expr *ae = obj->as.call_.args[pi];
        if (!pft || !ae) continue;
        Type extracted;
        if (tc_pattern_extract_var(pft, &ae->type, rname, &extracted))
            return extracted.kind == TY_TYVAR;
    }
    return false;
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

    /* Clojure-style receiver-first sugar: `(. obj field args...)` where the
     * head is the bare `.` symbol (length 1) and the field name is a separate
     * symbol.  Desugar to `(.field obj args...)` so the joined-form paths below
     * (struct field access, function-typed field call-through, typeclass
     * dispatch) all apply uniformly.  Without this rewrite the head `.` yields
     * an empty method name and dispatch fails with "no typeclass method found
     * for ''" (docs/archive/history/dot-method-call-misroutes-to-typeclass.md). */
    if (call->as.list.items[0]->tag == F_SYM &&
        call->as.list.items[0]->as.sym->len == 1 &&
        call->as.list.items[0]->as.sym->name[0] == '.') {
        if (call->as.list.len < 3 || call->as.list.items[2]->tag != F_SYM) {
            diag_emit(DIAG_ERROR, call->span,
                      "(. obj field args...) requires a field-name symbol, "
                      "e.g. (. p age) or (. lens get s)");
            return NULL;
        }
        const Symbol *field_sym = call->as.list.items[2]->as.sym;
        char dotbuf[160];
        int dotlen = snprintf(dotbuf, sizeof(dotbuf), ".%.*s",
                              (int)field_sym->len, field_sym->name);
        if (dotlen <= 0 || (size_t)dotlen >= sizeof(dotbuf)) {
            diag_emit(DIAG_ERROR, call->span,
                      "(. obj field ...) field name too long");
            return NULL;
        }
        const Symbol *dot_sym =
            symtab_intern(e->st, strslice(dotbuf, (uint32_t)dotlen));
        /* New list: [.field, obj, args...] = [.field, items[1], items[3..]]. */
        uint32_t n_items = call->as.list.len - 1;
        Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
        items[0] = form_sym(e->arena, call->as.list.items[2]->span, dot_sym);
        items[1] = call->as.list.items[1];
        for (uint32_t i = 3; i < call->as.list.len; i++)
            items[i - 1] = call->as.list.items[i];
        Form *dotcall = form_list(e->arena, call->span, items, n_items);
        return elab_method_call(e, dotcall);
    }

    /* Parse method name from the symbol (skip the leading '.') */
    Form *head = call->as.list.items[0];
    const Symbol *method_sym = head->as.sym;
    const char *method_name = method_sym->name + 1;  /* Skip '.' */
    uint32_t method_name_len = method_sym->len - 1;

    /* A bare `.` head (now handled by the receiver-first desugaring above)
     * leaves an empty method name; any other route here with an empty name is
     * a malformed call.  Reject it with a clear message rather than letting the
     * empty name reach the "no typeclass method found for ''" diagnostic. */
    if (method_name_len == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "method call requires a field/method name after '.', "
                  "e.g. (.field obj) or (. obj field)");
        return NULL;
    }

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
            /* heap-struct-field-extraction-collapses-to-carrier: a `@TypeName`
             * witness call pins the instance but never recorded abi_bindings, so
             * emit_abi_register_call had nothing to specialize and the call fell
             * through to the int64-carrier base clone.  For a :heap container
             * (`(Cons int)`) that base clone derefs `(xs)->head` on the int64
             * carrier -- a hard C compile error -- and bakes the int element
             * instance for every A.  Mirror the receiver-dispatch path's M4c
             * Path A binding population (elab_method_call below, ~line 5627):
             * bind the class var to the witness receiver's concrete type and the
             * instance head's own tyvars (the constraint var `A`) to the
             * receiver's element types, so a per-instantiation by-value/heap spec
             * is minted and the inner `(enc (.head xs))` re-dispatches per
             * element.  HKT classes keep the uniform-carrier dispatch. */
            if (witness_inst && witness_inst->typeclass &&
                out_w->as.call_.fn_binding != NULL) {
                TypeClass *wtc = witness_inst->typeclass;
                bool w_is_hkt = false;
                if (wtc->type_param_kinds) {
                    for (uint8_t i = 0; i < wtc->n_type_params; i++)
                        if (wtc->type_param_kinds[i] != KIND_STAR) { w_is_hkt = true; break; }
                }
                bool recv_parametric =
                    obj_w->type.kind == TY_APP;
                if (!w_is_hkt && wtc->n_type_params == 1 && wtc->type_params[0] &&
                    recv_parametric) {
                    const Symbol *hb_names[ABI_TYPE_BINDINGS_MAX];
                    Type hb_types[ABI_TYPE_BINDINGS_MAX];
                    uint8_t hb_n = 0;
                    if (witness_inst->n_type_args >= 1)
                        m7_collect_tyvar_bindings(e, witness_inst->type_args[0],
                                                  obj_w->type, hb_names, hb_types,
                                                  &hb_n, ABI_TYPE_BINDINGS_MAX - 1);
                    uint8_t total = (uint8_t)(1 + hb_n);
                    AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                        e->arena, total * sizeof(AbiTypeBinding));
                    bindings[0].name = wtc->type_params[0]->name;
                    bindings[0].type = obj_w->type;
                    for (uint8_t k = 0; k < hb_n; k++) {
                        bindings[1 + k].name = hb_names[k]->name;
                        bindings[1 + k].type = hb_types[k];
                    }
                    out_w->as.call_.abi_bindings = bindings;
                    out_w->as.call_.n_abi_bindings = total;
                }
            }
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

    /* Existential-open witness dispatch.
     * When the receiver `v` was bound by `(open e [a v] ...)` over a
     * constraint-carrying existential, its static type is erased to the int64
     * carrier, so static instance search would either trivially pick the lone
     * instance (single in-scope) or fail as ambiguous (>=2 in scope).  Neither
     * consults the witness packed at the `pack` site.  Resolve instead through
     * the record's runtime witness vtable: locate the constraint class that
     * declares this method, find its witness slot, and build an
     * EX_EXISTS_DISPATCH node that the emitter lowers to an indirect call
     * through `record->witnesses[slot]`.  This is independent of how many
     * instances of the class are in scope. */
    if (obj->kind == EX_VAR && obj->as.var.binding
            && obj->as.var.binding->exists_open_type) {
        const Type *ex = obj->as.var.binding->exists_open_type;
        for (uint8_t ci = 0; ci < ex->as.forall_.n_constraints; ci++) {
            const TypeClass *tc = ex->as.forall_.constraint_classes[ci];
            if (!tc) continue;
            for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
                if (tc->methods[mi].name->len != method_name_len ||
                    memcmp(tc->methods[mi].name->name, method_name,
                           method_name_len) != 0) {
                    continue;
                }
                /* Found the class+method.  Elaborate the receiver and any extra
                 * args (args[0] is the receiver `v`). */
                uint32_t n_args = call->as.list.len - 1;
                Expr **args = (Expr **)arena_alloc(e->arena,
                                                   n_args * sizeof(Expr *));
                args[0] = obj;
                bool args_ok = true;
                for (uint32_t i = 1; i < n_args; i++) {
                    args[i] = elab_form(e, call->as.list.items[1 + i]);
                    if (!args[i]) { args_ok = false; break; }
                }
                if (!args_ok) return NULL;

                Type result_type = tc->methods[mi].return_type;
                if (result_type.kind == TY_UNKNOWN ||
                    result_type.kind == TY_NIL ||
                    result_type.kind == TY_TYVAR) {
                    result_type = TYPE_INT;
                }

                Expr *out = expr_new(e->arena, EX_EXISTS_DISPATCH, result_type,
                                     call->span);
                out->as.exists_dispatch_.open_binding = obj->as.var.binding;
                out->as.exists_dispatch_.typeclass    = tc;
                out->as.exists_dispatch_.witness_idx  = ci;
                out->as.exists_dispatch_.method_idx   = mi;
                out->as.exists_dispatch_.args         = args;
                out->as.exists_dispatch_.n_args       = n_args;
                return out;
            }
        }
        /* No constraint class declares this method -- fall through to the
         * normal dispatch path (which will report no-method or an unrelated
         * struct-field access). */
    }

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
        /* CONV-S1 (slice 2): auto-deref for an rc<ADT> receiver, so
         * `(.field rc-of-adt)` resolves through the record variant carried on
         * the rc type.  obj keeps its rc<ADT> type; codegen derefs the control
         * block's value pointer to reach the aggregate. */
        if (base.kind == TY_RC && base.as.rc.adt_def) {
            struct AdtDef *rc_adt = base.as.rc.adt_def;  /* read before union rewrite */
            base.kind = TY_ADT;
            base.as.adt_.def = rc_adt;
            field_owner_type = &base;
        }
        /* CONV-S0/S4: field access on a single-variant record ADT.  A
         * single-variant, non-GADT ADT whose sole constructor is record-style
         * is a product, so `(.field v)` / `(. v field)` reads the named field
         * directly -- the same surface a struct exposes (a struct *is* this
         * case).  Unwrap a parametric TY_APP head to its base ADT first. */
        {
            const Type *adt_base = field_owner_type;
            while (adt_base && adt_base->kind == TY_APP && adt_base->as.app.fn)
                adt_base = adt_base->as.app.fn;
            if (adt_base && adt_base->kind == TY_ADT && adt_base->as.adt_.def) {
                const AdtDef *adt = adt_base->as.adt_.def;
                /* CONV-S4N: the sole ctor of a single-variant record product,
                 * OR -- for a scrutinee a `match` arm has narrowed to one record
                 * variant -- the proven variant.  Reading `(.field s)` off a
                 * narrowed multi-variant scrutinee reads that variant's field out
                 * of the tagged union (the value's tag proves the variant), the
                 * same member path a match field-bind uses. */
                const CtorDef *ctor = NULL;
                /* A defdata that failed mid-elaboration (e.g. an unresolvable
                 * constructor field type) can leave n_ctors set with NULL
                 * ctors entries; guard so the error path degrades to "field
                 * not found" instead of a NULL-deref SIGSEGV. */
                if (adt_is_flat_product(adt) && adt->n_ctors == 1 &&
                    adt->ctors && adt->ctors[0] && adt->ctors[0]->is_record) {
                    ctor = adt->ctors[0];
                } else if (adt_is_narrowed_to_record_variant(*adt_base) &&
                           adt->ctors) {
                    ctor = adt->ctors[adt_base->as.adt_.narrowed_ctor_idx];
                }
                if (ctor) {
                    for (uint32_t i = 0; i < ctor->n_fields; i++) {
                        if (!ctor->fields[i].name) continue;
                        if (strcmp(ctor->fields[i].name, method_name) == 0) {
                            Type ftype;
                            if (ctor->fields[i].full_type) {
                                ftype = *ctor->fields[i].full_type;
                            } else if (ctor->fields[i].kind == TY_REF ||
                                       ctor->fields[i].kind == TY_LREF ||
                                       ctor->fields[i].kind == TY_RC ||
                                       ctor->fields[i].kind == TY_WEAK) {
                                /* linear-lref-struct-field: a pointer-kind field
                                 * (lref/ref/rc/weak) with no full_type still
                                 * carries its pointee in inner_kind.  Without it
                                 * `(.ptr b)` types as a bare lref<?> and `deref`
                                 * yields TY_UNKNOWN.  Mirror the struct path's
                                 * elab_struct_field_use_type, which threads the
                                 * inner kind onto the reconstructed type. */
                                ftype = type_from_kind(ctor->fields[i].kind);
                                if (ctor->fields[i].kind == TY_REF ||
                                    ctor->fields[i].kind == TY_LREF)
                                    ftype.as.ref.inner = ctor->fields[i].inner_kind;
                                else
                                    ftype.as.rc.inner = ctor->fields[i].inner_kind;
                            } else {
                                ftype = type_from_kind(ctor->fields[i].kind);
                            }
                            /* Parametric record ADT: substitute the receiver's
                             * concrete type args for the field's TY_TYVAR names so
                             * `(.val (Box 42))` reads as int, not the bare tyvar A
                             * (which fails overload resolution in untyped contexts
                             * like `(println (.val b))`).  Mirrors the match
                             * field-bind substitution (elab_structs.c) and the
                             * struct path's elab_struct_field_use_type. */
                            if (ctor->fields[i].full_type &&
                                    adt->n_type_params > 0 &&
                                    field_owner_type->kind == TY_APP) {
                                Type *type_args = (Type *)arena_alloc(e->arena,
                                    adt->n_type_params * sizeof(Type));
                                if (elab_adt_type_extract_args(field_owner_type,
                                                               adt, type_args))
                                    ftype = adt_field_instantiate_type(e, adt,
                                        ctor->fields[i].full_type, type_args);
                            }
                            /* Bare/unparameterized receiver (`r : Result`, a
                             * TY_ADT with no type-arg application) leaves a tyvar
                             * field type unbound; the value rides the int64
                             * carrier exactly as at default, so collapse the
                             * residual tyvar to the field's carrier kind (TY_INT).
                             * Without this `(.ok-val r)` types as a bare tyvar and
                             * fails overload resolution in an untyped context like
                             * `(println (.ok-val r))`.  Mirrors the struct path's
                             * elab_struct_field_use_type, which already collapses
                             * an unbound field tyvar.  Gated to the bare receiver:
                             * an APPLIED receiver `(Tuple2 A B)` inside a generic
                             * body keeps its field tyvar so the ABI spec can later
                             * resolve it to a concrete by-value aggregate (the
                             * accessor-unbox path). */
                            if (ftype.kind == TY_TYVAR &&
                                field_owner_type->kind != TY_APP)
                                ftype = type_from_kind(ctor->fields[i].kind);
                            Expr *out = expr_new(e->arena, EX_GET_FIELD, ftype,
                                                 call->span);
                            out->as.get_field_.struct_expr = obj;
                            out->as.get_field_.field_idx = i;
                            out->as.get_field_.adt_def = adt;
                            out->as.get_field_.adt_ctor = ctor;
                            return out;
                        }
                    }
                }
            }
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
        /* CONV-S1 (slice 6): the same capability-field call through an `fn` field
         * of a single-variant record ADT -- a record ADT *is* a product, so
         * `(.handler v arg ...)` calls the function stored in the named field,
         * exactly as a struct does (a `defstruct` with an `fn` field lowers to
         * this case).  Mirror the TY_STRUCT branch above, building the
         * EX_GET_FIELD with adt_def/adt_ctor instead of a StructDef. */
        {
            const Type *adt_base = &base;
            while (adt_base && adt_base->kind == TY_APP && adt_base->as.app.fn)
                adt_base = adt_base->as.app.fn;
            if (adt_base && adt_base->kind == TY_ADT && adt_base->as.adt_.def) {
                const AdtDef *adt = adt_base->as.adt_.def;
                if (adt_is_flat_product(adt) && adt->n_ctors == 1 &&
                    adt->ctors[0]->is_record) {
                    const CtorDef *ctor = adt->ctors[0];
                    for (uint32_t i = 0; i < ctor->n_fields; i++) {
                        if (!ctor->fields[i].name) continue;
                        if (strcmp(ctor->fields[i].name, method_name) != 0) continue;
                        if (ctor->fields[i].kind != TY_FN) break;
                        Type field_type = ctor->fields[i].full_type
                            ? *ctor->fields[i].full_type
                            : type_from_kind(ctor->fields[i].kind);
                        /* lowered-adt-ctor-skips-fn-field-type-param-inference:
                         * substitute the receiver's concrete type args into the
                         * fn-field's signature so `(.get l p)` on `l : (Lens Person
                         * cstr)` reads `get` as `(fn [Person] cstr)` -- otherwise
                         * the result stays the bare tyvar `A` and the call types as
                         * a tyvar (TUR-E0006 in an untyped context).  Mirrors the
                         * plain dot-read path's app-arg substitution above. */
                        if (ctor->fields[i].full_type && adt->n_type_params > 0 &&
                            base.kind == TY_APP) {
                            Type *type_args = (Type *)arena_alloc(
                                e->arena, adt->n_type_params * sizeof(Type));
                            if (elab_adt_type_extract_args(&base, adt, type_args))
                                field_type = adt_field_instantiate_type(
                                    e, adt, ctor->fields[i].full_type, type_args);
                        }
                        Expr *get_field = expr_new(e->arena, EX_GET_FIELD,
                                                   field_type, call->span);
                        get_field->as.get_field_.struct_expr = obj;
                        get_field->as.get_field_.field_idx = i;
                        get_field->as.get_field_.adt_def = adt;
                        get_field->as.get_field_.adt_ctor = ctor;

                        uint32_t n_args = call->as.list.len - 2;
                        Expr **args = (Expr **)arena_alloc(e->arena,
                                                           n_args * sizeof(Expr *));
                        for (uint32_t j = 0; j < n_args; j++) {
                            args[j] = elab_form(e, call->as.list.items[2 + j]);
                            if (!args[j]) return NULL;
                        }
                        Type result_type = TYPE_INT;
                        if (field_type.kind == TY_FN) {
                            Type rt = field_type.as.fn.result_full_type
                                          ? *field_type.as.fn.result_full_type
                                          : type_from_kind(field_type.as.fn.result_kind);
                            if (rt.kind != TY_UNKNOWN && rt.kind != TY_NIL)
                                result_type = rt;
                        }
                        Expr *call_out = expr_new(e->arena, EX_CALL, result_type,
                                                  call->span);
                        call_out->as.call_.fn_binding = NULL;
                        call_out->as.call_.fn_expr = get_field;
                        call_out->as.call_.args = args;
                        call_out->as.call_.n_args = n_args;
                        return call_out;
                    }
                }
            }
        }
    }

    /* structdef-retirement DS-D: the "search all registered struct defs for a
     * matching :fn field" fallback (for an untyped `(.method cap ...)` receiver)
     * is removed with the StructDef registry -- every former struct is a record
     * ADT and its capability fields dispatch through the ADT field paths above. */

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
    /* M5 (docs/archive/history/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md):
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
        obj_is_unascribed_carrier_elem(obj);
    if (obj_is_abstract_tyvar) {
        TypeClassInstance *carrier_inst = NULL;
        FnDef *carrier_method = NULL;
        /* constrained-generic-instance-element-dispatch: when no `int` instance
         * exists, fall back to any *carrier-compatible scalar* instance as the
         * representative (e.g. `Enc [cstr]` for a json `Encode [Vec]` whose only
         * scalar instances are cstr/float).  Such a scalar rides the int64
         * carrier, so the polymorphic base clone stays valid C; emit-side
         * re-resolution (emit_reresolve_method_call) then specializes the inner
         * element call to the concrete A per ABI specialization.  Without this
         * representative the dispatch fell through to the generic search and a
         * tyvar receiver with >1 name-matching instance reported TUR_E0020. */
        TypeClassInstance *scalar_inst = NULL;
        FnDef *scalar_method = NULL;
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
                } else if (!scalar_inst && inst->n_type_args > 0) {
                    /* A scalar that fits the int64 carrier slot (pointer-or-word
                     * sized): cstr/bool/nil/sized-int.  Floats and aggregates do
                     * not ride the carrier and would make the base clone ill-typed,
                     * so they are not eligible representatives. */
                    TypeKind itk = inst->type_args[0].kind;
                    bool carrier_scalar =
                        (itk == TY_CSTR || itk == TY_BOOL || itk == TY_NIL ||
                         itk == TY_PTR_VOID || itk == TY_SYM ||
                         itk == TY_INT8 || itk == TY_INT16 || itk == TY_INT32 ||
                         itk == TY_INT64 ||
                         itk == TY_UINT8 || itk == TY_UINT16 || itk == TY_UINT32 ||
                         itk == TY_UINT64);
                    if (carrier_scalar) {
                        scalar_inst = inst;
                        scalar_method = inst->method_impls[i];
                    }
                }
                break; /* one method match per instance */
            }
        }
        if (!carrier_inst && scalar_inst) {
            carrier_inst = scalar_inst;
            carrier_method = scalar_method;
        }
        if (carrier_inst) {
            best_method = carrier_method;
            best_inst = carrier_inst;
            exact_match_found = true;
            goto found_method;
        }
        /* No carrier-compatible instance for this class: fall through to the
         * generic search (keeps prior behavior for classes without a
         * carrier-compatible instance; such a constrained generic would still
         * need a fix). */
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
                    /* M5 fix (docs/archive/history/m5-constrained-poly-spec-wrong-
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
                    /* Bare type-variable instance argument (whole type_args[0] is
                     * a TY_TYVAR, not a partially-applied head like
                     * `Functor [(Result _ B)]` whose *argument* is erased): this
                     * arises when an instance is registered for a type name that
                     * did not resolve to a concrete nominal type -- e.g.
                     * `Eq [str]`, where `str` has no defstruct/defdata and stays
                     * an unbound tyvar.  Left as an exact match it becomes a
                     * catch-all wildcard: every non-primitive receiver "matches"
                     * it, so the first such instance in registration order shadows
                     * the receiver's own instance, producing a silent wrong-vtable
                     * dispatch that SIGSEGVs (docs/archive/
                     * eq-bound-misdispatch-extra-instance.md: `(.eq? (Inclusive 4)
                     * (Inclusive 4))` mis-dispatched to `Eq [str]`).  When the
                     * receiver is a concrete nominal type (an ADT/struct with a
                     * def, bare or applied), demote the bare-tyvar instance to a
                     * *fallback* -- the search then continues to the receiver's
                     * concrete instance and prefers it, while a lone bare-tyvar
                     * instance still wins when it is the only candidate.  This
                     * mirrors the KIND_STAR path, where a tyvar type_args[0] never
                     * equals a primitive receiver kind and is already a fallback. */
                    if (type_ok && itk == TY_TYVAR) {
                        const Type *oh = &obj->type;
                        while (oh && oh->kind == TY_APP) oh = oh->as.app.fn;
                        /* Structs lower to TY_ADT (from_struct_lowering), so a
                         * concrete nominal head is always a TY_ADT with a def. */
                        bool recv_concrete_nominal =
                            oh && oh->kind == TY_ADT && oh->as.adt_.def;
                        if (recv_concrete_nominal) type_ok = false;
                    }
                    /* Arrow head (itk == TY_FN): an `Arrow [(->)]` instance
                     * matches only a function receiver, never a struct/vec.
                     * Conversely, a function receiver must not bind a non-arrow
                     * KIND_ARROW instance (e.g. an opaque container). */
                    if (type_ok && (itk == TY_FN || obj->type.kind == TY_FN)) {
                        type_ok = (itk == TY_FN && obj->type.kind == TY_FN);
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
                        if (oh && ih && oh->kind == TY_ADT && ih->kind == TY_ADT &&
                            oh->as.adt_.def && ih->as.adt_.def &&
                            oh->as.adt_.def != ih->as.adt_.def) {
                            heads_differ = true;
                        }
                        if (heads_differ) {
                            type_ok = false;
                        } else {
                            const Type *oa = obj->type.as.app.arg;
                            const Type *ia = inst->type_args[0].as.app.arg;
                            /* G10: discriminate on a concrete PRIMITIVE element
                             * too (cstr vs int), not only struct/ADT elements --
                             * so `Enc [(Option cstr)]` and `Enc [(Option int)]`
                             * are not conflated.  A tyvar element stays a
                             * wildcard. */
                            if (typeclass_type_arg_concrete(oa) &&
                                typeclass_type_arg_concrete(ia) &&
                                !type_eq(*oa, *ia)) {
                                type_ok = false;
                            }
                        }
                    }
                    /* CONV-S1: head-normalized nominal discrimination across the
                     * applied/bare and struct/ADT asymmetries the blocks above
                     * miss.  Once a parametric struct lowers, an ADT-app RECEIVER
                     * `(Option int)` (TY_APP, head TY_ADT) is matched against
                     * instances whose head is a *bare* ADT (`Eq [Option]`,
                     * type_arg TY_ADT) AND against still-struct heads (`:heap`
                     * `Eq [MutableMap]`, type_arg TY_STRUCT).  None of the
                     * struct/struct, app/app, or adt/adt blocks above fire on the
                     * cross-category (TY_ADT head vs TY_STRUCT head) or the
                     * applied-vs-bare pairing, so every non-primitive instance is
                     * an equally-good match and the first registered one wins
                     * (e.g. `(eq? (some 5) (some 5))` mis-dispatched to
                     * `Eq [MutableMap]`).  Walk both sides to their head; when BOTH
                     * heads are concrete nominal types (a struct/ADT with a def),
                     * require the same constructor.  A tyvar / fn / erased
                     * (NULL-def) head stays a wildcard. */
                    if (type_ok) {
                        const Type *oh = &obj->type;
                        while (oh && oh->kind == TY_APP) oh = oh->as.app.fn;
                        const Type *ih = &inst->type_args[0];
                        while (ih && ih->kind == TY_APP) ih = ih->as.app.fn;
                        bool o_adt = oh && oh->kind == TY_ADT && oh->as.adt_.def;
                        bool i_adt = ih && ih->kind == TY_ADT && ih->as.adt_.def;
                        if (o_adt && i_adt) {
                            bool same = oh->as.adt_.def == ih->as.adt_.def;
                            if (!same) type_ok = false;
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

    /* ambiguous-dispatch-error-quality + method-dispatch-missing-instance:
     * when the receiver's static type is a genuinely DISTINCT concrete type and
     * NO instance matches it exactly, the true cause is "no instance of <Class>
     * for <that type>" -- regardless of how many carrier-compatible fallbacks
     * exist.  A positive whitelist (bool / cstr / float / sized ints / a TY_ADT
     * with a real def -- structs, opaque newtypes, user ADTs), NOT merely "not a
     * tyvar".  Two bugs collapse here:
     *   - fallback_count > 1: the misleading TUR-E0020 "ambiguous / Show[?] /
     *     annotate" wording (this report).
     *   - fallback_count == 1: a SILENT bind to the single carrier-compatible
     *     representative -- a wrong-instance dispatch that SIGSEGVs when the
     *     representative's layout differs (method-dispatch-missing-instance-
     *     falls-back-to-carrier-representative.md).
     * The bare int64 carrier (:int) is EXCLUDED (a `:int` receiver is
     * indistinguishable from an erased value -- errors/hkt-dispatch-ambiguous),
     * as are abstract tyvars / carrier-erased element reads (obj_is_abstract_-
     * tyvar) and null-def ADTs (abstract-tyvar stand-ins).  EXACT matches set
     * exact_match_found and never reach here, so the recursive self-type case
     * (a Debug[Tree] body calling .debug on a Tree subfield) still resolves to
     * self. */
    {
        TypeKind rrk = obj->type.kind;
        bool concrete_distinct_receiver = !obj_is_abstract_tyvar && (
            rrk == TY_BOOL   || rrk == TY_CSTR   || rrk == TY_FLOAT  ||
            rrk == TY_FLOAT32|| rrk == TY_NIL    || rrk == TY_SYM    ||
            rrk == TY_INT8   || rrk == TY_INT16  || rrk == TY_INT32  ||
            rrk == TY_UINT8  || rrk == TY_UINT16 || rrk == TY_UINT32 ||
            rrk == TY_UINT64 ||
            (rrk == TY_ADT && obj->type.as.adt_.def != NULL));
        if (!exact_match_found && fallback_count >= 1 &&
            concrete_distinct_receiver) {
            const char *cn = best_inst ? best_inst->typeclass->name->name : "?";
            /* Attribute a macro-emitted `.method` call (e.g. derive-show's
             * `(.show (.field __p))`) to the USER's derive call site rather than
             * the macro body in stdlib/macros.tur; a direct user call keeps its
             * own receiver span.  Scoped to THIS diagnostic. */
            Span err_span = call->as.list.items[1]->span;
            bool from_macro = e->macro_expand_depth > 0;
            if (from_macro) err_span = e->macro_call_site_span;
            diag_emit_with_code(DIAG_ERROR, err_span,
                                TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                                "no instance of typeclass '%s' for type '%s' "
                                "(method '.%.*s'). Add (definstance %s [%s] ...) "
                                "or dispatch on a type that has one.",
                                cn, type_name(obj->type),
                                (int)method_name_len, method_name,
                                cn, type_name(obj->type));
            if (from_macro)
                diag_emit(DIAG_NOTE, call->as.list.items[1]->span,
                          "the '.%.*s' call is emitted by this macro expansion",
                          (int)method_name_len, method_name);
            return NULL;
        }
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
        /* Build a comma-separated list of matching instance names for the message,
         * and capture the typeclass name for the concrete-receiver diagnosis. */
        char inst_list[512];
        int pos = 0;
        int listed = 0;
        const char *class_name = NULL;
        for (TypeClassInstance *ci = e->typeclass_env.instances;
             ci != NULL && pos < (int)sizeof(inst_list) - 2; ci = ci->next) {
            for (uint8_t mi = 0; mi < ci->typeclass->n_methods; mi++) {
                const TypeClassMethod *cm = &ci->typeclass->methods[mi];
                if (cm->name->len != method_name_len ||
                    memcmp(cm->name->name, method_name, method_name_len) != 0) continue;
                if (!class_name) class_name = ci->typeclass->name->name;
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
        (void)class_name;
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
            /* Phase CCL (symmetric with the args path below): a fat closure --
             * a capturing or non-capturing lambda (boxed TY_FN) or a ptr<void>
             * closure handle -- is wrapped for tur_poly_fn_t packing rather than
             * rejected.  Reached when a typed-fn element param lands in params[0]
             * (Bifunctor `bimap [g h x]`, whose first param is a mapper fn, not
             * the HKT receiver). */
            if (obj->type.kind == TY_PTR_VOID ||
                (obj->type.kind == TY_FN && obj->type.as.fn.boxed)) {
                Expr *cwrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, obj->span);
                cwrap->as.poly_wrap_.inner = obj;
                cwrap->as.poly_wrap_.wrapper_binding = NULL;
                cwrap->as.poly_wrap_.is_closure = true;
                obj = cwrap;
            } else {
                diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                          "rank-N typeclass method argument must be a named function");
                return NULL;
            }
        } else {
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, obj->span);
            wrap->as.poly_wrap_.inner = obj;
            if (inner_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
            } else {
                uint32_t inner_arity = (inner_b->type.kind == TY_FN)
                    ? (uint8_t)inner_b->type.as.fn.arity : 1;
                Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, obj->span, false);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            obj = wrap;
        }
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
            /* constrained-hkt-byvalue-carriers: when the receiver is the ABSTRACT
             * type constructor of a constrained poly fn, this method call lowers to
             * a dictionary-slot dispatch, whose method pointer returns the int64
             * carrier -- and the instance impl invokes this `:fn` argument through
             * `((int64_t (*)(void*, int64_t))k.fn)(...)`.  A continuation returning
             * a by-value aggregate (e.g. `(fn [v] (some (dbl v)))` at `(Option
             * int)`) therefore had a struct-returning thunk cast to an
             * int64-returning pointer: an x86-64 return-ABI mismatch (RAX:RDX vs
             * RAX) that handed the instance garbage, which the caller then
             * dereferenced -- the Gap 2 segfault.
             *
             * Ask for the carrier-spill shim so the thunk boxes its aggregate
             * return, matching the carrier ABI on both sides.  Only the abstract
             * receiver opts in: a CONCRETE receiver resolves to the instance's own
             * by-value entry point and must keep consuming the struct directly,
             * which is what the existing gate protects.  The shim is itself
             * defensive -- ensure_aggregate_spill_shim returns NULL unless the
             * result really is a by-value aggregate -- so this is a no-op for
             * carrier-returning continuations. */
            {
                /* The receiver is `(m int)` -- a TY_APP spine headed by the
                 * abstract constructor -- not a bare TY_TYVAR, so walk to the
                 * head before comparing against this body's constraint var. */
                Type rcv = obj->type;
                while (rcv.kind == TY_APP && rcv.as.app.fn) rcv = *rcv.as.app.fn;
                bool rcv_is_ambient_ctor =
                    rcv.kind == TY_TYVAR && rcv.as.tyvar_.name &&
                    e->cur_hkt_constraint_tyvar &&
                    strcmp(rcv.as.tyvar_.name, e->cur_hkt_constraint_tyvar) == 0;
                if (obj_is_abstract_tyvar || rcv_is_ambient_ctor)
                    wrap->as.poly_wrap_.boxes_aggregate = true;
            }
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
                uint32_t inner_arity = (inner_b->type.kind == TY_FN)
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
                rft->kind == TY_ADT && rft->as.adt_.def &&
                rft->as.adt_.def->n_type_params == 0) {
                result_type = *rft;
            }
            /* ECS E2d-P6 (parametric associated-type element): the impl's
             * result_full_type is the instance head's own tyvar (`A`, threaded
             * above for `(definstance StorageOps [(Dense A)] (type Elem = A))`).
             * Ground it through the receiver: unify the matched instance's head
             * (`(Dense A)`) against the call-site receiver (`(Dense Pos)`) to bind
             * `A -> Pos`, then substitute into `A`.  When that grounds to a
             * non-parametric struct/ADT, commit it (with def) so a struct element
             * carries its precise type to the call site -- `(.field (storage-get
             * ...))` resolves and a struct round-trips by value through the spec
             * minted below (the abi_bindings attachment grounds the same `A`). */
            else if (rft && rft->kind == TY_TYVAR && rft->as.tyvar_.name &&
                     best_inst && best_inst->n_type_args >= 1) {
                const Symbol *hb_names[8];
                Type hb_types[8];
                uint8_t hb_n = 0;
                m7_collect_tyvar_bindings(e, best_inst->type_args[0],
                                          obj_orig_type, hb_names, hb_types,
                                          &hb_n, 8);
                if (hb_n > 0) {
                    Type grounded = elab_subst_class_tyvars(
                        e->arena, *rft, hb_names, hb_n, hb_types, hb_n);
                    if (grounded.kind == TY_ADT && grounded.as.adt_.def &&
                        grounded.as.adt_.def->n_type_params == 0) {
                        result_type = grounded;
                    }
                }
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
            for (uint32_t pi = 0; pi < best_method->n_params; pi++) {
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
    /* M6 / gap G6(c) recursive combinator: true when the method result element
     * was bound SYMBOLICALLY to an ungrounded tyvar (the enclosing generic's own
     * type param, e.g. `b -> B` for `fmap` inside `re-cata [B]`).  The result
     * does NOT ground here (B is free), but attaching the symbolic binding lets
     * the emit side compose `b -> B -> bool` once the enclosing generic is
     * monomorphized.  Kept distinct from m7_byvalue_grounded so the by-value
     * RESULT TYPE is NOT committed at elab (only the bindings are attached). */
    bool m7_symbolic_elem = false;
    /* M7 layer-4: is the instance body by-value-expressible (pure-Turmeric)?
     * Computed up front because it gates BOTH the by-value result-type commit
     * below AND the abi_bindings attachment further down -- the two MUST agree.
     * Committing a by-value result type for a CARRIER-bodied method (so the
     * consumer reads by value) without also interning the by-value spec (so the
     * producer returns by value) is exactly the carrier-vs-by-value mismatch
     * that silently miscompiles a selection body to 0 (Alternative `<|>`). */
    /* The result shape selects the by-value body criterion, and is read from the
     * CLASS method's declared return type -- not the instance binding, whose
     * result_full_type is NULL for a bare-element body (extract's `(.value w)`)
     * and only reliably TY_APP for a constructing body (fmap).  Applied `(f b)`
     * results must CONSTRUCT in-body (fmap/bind/ap/alt); a bare-element result
     * (`a`) must merely READ a scalar out of the by-value receiver (Comonad
     * `extract`).  An unmigrated stdlib class declaring a `: int` carrier return
     * classifies as neither -> stays on the uniform carrier dispatch. */
    const TypeClassMethod *m7_cm = NULL;
    if (best_inst && best_inst->typeclass) {
        TypeClass *tc0 = best_inst->typeclass;
        for (uint8_t mi = 0; mi < tc0->n_methods; mi++)
            if (tc0->methods[mi].name &&
                strcmp(tc0->methods[mi].name->name, method_name) == 0) {
                m7_cm = &tc0->methods[mi];
                break;
            }
    }
    bool m7_result_is_applied   = m7_cm && m7_cm->return_type.kind == TY_APP;
    bool m7_result_is_bare_elem = m7_cm && m7_cm->return_type.kind == TY_TYVAR;
    bool m7_body_byvalue_ok = best_method && best_method->body &&
        best_method->body->kind != EX_INLINE_C &&
        ((m7_result_is_applied &&
          m7_body_constructs_byvalue(best_method->body)) ||
         (m7_result_is_bare_elem &&
          m7_body_returns_byvalue_element(best_method->body)));
    if (best_inst && best_inst->typeclass &&
        best_method->binding->type.kind == TY_FN && m7_cm &&
        (m7_result_is_applied || m7_result_is_bare_elem)) {
        const TypeClassMethod *cm = m7_cm;
        {
            /* The declared element tyvars `(g a)` / `(fn [a] b)` live on the
             * CLASS method's param_types (parsed with the method-tyvar fix), not
             * on the instance binding (whose arg_full_types is NULL).  Substitute
             * into the binding's result_full_type when it carries the applied
             * result; for a bare-element result that field is NULL, so fall back
             * to the class method's declared return type (`a`). */
            const Type *rft = best_method->binding->type.as.fn.result_full_type;
            if (!rft) rft = &cm->return_type;
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
            /* M6 / G6(c): the method result `(f b)`'s element `b` is determined by
             * a CLOSURE argument's RESULT (the fmap/Monad shape: `g : (fn [a] b)`).
             * When that result is an ungrounded tyvar -- the enclosing generic's
             * own param `B` in `(defn re-cata [B] ... (fmap recv (fn [c] : B ...)))`
             * -- m7_collect SKIPPED it (its tyvar-actual skip guards the
             * Applicative `ap` shape, whose element comes from a wrapped VALUE arg,
             * not a closure result).  Bind it symbolically (`b -> B`) so emit can
             * compose it through the enclosing monomorphization.  Tightly gated:
             * only an APPLIED result whose element tyvar is the RESULT of a
             * fn-typed param (never `ap`, whose element sits in a value arg). */
            if (m7_result_is_applied && cm->return_type.as.app.arg &&
                cm->return_type.as.app.arg->kind == TY_TYVAR &&
                cm->return_type.as.app.arg->as.tyvar_.name) {
                const char *belem = cm->return_type.as.app.arg->as.tyvar_.name;
                bool present = false;
                for (uint8_t k = 0; k < m7_nb; k++)
                    if (m7_bind_names[k] &&
                        strcmp(m7_bind_names[k]->name, belem) == 0) { present = true; break; }
                if (!present) {
                    for (uint8_t p = 1; p < cm->n_params; p++) {
                        const Type *pt = &cm->param_types[p];
                        if (pt->kind != TY_FN) continue;
                        const Type *pr = pt->as.fn.result_full_type;
                        if (!pr || pr->kind != TY_TYVAR || !pr->as.tyvar_.name ||
                            strcmp(pr->as.tyvar_.name, belem) != 0) continue;
                        if ((uint32_t)(p - 1) >= n_args) break;
                        Type at = args_orig_types[p - 1];
                        if (at.kind != TY_FN) break;
                        Type ar = at.as.fn.result_full_type
                            ? *at.as.fn.result_full_type
                            : type_from_kind(at.as.fn.result_kind);
                        if (m7_nb < 16) {
                            m7_bind_names[m7_nb] = symtab_intern(
                                e->st, strslice(belem, (uint32_t)strlen(belem)));
                            m7_bind_types[m7_nb] = ar;
                            m7_nb++;
                            m7_symbolic_elem = true;
                        }
                        break;
                    }
                }
            }
            if (m7_nb > 0) {
                Type substituted = elab_subst_class_tyvars(
                    e->arena, *rft, m7_bind_names, m7_nb, m7_bind_types, m7_nb);
                Type ptr_family_result;
                bool ptr_family = m7_app_to_ptr_family(substituted, &ptr_family_result);
                /* hkt-inline-c-instance-body-loses-result-type: a :heap ADT is
                 * the third carrier-width result class, alongside the
                 * int-carrier newtypes below and the pointer-family handles
                 * above.  `(HBox int)` for a `:heap` parametric struct emits as
                 * `tur_adt_HBox__int *` -- a pointer, so the by-value
                 * representation IS the int64 carrier the method returns and
                 * committing the precise type needs no by-value spec.  Unlike
                 * the pointer-family case there is nothing to collapse: the
                 * applied type is already the right one. */
                bool heap_app = !ptr_family &&
                    (type_is_heap_adt(substituted) ||
                     type_is_heap_struct(substituted));
                /* hkt-inline-c-instance-body-loses-result-type: the last class,
                 * and the only one that is NOT carrier-width -- a by-value
                 * aggregate.  Committing it is safe because the CONSUMER bridges:
                 * emit_expr.c's fn_body_tail_byvalue_carrier_type reports the
                 * grounded aggregate for an inline-C-bodied dispatch call, so the
                 * existing carrier -> by-value deref runs at the binding
                 * (`Box__int m = *(Box__int *)(intptr_t)__ps_N`).  The argument
                 * was heap-spilled to reach the dict's int64 slot, so the carrier
                 * really is a pointer to the aggregate -- nothing new is
                 * generated, no wrapper and no second ABI.
                 *
                 * m7_byvalue_grounded still stays false: dispatch remains on the
                 * uniform carrier ABI and no by-value spec is minted.  That is the
                 * point -- an inline-C body cannot be re-specialized, so the
                 * PRODUCER keeps the carrier and only the consumer adapts.
                 *
                 * Recognized with type_app_is_concrete_adt, the same predicate
                 * the by-value HKT spec machinery uses for a monomorphizable
                 * parametric result.  It already excludes an opaque head (that
                 * is the int-carrier arm's job) and demands every type argument
                 * have a concrete layout, so a still-parametric result never
                 * reaches here. */
                bool byval_agg = !ptr_family && !heap_app &&
                    substituted.kind == TY_APP &&
                    type_app_is_concrete_adt(&substituted);
                /* Only commit the by-value result type (and, below, the by-value
                 * element bindings) when the result fully grounds.  A residual
                 * free element tyvar -- the `ap` fat-closure-carrier case --
                 * keeps the carrier result_type so dispatch falls back to the
                 * uniform carrier ABI instead of emitting a broken half-by-value
                 * spec with a dangling carrier-base dict reference. */
                if (!m7_type_has_free_tyvar(substituted)) {
                    if (ptr_family && result_type.kind == TY_APP) {
                        /* hkt-fmap-result-is-not-droppable: a pointer-family head
                         * (`(type-app rc<?> int)`) collapses to the concrete
                         * `rc<int>`, so `(rc/drop (fmap r f))` type-checks instead
                         * of failing on the def-less `(type-app ? ?)` shell.
                         *
                         * Checked BEFORE m7_body_byvalue_ok, not after: committing
                         * the TY_APP form for a pure-Turmeric body would leave the
                         * same un-droppable shape, and the by-value spec is not
                         * wanted here anyway -- an rc IS the carrier, so the
                         * uniform carrier dispatch is already the right ABI.
                         * m7_byvalue_grounded deliberately stays false, exactly as
                         * in the int-carrier-newtype arm below.
                         *
                         * Same `result_type.kind == TY_APP` guard as that arm: only
                         * REFINE the def-less carrier shell, never clobber an
                         * instance that overrode the class result with a concrete
                         * scalar (typeclass-instance-float-return). */
                        result_type = ptr_family_result;
                    } else if (m7_body_byvalue_ok) {
                        result_type = substituted;
                        m7_byvalue_grounded = true;
                    } else if (result_type.kind == TY_APP &&
                               (heap_app || byval_agg ||
                                m7_result_is_int_carrier(substituted))) {
                        /* method-result-functor-inference: the instance body
                         * delegates to a carrier helper (`(mk-box ...)`), so it
                         * is not by-value-constructible -- but the grounded
                         * result is an int64-carrier newtype (`(Box int)`),
                         * whose representation is ALREADY the carrier the method
                         * returns.  Commit the precise applied result type so a
                         * downstream `(un-box r)` matches `(Box A)`, WITHOUT
                         * setting m7_byvalue_grounded: dispatch stays on the
                         * uniform carrier ABI and no by-value spec is minted.
                         *
                         * Gate on `result_type.kind == TY_APP`: we only REFINE
                         * the def-less `(type-app ? ?)` carrier shell.  An
                         * instance that overrode the class's `(f b)` with a
                         * concrete scalar return (`fmap ... : float`) already
                         * has result_type == float here -- leave it alone so the
                         * divergent scalar is not clobbered by the recovered
                         * `(f int)` (typeclass-instance-float-return). */
                        result_type = substituted;
                    }
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
    /* RT1: a statically-resolved method dispatch is a crossing into whatever
     * refinements the SELECTED INSTANCE's method parameters declare.  The
     * receiver sits at argument 0 of the impl and at index 1 of the source
     * form, so the parameter/argument slots line up exactly as for a named
     * call.
     *
     * A dispatch on an ABSTRACT receiver never resolved to an instance -- it
     * fell back to an arbitrary carrier-compatible one, which is not the
     * instance that will run.  Cross into the CLASS signature there instead:
     * it is the one demand every instance honours (see
     * rt_class_method_refine_binding).  Class parameter indices line up with
     * instance ones -- both count the receiver at 0 -- so the same arg_offset
     * of 1 applies.  Either way the method's own entry check still guards the
     * call; this layer reports, it never elides. */
    const Binding *rt_cs_callee = NULL;
    if (obj_is_abstract_tyvar && best_inst && best_inst->typeclass) {
        TypeClass *rt_tc = best_inst->typeclass;
        for (uint8_t rt_mi = 0; rt_mi < rt_tc->n_methods; rt_mi++) {
            const Symbol *mn = rt_tc->methods[rt_mi].name;
            if (mn && strlen(mn->name) == method_name_len &&
                strncmp(mn->name, method_name, method_name_len) == 0) {
                rt_cs_callee = rt_class_method_refine_binding(e, rt_tc, rt_mi);
                break;
            }
        }
    }
    bool rt_is_class_cs = (rt_cs_callee != NULL);
    if (!rt_cs_callee && best_method) rt_cs_callee = best_method->binding;
    if (rt_cs_callee) {
        (void)refine_note_call_site(e, rt_cs_callee, call, 1);
        /* Reading B + lint: a STATICALLY-resolved dispatch is obliged to
         * satisfy the instance it resolved to, which is the more precise
         * contract.  Carry the class's predicates alongside so a call the
         * class would reject can be linted (TUR-W0377) without changing
         * what it must prove.  Not for a dynamic crossing -- that one IS
         * the class signature, so there is nothing to compare against. */
        if (!rt_is_class_cs && best_inst && best_inst->typeclass) {
            TypeClass *rt_tc = best_inst->typeclass;
            for (uint8_t rt_mi = 0; rt_mi < rt_tc->n_methods; rt_mi++) {
                const Symbol *mn = rt_tc->methods[rt_mi].name;
                if (!mn || strlen(mn->name) != method_name_len ||
                    strncmp(mn->name, method_name, method_name_len) != 0)
                    continue;
                if (rt_tc->methods[rt_mi].param_refine_preds)
                    refine_note_call_site_class_preds(
                        e, rt_cs_callee, call,
                        rt_tc->methods[rt_mi].param_refine_preds,
                        rt_tc->methods[rt_mi].param_refine_vars,
                        rt_tc->methods[rt_mi].n_params);
                break;
            }
        }
    }

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
    /* M4c Path A step 1 (docs/archive/m4c-execution-plan.md): bind the
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
            /* ECS E2d-P6 (parametric associated-type element): besides binding
             * the class var (`S -> (Dense Pos)`), also bind the matched
             * instance's OWN head tyvars by unifying its head (`(Dense A)`)
             * against the receiver (`(Dense Pos)`) -> `A -> Pos`.  The instance
             * method's param/return types are written in terms of `A` (the
             * `: Elem` associated member projects to it), so without these
             * bindings emit_abi_register_call sees nothing to substitute, mints
             * no by-value spec, and the struct element stays on the int64
             * carrier (`storage-get` returns int64_t, `storage-insert!` takes
             * int64_t v).  Grounding `A` lets the spec emit a by-value signature
             * and monomorphize the body's `dense-get`/`dense-set!` to the struct.
             * Inert for an int element (`A -> int` is the carrier identity) and
             * for a concrete instance head (no head tyvars to collect). */
            const Symbol *hb_names[ABI_TYPE_BINDINGS_MAX];
            Type hb_types[ABI_TYPE_BINDINGS_MAX];
            uint8_t hb_n = 0;
            if (best_inst->n_type_args >= 1)
                m7_collect_tyvar_bindings(e, best_inst->type_args[0],
                                          obj_orig_type, hb_names, hb_types,
                                          &hb_n, ABI_TYPE_BINDINGS_MAX - 1);
            uint8_t total = (uint8_t)(1 + hb_n);
            AbiTypeBinding *bindings = (AbiTypeBinding *)arena_alloc(
                e->arena, total * sizeof(AbiTypeBinding));
            bindings[0].name = tc->type_params[0]->name;
            bindings[0].type = obj_orig_type;
            for (uint8_t k = 0; k < hb_n; k++) {
                bindings[1 + k].name = hb_names[k]->name;
                bindings[1 + k].type = hb_types[k];
            }
            out->as.call_.abi_bindings = bindings;
            out->as.call_.n_abi_bindings = total;
        }
        /* M7 layer-4 prep: for an HKT class, attach the class var
         * (`g -> Option`) plus the element tyvars collected by layers 1+3
         * (`a -> int`, `b -> int`) as the dispatch call's abi_bindings, so
         * emit_abi_register_call can mint a per-(f, A) by-value instance-method
         * spec instead of falling through to the carrier-double-boxing path. */
        /* M7 layer-4: only by-value-expressible (pure-Turmeric) instance bodies
         * can be monomorphized by value.  Carrier inline-C instance bodies (the
         * current stdlib HKT instances) must stay on the uniform-carrier
         * dispatch until Phase 4.2 rewrites them; attaching the element bindings
         * to them would mint a by-value spec whose signature contradicts the
         * carrier inline-C body.  Gate on the body kind (m7_body_byvalue_ok,
         * computed up front above). */
        if (is_hkt && m7_body_byvalue_ok &&
            (m7_byvalue_grounded || m7_symbolic_elem) &&
            tc->n_type_params >= 1 && tc->type_params[0]) {
            uint8_t total = (uint8_t)(1 + m7_nb + 4);  /* +4: struct-param grounding */
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
            /* Prefer the class var's COLLECTED binding -- the receiver's
             * constructor head, recovered by m7_collect from whichever param
             * carries the class var (`(g a)` / `(p a b)`).  `obj` is the FIRST
             * arg, which is the HKT receiver only when the receiver is param 0
             * (fmap/bind/ap/extract).  For methods whose HKT receiver is NOT
             * first -- Bifunctor `bimap [g h x]`, Foldable `foldr [f z t]` --
             * obj is a function arg, so stripping its head gives the wrong type
             * (a `(fn ...)` instead of `Result`); the collected binding is right.
             * m7_collect already records the HEAD (it recurses into the TY_APP
             * fn position), so this needs no extra stripping. */
            for (uint8_t k = 0; k < m7_nb; k++) {
                if (m7_bind_names[k] &&
                    strcmp(m7_bind_names[k]->name,
                           tc->type_params[0]->name) == 0) {
                    hkt_head = m7_bind_types[k];
                    break;
                }
            }
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
