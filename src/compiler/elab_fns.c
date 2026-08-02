/* elab_fns.c -- function definition forms: defn, fn, extern-c, def. */
#include "elab_internal.h"
#include "refine_discharge.h"   /* RT3: decide a refinement obligation in place */
#include "refine_solver.h"      /* RT1: refine_model_search, for the W0377 witness */
#include "globals.h"            /* repr-trace: g_emit_abi_trace */

/* closure-drop-glue S1c: the closure-escape analysis (defined emit-side in
 * emit_core.c) is a pure walk of the shared Expr tree, reused here to infer
 * per-param non-retention when a defn is elaborated.  Forward-declared to avoid
 * pulling the whole emit-internal surface into the elaborator. */
bool closure_binding_escapes(const Expr *e, const Binding *b);
bool expr_subtree_has_inline_c(const Expr *e);
/* catch-box-reader-confinement-whitelist: the box-confinement walk, likewise
 * defined emit-side, reused here to infer per-param non-retention for
 * pointer-carrying scalars when a defn is elaborated. */
bool ptr_param_is_nonretaining(const Expr *body, const Binding *p,
                               bool result_cannot_carry);

/* closure-capture-escapes-linearity: one enclosing linear/unique binding's
 * substructural state, recorded before a lambda body is elaborated so the body's
 * effect on it can be read off afterwards.  See the snapshot in elab_fn. */
typedef struct FnCaptureLinSnap {
    Binding *binding;
    bool     was_consumed;   /* is_linear_consumed before the body */
    bool     was_moved;      /* is_moved before the body */
} FnCaptureLinSnap;

/* closure-drop-glue S1c: a scalar Copy type kind -- safe to hold in / bare-free
 * around a closure env (no owning teardown, no aliasing of the env).  Excludes
 * rc/ref/weak (owning), structs/ADTs/tyvars (aggregate/opaque), and fn/cont. */
static bool fn_result_kind_is_scalar_copy(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR: case TY_NIL:
        case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* ------------------------------------------------------------------------- *
 * RT1 (refinement-types-plan): obligation collection for a `defn`.
 *
 * Parameter refinements and `:pre` become HYPOTHESES; the return refinement
 * and `:post` become GOALS.  When a backend proves a goal, the runtime
 * contract check for it is not emitted at all -- that elision is the whole
 * point of the feature, and it is sound because the proof is over the same
 * body the check would have guarded.
 * ------------------------------------------------------------------------- */

/* Map a Turmeric type kind to the VC sort the solver reasons in.
 *
 * RM-B1: `:bool` denotes a PROPOSITION, not the integer 0/1.  Without this arm
 * a bool-returning function could not be used as a predicate atom at all --
 * `(alive? w x)` encoded Int-sorted and refine_vc_build dropped the whole
 * obligation as "does not denote a proposition". */
VCSort rt_sort_of_kind(TypeKind k) {
    switch (k) {
        case TY_FLOAT: case TY_FLOAT32: case TY_FLOAT64: return VS_REAL;
        case TY_BOOL: return VS_BOOL;
        default: return VS_INT;
    }
}

/* CT1: inject a contract entry check for each `{ v : T | pred }` parameter:
 *
 *     (tur-contract-check (let [v <param>] pred) "Contract violated")
 *
 * Shared by `defn` and `fn` so a lambda's contract parameters behave exactly
 * like a defn's rather than being silently decorative.  Returns the new body
 * (the original when nothing was injected).  Must run while the function's
 * inner scope is still current -- the predicate is elaborated in it. */
static bool rt_expr_definitely_impure(const Expr *x);

/* C2 (#reads): does this predicate reference a `#reads`-annotated measure?
 * Defined after rt_resolve_fn; forward-declared here for rt_inject_param_checks. */
static bool rt_pred_reads_measure(Elab *e, const Form *f);

/* CT1: a contract predicate whose EVALUATION does something observable.
 *
 * Checks are conditional on the build: `--no-contracts` strips them, a release
 * build drops them unless --keep-contracts, and static refinement discharge
 * elides the ones it can prove (unconditional since `refined` graduated in
 * v0.33.0).  A predicate with side effects therefore makes program
 * behaviour depend on whether its own contracts were compiled in --
 * `(>= (tick) 0)` advances a counter every time it is checked and not at all
 * when it is not.  That is a bug in the contract, not in any of those flags.
 *
 * Reported only on PROVEN impurity (`RT_P_IMPURE`), never on a predicate the
 * walk merely does not model, so a measure written with a `match` or a field
 * read is left alone.  That asymmetry is why the classifier is three-valued.
 *
 * Gate-independent on purpose: the predicate is equally wrong with the
 * refinement experiment off. */
static void rt_diag_impure_pred(Elab *e, const Expr *pred_e, Span span) {
    (void)e;
    if (!pred_e || !rt_expr_definitely_impure(pred_e)) return;
    diag_emit_with_code(DIAG_ERROR, span, TUR_E0375_REFINE_EFFECTFUL,
        "contract predicate has side effects; predicates must be pure");
    diag_emit(DIAG_NOTE, span,
        "evaluating this predicate changes program state, so whether the "
        "check is compiled in becomes observable");
}

Expr *rt_inject_param_checks(Elab *e, Expr *body, Binding *check_fn,
                                    Binding **params, uint32_t n_params,
                                    const Form **ct_preds, const char **ct_vars,
                                    const uint32_t *ct_idx, uint32_t n_ct,
                                    Span span) {
    if (!check_fn || !body) return body;
    for (uint32_t ci = 0; ci < n_ct; ci++) {
        if (ct_preds[ci] == NULL) continue;
        uint32_t pi = ct_idx[ci];
        if (pi >= n_params || !params[pi]) continue;
        /* C2 (#reads): a param whose refinement is a `#reads`-annotated measure
         * is statically checked at its CROSSINGS, never at runtime.  The measure
         * is impure by construction (that is why it needs the grant at all), so a
         * runtime entry contract for it is impossible -- it would be TUR-E0375,
         * "predicate has side effects" -- and would in any case defeat the
         * trusted-congruence grant.  Suppress the entry-check injection here; the
         * caller-side crossing obligation is the enforcement point (proven ->
         * elided; unknown -> a kept impure check that is itself E0375, so a caller
         * that cannot discharge the crossing still fails to compile).  The grant's
         * soundness is pinned by tests/fixtures/errors/refine-stateful-*. */
        if (rt_pred_reads_measure(e, ct_preds[ci])) continue;
        const char *var_nm = ct_vars[ci];

        /* Bind the contract's own variable name as an alias for the parameter,
         * unless it already IS the parameter's name. */
        Binding *cv_b = NULL;
        if (var_nm) {
            StrSlice vnsl = strslice(var_nm, (uint32_t)strlen(var_nm));
            const Symbol *cv_sym = symtab_intern(e->st, vnsl);
            Binding *existing_cv = scope_lookup(e->scope, cv_sym);
            if (!existing_cv || existing_cv != params[pi]) {
                cv_b = binding_new(e, cv_sym, params[pi]->type, false, false, span);
                scope_add(e->scope, cv_b);
            }
        }
        Expr *pred_e = elab_form(e, (Form *)ct_preds[ci]);
        if (!pred_e) continue;
        rt_diag_impure_pred(e, pred_e, span);

        Expr *check_expr = pred_e;
        if (cv_b) {
            Expr *param_var_e = expr_new(e->arena, EX_VAR, params[pi]->type, span);
            param_var_e->as.var.binding = params[pi];
            LetBinding *cv_lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
            cv_lb->binding = cv_b;
            cv_lb->init = param_var_e;
            Expr *let_cv = expr_new(e->arena, EX_LET, pred_e->type, span);
            let_cv->as.let_.bindings = cv_lb;
            let_cv->as.let_.n = 1;
            let_cv->as.let_.body = pred_e;
            check_expr = let_cv;
        }

        Expr **ck_args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        ck_args[0] = check_expr;
        Expr *ck_msg = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, span);
        ck_msg->as.s.p = "Contract violated";
        ck_msg->as.s.len = 17;
        ck_args[1] = ck_msg;
        Expr *ck_call = expr_new(e->arena, EX_CALL, TYPE_NIL, span);
        ck_call->as.call_.fn_binding  = check_fn;
        ck_call->as.call_.args        = ck_args;
        ck_call->as.call_.n_args      = 2;
        ck_call->as.call_.fn_expr     = NULL;
        ck_call->as.call_.dict_arg    = NULL;
        ck_call->as.call_.is_poly_call = false;
        ck_call->as.call_.poly_arg_mask = 0;

        Expr **do2 = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        do2[0] = ck_call;
        do2[1] = body;
        Expr *new_b = expr_new(e->arena, EX_DO, body->type, span);
        new_b->as.do_.items = do2;
        new_b->as.do_.n = 2;
        body = new_b;
    }
    return body;
}

/* CT1: wrap `body` so its RESULT is checked against `pred`:
 *
 *   (let [<var> body] (tur-contract-check pred "<msg>") <var>)
 *
 * `var_name` is the name the predicate binds the result under -- the contract's
 * own variable for a `: #refine{ r : T | p }` return, or NULL for `:post`,
 * which uses `result`.  Returns `body` unchanged when there is nothing to do.
 *
 * Shared by `defn` and typeclass instance methods.  It lives in one place
 * because the sibling peel (rt_peel_contract) had to be reached independently
 * three times before it was centralised, and this is the same shape of trap:
 * a second hand-rolled copy is a second place for a refinement to be accepted
 * and silently not enforced. */
Expr *rt_wrap_return_check(Elab *e, Expr *body, Binding *check_fn,
                           const Form *pred, const char *var_name,
                           const char *fail_msg, Span span) {
    if (!check_fn || !body || !pred) return body;
    const Symbol *bind_sym = e->sym_result;
    if (var_name)
        bind_sym = symtab_intern(e->st, strslice(var_name, (uint32_t)strlen(var_name)));

    Binding *result_b = binding_new(e, bind_sym, body->type, false, false, span);
    scope_add(e->scope, result_b);
    Expr *pred_e = elab_form(e, (Form *)pred);
    if (!pred_e) return body;
    rt_diag_impure_pred(e, pred_e, span);

    Expr **args = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = pred_e;
    Expr *msg = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, span);
    msg->as.s.p   = fail_msg;
    msg->as.s.len = (uint32_t)strlen(fail_msg);
    args[1] = msg;
    Expr *check = expr_new(e->arena, EX_CALL, TYPE_NIL, span);
    check->as.call_.fn_binding   = check_fn;
    check->as.call_.args         = args;
    check->as.call_.n_args       = 2;
    check->as.call_.fn_expr      = NULL;
    check->as.call_.dict_arg     = NULL;
    check->as.call_.is_poly_call = false;
    check->as.call_.poly_arg_mask = 0;

    Expr *result_var = expr_new(e->arena, EX_VAR, body->type, span);
    result_var->as.var.binding = result_b;
    Expr **inner = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
    inner[0] = check;
    inner[1] = result_var;
    Expr *inner_do = expr_new(e->arena, EX_DO, body->type, span);
    inner_do->as.do_.items = inner;
    inner_do->as.do_.n = 2;

    LetBinding *lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
    lb->binding = result_b;
    lb->init = body;
    Expr *let_e = expr_new(e->arena, EX_LET, body->type, span);
    let_e->as.let_.bindings = lb;
    let_e->as.let_.n = 1;
    let_e->as.let_.body = inner_do;
    return let_e;
}

/* True when contract checks are being emitted for this build.  `--no-contracts`
 * strips them, and a release build drops them unless --keep-contracts. */
bool rt_contracts_emitted(void) {
#ifdef NDEBUG
    if (!g_keep_contracts_in_release) return false;
#endif
    return !g_no_contracts;
}

/* ------------------------------------------------------------------------- *
 * RT4 purity -- a THREE-valued classification, because two questions are
 * asked of it and they want OPPOSITE conservatism.
 *
 * 1. CONGRUENCE: "may two occurrences of this call be modelled as the same
 *    value?"  Answering yes wrongly elides a real check -- `(- (tick) (tick))`
 *    with a counter-bumping `tick` encodes as `t - t`, the solver proves
 *    `t - t >= 0`, and a program that returns -1 ships unchecked.  This
 *    question needs PURE to mean proven-pure; everything else reads as impure.
 *
 * 2. DIAGNOSTICS: "is this predicate effectful, so that evaluating the check
 *    is itself part of the program's behaviour?"  Answering yes wrongly
 *    REJECTS WORKING CODE.  This question needs IMPURE to mean proven-impure;
 *    everything else must be left alone.
 *
 * These are not negations of each other, and collapsing them into one boolean
 * is exactly how a default-deny purity test turns into false errors on a
 * perfectly good measure whose body happens to use a form the walk has not
 * learned (a `match`, a field read).  The gap between them is RT_P_UNKNOWN,
 * and each caller reads it its own way.
 *
 * The DECLARED EFFECT ROW IS NOT EVIDENCE either way.  The effect system
 * tracks algebraic effects (`perform`/handlers); it infers nothing from
 * `set!`, from mutable globals, or from inline C -- effect_check.c says so
 * where it suppresses W0031 for `#fx{Unsafe}` bodies containing inline C.  A
 * function can declare `#fx{}`, carry a `static` counter in a C block, and
 * return a different value every call.
 * ------------------------------------------------------------------------- */

typedef enum RtPurity {
    RT_P_PURE = 0,   /* proven: computes a value, touches nothing        */
    RT_P_IMPURE,     /* proven: reaches a concrete effectful construct   */
    RT_P_UNKNOWN,    /* neither proven -- a form the walk does not model */
} RtPurity;

/* Combine sibling results.  IMPURE dominates (one proven effect is enough);
 * otherwise UNKNOWN dominates PURE. */
static inline RtPurity rt_p_join(RtPurity a, RtPurity b) {
    if (a == RT_P_IMPURE  || b == RT_P_IMPURE)  return RT_P_IMPURE;
    if (a == RT_P_UNKNOWN || b == RT_P_UNKNOWN) return RT_P_UNKNOWN;
    return RT_P_PURE;
}

#define RT_PURE_MAX_DEPTH 64
#define RT_PURE_MAX_NODES 20000

typedef struct RtPureCtx {
    Binding *stack[RT_PURE_MAX_DEPTH];
    uint32_t depth;
    /* Smallest stack index of an in-progress binding this subtree leaned on.
     * A result derived from an assumption about an OUTER frame is provisional
     * and must not be memoized -- that frame may still turn out impure. */
    uint32_t min_open;
    uint32_t budget;
} RtPureCtx;

static RtPurity rt_classify_expr(RtPureCtx *c, const Expr *x);

/* Builtins that compute a value from their arguments and touch nothing. */
static bool rt_builtin_shape_pure(BuiltinShape s) {
    switch (s) {
    case BS_BIN_INFIX:
    case BS_VARIADIC_FOLD:
    case BS_PREFIX_UNARY:
    case BS_AND_SC:
    case BS_OR_SC:
    case BS_DIV_CHECK:   /* may trap on /0, but a trap is not a state change */
        return true;
    default:
        return false;
    }
}

/* Builtins that definitely DO something: printing, writing memory, freeing,
 * loading libraries.  A shape in neither list is UNKNOWN, never impure. */
static bool rt_builtin_shape_impure(BuiltinShape s) {
    switch (s) {
    case BS_PRINTLN_INT:  case BS_PRINTLN_FLOAT:  case BS_PRINTLN_BOOL:
    case BS_PRINTLN_CSTR: case BS_PRINTLN_UINT:   case BS_PRINTLN_FLOAT32:
    case BS_PREFIX_UNARY_FREE:
    case BS_PTR_WRITE:    case BS_ARRAY_SET_UNCHECKED:
    case BS_RAW_MALLOC:   case BS_RAW_FREE:       case BS_RAW_REALLOC:
    case BS_RAW_MEMCPY:   case BS_RAW_MEMSET:
    case BS_DLOPEN:       case BS_DLSYM:          case BS_DLCLOSE:
        return true;
    default:
        return false;
    }
}

static RtPurity rt_classify_binding(RtPureCtx *c, Binding *b) {
    if (!b) return RT_P_UNKNOWN;
    if (b->refine_purity) return (RtPurity)(b->refine_purity - 1);
    for (uint32_t i = 0; i < c->depth; i++) {
        if (c->stack[i] == b) {
            /* Recursion: assume pure for now.  Impurity only ever enters
             * through a concrete leaf, so the greatest fixpoint is the right
             * one -- but record that we leaned on frame `i`. */
            if (i < c->min_open) c->min_open = i;
            return RT_P_PURE;
        }
    }
    FnDef *fd = b->source_fn_def;
    /* No body in hand: an extern, an inline-C-only defn whose FnDef never got
     * linked, or a forward reference not yet elaborated.  UNKNOWN -- not
     * congruent, and not diagnosable either -- and deliberately NOT memoized,
     * since the same name may be resolvable later in the unit. */
    if (!fd || !fd->body) return RT_P_UNKNOWN;
    if (c->depth >= RT_PURE_MAX_DEPTH || c->budget == 0) return RT_P_UNKNOWN;

    uint32_t my_depth  = c->depth;
    uint32_t saved_min = c->min_open;
    c->stack[c->depth++] = b;
    c->min_open = UINT32_MAX;

    RtPurity r = rt_classify_expr(c, fd->body);

    uint32_t used = c->min_open;
    c->depth--;
    if (used >= my_depth) b->refine_purity = (uint8_t)(r + 1);
    c->min_open = (used < saved_min) ? used : saved_min;
    return r;
}

static RtPurity rt_classify_expr(RtPureCtx *c, const Expr *x) {
    if (!x) return RT_P_PURE;
    if (c->budget == 0) return RT_P_UNKNOWN;
    c->budget--;

    switch (x->kind) {
    case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
    case EX_FLOAT_LIT: case EX_CSTR_LIT:
        return RT_P_PURE;

    case EX_VAR:
        /* Reading a mutable binding is not congruent -- two reads can differ --
         * but a read is not an EFFECT, so it is UNKNOWN, never impure. */
        if (!x->as.var.binding) return RT_P_UNKNOWN;
        return x->as.var.binding->is_mut ? RT_P_UNKNOWN : RT_P_PURE;

    /* Proven effects. */
    case EX_INLINE_C:
    case EX_SET: case EX_SET_DEREF:
    case EX_PERFORM:
        return RT_P_IMPURE;

    case EX_IF:
        return rt_p_join(rt_classify_expr(c, x->as.if_.cond),
                         rt_p_join(rt_classify_expr(c, x->as.if_.then_),
                                   rt_classify_expr(c, x->as.if_.else_or_null)));

    case EX_DO: {
        RtPurity r = RT_P_PURE;
        for (uint32_t i = 0; i < x->as.do_.n; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.do_.items[i]));
        return r;
    }

    case EX_LET: {
        RtPurity r = rt_classify_expr(c, x->as.let_.body);
        for (uint32_t i = 0; i < x->as.let_.n; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.let_.bindings[i].init));
        return r;
    }

    case EX_RETURN:
        return rt_classify_expr(c, x->as.return_.value);

    /* A `match` computes a value from its scrutinee and one arm; nothing about
     * dispatching on a constructor is observable.  Pattern BINDINGS are not
     * walked -- they are introduced by the pattern, not evaluated -- so an arm
     * that merely reads them stays pure.  Widening the classifier here grows
     * congruence (a measure written with a `match` is now usable as one) and
     * cannot grow TUR-E0375, since moving a form from UNKNOWN to PURE only
     * ever removes diagnostics. */
    case EX_MATCH: {
        RtPurity r = rt_classify_expr(c, x->as.match_.scrutinee);
        for (uint32_t i = 0; i < x->as.match_.n_arms; i++) {
            r = rt_p_join(r, rt_classify_expr(c, x->as.match_.arms[i].body));
            r = rt_p_join(r, rt_classify_expr(c, x->as.match_.arms[i].guard));
        }
        return r;
    }

    /* A field read is exactly as pure as its receiver.  There is no per-field
     * `mut` marker in the language, but there does not need to be one: `set!`
     * on `(.f s)` requires `s` to be bound `^mut`, so an immutable binding's
     * fields cannot be written THROUGH IT -- the same declaration-level
     * guarantee that already makes a non-mut variable read congruent.
     * Recursing on the receiver inherits that answer, and inherits IMPURE when
     * computing the receiver itself has an effect (`(.w (next-box))`).
     *
     * BEHIND A REFERENCE IT IS DECLINED.  The guarantee is about one binding,
     * not about the object: with `rc<T>` or a borrow, a caller can hold a
     * `^mut` handle to the very object the callee reads through a non-mut one,
     * and mutate it between two calls -- which is precisely the aliasing that
     * congruence assumes away.  A by-value receiver has no second handle to
     * mutate through (`:copy` copies, and a moved value leaves the caller
     * nothing), so the vector closes by construction.  Declining costs
     * precision on `rc` getters and nothing else.  Three congruence
     * miscompiles on this feature have come from assuming an aliasing
     * question away; this one is answered instead. */
    case EX_GET_FIELD: {
        const Expr *recv = x->as.get_field_.struct_expr;
        if (!recv) return RT_P_UNKNOWN;
        switch (recv->type.kind) {
            case TY_REF: case TY_RC: case TY_PTR_VOID:
            case TY_REF_IMMUT: case TY_REF_MUT:
                return RT_P_UNKNOWN;
            default: break;
        }
        return rt_classify_expr(c, recv);
    }

    case EX_BUILTIN: {
        if (!x->as.builtin.spec) return RT_P_UNKNOWN;
        BuiltinShape sh = x->as.builtin.spec->shape;
        RtPurity r = rt_builtin_shape_impure(sh) ? RT_P_IMPURE
                   : rt_builtin_shape_pure(sh)   ? RT_P_PURE
                                                 : RT_P_UNKNOWN;
        for (uint32_t i = 0; i < x->as.builtin.n; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.builtin.args[i]));
        return r;
    }

    case EX_CALL: {
        /* An indirect call (`fn_expr`) or a rank-2 poly call can land
         * anywhere, so there is no callee to interrogate. */
        RtPurity r = (x->as.call_.fn_expr || x->as.call_.is_poly_call)
                   ? RT_P_UNKNOWN
                   : rt_classify_binding(c, x->as.call_.fn_binding);
        for (uint32_t i = 0; i < x->as.call_.n_args; i++)
            r = rt_p_join(r, rt_classify_expr(c, x->as.call_.args[i]));
        return r;
    }

    default:
        /* A form the walk does not model -- match, field reads, closures,
         * while, STM, async, panic.  Neither proven pure nor proven
         * effectful, and that distinction is the whole point. */
        return RT_P_UNKNOWN;
    }
}

static RtPurity rt_classify_binding_top(Binding *b) {
    if (!b) return RT_P_UNKNOWN;
    if (b->refine_purity) return (RtPurity)(b->refine_purity - 1);
    /* A declared non-empty effect row rules PURE out.  It does not prove
     * IMPURE -- the row can name an effect the body never performs -- so it
     * only ever downgrades to UNKNOWN. */
    bool row_veto = (b->type.kind == TY_FN && b->type.as.fn.effect_row &&
                     !effect_row_is_empty(b->type.as.fn.effect_row));
    RtPureCtx c = { { 0 }, 0, UINT32_MAX, RT_PURE_MAX_NODES };
    RtPurity r = rt_classify_binding(&c, b);
    if (row_veto && r == RT_P_PURE) return RT_P_UNKNOWN;
    return r;
}

/* True when calling `b` twice with equal arguments is guaranteed to produce
 * equal results with no observable side effect.  UNKNOWN reads as "no". */
static bool rt_binding_is_pure(Binding *b) {
    return rt_classify_binding_top(b) == RT_P_PURE;
}

/* CT1/RT: does evaluating this elaborated predicate DO something observable?
 * Only a proven effect counts -- an unrecognised form answers false, so a
 * predicate the walk cannot model is never diagnosed and never blocks
 * elision on suspicion alone. */
static bool rt_expr_definitely_impure(const Expr *x) {
    RtPureCtx c = { { 0 }, 0, UINT32_MAX, RT_PURE_MAX_NODES };
    return rt_classify_expr(&c, x) == RT_P_IMPURE;
}

/* RT4: resolve a called function's return refinement for the encoder.  Owned
 * by the elaborator because it is the only side that can look a name up in the
 * global scope; refine_collect.c reaches it through a function pointer so it
 * stays free of scope/binding knowledge. */
bool rt_resolve_fn(void *ud, const char *name, RefineFnInfo *out) {
    Elab *e = (Elab *)ud;
    if (!e || !name) return false;
    /* No flag test here on purpose.  A refinement only reaches
     * `refine_return_pred` when something actually enforces it -- it was proved
     * statically, or the runtime check that guarantees it is being emitted --
     * and that decision is made where the callee is elaborated, which is the
     * only place that knows whether contracts survived for it.  Re-testing
     * `--no-contracts` here would also throw away INFERRED refinements, which
     * are proved facts and hold whether or not any check is emitted. */
    const Symbol *sym = symtab_intern(e->st, strslice(name, (uint32_t)strlen(name)));

    /* A DATA CONSTRUCTOR IS PURE BY CONSTRUCTION.  It stores its arguments and
     * runs no user code, so two applications to equal arguments hold equal
     * fields -- which is the only thing the VC ever asks, since a constructor
     * term is only ever reached through a selector.  (Object IDENTITY differs
     * between two applications, but nothing in the predicate language can
     * observe it.)
     *
     * Answered before the binding lookup because a constructor DOES have a
     * global binding and no `defn` body, so the default-deny body walk finds
     * no evidence and lands on UNKNOWN -- which congruence reads as impure.
     * That gave every occurrence its own symbol and made the constructor
     * axioms below inert: the `Box(p,3)` in the axiom and the `Box(p,3)` in
     * the goal were different terms. */
    if (elab_lookup_ctor(e, sym)) {
        out->ret_pred    = NULL;
        out->ret_var     = NULL;
        out->param_names = NULL;
        out->n_params    = 0;
        out->pure        = true;
        /* A constructor yields an aggregate handle -- an opaque Int term, the
         * only thing the predicate language can say about it. */
        out->ret_sort    = VS_INT;
        return true;
    }

    Binding *b = scope_lookup(&e->global, sym);
    if (!b) {
        /* A TYPECLASS METHOD has no global binding under its bare name -- the
         * dispatch is resolved elsewhere -- so it used to fall through to the
         * abstract-measure rule below and pick up congruence for free.  That is
         * the same miscompile as the `tick` case, reached through a third door:
         * an instance method that counts up made `(- (tickm 1) (tickm 1))`
         * encode as `t - t`, the solver proved `t - t >= 0`, and the runtime
         * check that would have caught -1 was elided.
         *
         * A method is code that runs.  Report it as a known callee with NO
         * purity, so each occurrence gets its own symbol.  The class's result
         * refinement is deliberately NOT published here: which instance runs is
         * not known at the encoder, and the enforcement question is
         * per-instance. */
        /* Both dispatch spellings reach here: the bare `(m x)` as `m`, and the
         * dotted `(.m x)` as `.m`.  Look the dotted one up under its method
         * name -- checking only `sym` fixed the bare form and left the dotted
         * one still congruent, which the fuzzer caught two seeds later. */
        const Symbol *msym = sym;
        if (name[0] == '.' && name[1] != '\0')
            msym = symtab_intern(e->st, strslice(name + 1,
                                                 (uint32_t)strlen(name) - 1));
        const TypeClass *owner = NULL;
        const TypeClassMethod *m =
            typeclass_env_find_method(&e->typeclass_env, msym, &owner);
        if (m) {
            out->ret_pred    = NULL;
            out->ret_var     = NULL;
            out->param_names = NULL;
            out->n_params    = 0;
            out->pure        = false;
            /* RM-B1: the class signature's declared result type is the one
             * promise true of every instance, so its sort is the dispatch's. */
            out->ret_sort    = rt_sort_of_kind(m->return_type.kind);
            /* RT4: the CLASS's result refinement propagates, even though which
             * instance runs is unknown here -- because it is the one promise
             * true of EVERY instance.  Result variance is what buys that: an
             * instance either inherits the class predicate (and is checked
             * against it) or restates one, and then either its own is proved
             * to imply the class's or the class's is checked alongside it.
             *
             * Gated on the check surviving, matching `rt_ret_guaranteed` on the
             * `defn` path: `--no-contracts` removes the thing that enforces
             * this, so the fact must go with it. */
            if (m->return_refine_pred && rt_contracts_emitted()) {
                out->ret_pred = m->return_refine_pred;
                out->ret_var  = m->return_refine_var;
                /* Parameter names let a predicate that mentions a parameter be
                 * substituted with what the call site supplied.  The receiver
                 * is slot 0 of both the class signature and the dispatch, so
                 * the slots line up. */
                if (m->param_names && m->n_params) {
                    const char **pn = (const char **)arena_alloc(
                        e->arena, m->n_params * sizeof(char *));
                    for (uint8_t i = 0; i < m->n_params; i++)
                        pn[i] = m->param_names[i] ? m->param_names[i]->name : NULL;
                    out->param_names = pn;
                    out->n_params    = m->n_params;
                }
            }
            return true;
        }
        /* Genuinely nothing: an abstract measure -- an uninterpreted
         * mathematical function, which the language defines as congruent. */
        return false;
    }

    out->ret_pred    = b->refine_return_pred;
    out->ret_var     = b->refine_return_var;
    out->param_names = b->refine_param_names;
    out->n_params    = b->n_refine_params;

    /* RM-B1: the sort the measure symbol is declared at.  `result_kind` is the
     * declared return type PEELED to its base, which is exactly what the VC
     * reasons over -- a `: #refine{ r : float | q }` return is a Real, and the
     * `q` is carried separately in ret_pred.  A non-fn binding (a `def` used as
     * a nullary measure) answers from its own type. */
    out->ret_sort = rt_sort_of_kind(b->type.kind == TY_FN ? b->type.as.fn.result_kind
                                                          : b->type.kind);

    /* C2 / #reads: publish the read-frame param so the encoder can grant
     * congruence when that argument is frozen at the call site. */
    out->reads_param_plus1 = b->reads_param_plus1;

    /* PURITY, which decides whether two occurrences of this call may be
     * modelled as the same value.  See rt_binding_is_pure above: the declared
     * effect row is only a veto, the real evidence is a default-deny walk of
     * the callee's body. */
    out->pure = rt_binding_is_pure(b);
    return true;
}

/* The resolver, for callers outside this file that build their own env. */
RefineFnResolver rt_refine_resolver(Elab *e) { (void)e; return rt_resolve_fn; }

/* Build the hypothesis environment visible at a function's return point. */
static RefineEnv *rt_build_env(Elab *e, Binding **params, uint32_t n_params,
                               const Form **ct_param_preds,
                               const char **ct_param_varnames,
                               const uint32_t *ct_param_param_idx,
                               uint32_t n_ct_param_preds,
                               const Form *ct_pre_form) {
    RefineEnv *env = refine_env_new(e->arena);
    refine_env_set_resolver(env, rt_resolve_fn, e);
    for (uint32_t i = 0; i < n_params; i++) {
        if (!params[i] || !params[i]->name) continue;
        refine_env_declare(env, params[i]->name->name,
                           rt_sort_of_kind(params[i]->type.kind));
    }
    for (uint32_t i = 0; i < n_ct_param_preds; i++) {
        if (!ct_param_preds[i]) continue;
        uint32_t pi = ct_param_param_idx[i];
        if (pi >= n_params || !params[pi] || !params[pi]->name) continue;
        refine_env_push(env, ct_param_preds[i], ct_param_varnames[i],
                        params[pi]->name->name);
    }
    /* A `:pre` predicate mentions the parameters directly -- nothing to
     * rename, so it rides in with a NULL bound variable. */
    if (ct_pre_form) refine_env_push(env, ct_pre_form, NULL, NULL);
    return env;
}

/* ------------------------------------------------------------------------- *
 * RT4: template-based predicate propagation.
 *
 * A deliberately small convenience, NOT the general refinement inference the
 * plan rules out.  We do not search for a predicate that satisfies a recursive
 * constraint system (that is the LiquidHaskell layer we deleted by making
 * refinements written rather than inferred); we try a fixed vocabulary of
 * shapes against the body and keep the first the solver can prove.  Wrong
 * guesses cost a discarded proof attempt, never a wrong answer.
 * ------------------------------------------------------------------------- */

#define RT4_RESULT_VAR "__rt_result"

/* Build the predicate Form `(<op> __rt_result 0)`. */
static const Form *rt4_template(Elab *e, const char *op, Span span, bool is_float) {
    Form **items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    items[0] = form_sym(e->arena, span,
                        symtab_intern(e->st, strslice(op, (uint32_t)strlen(op))));
    items[1] = form_sym(e->arena, span,
                        symtab_intern(e->st, strslice(RT4_RESULT_VAR,
                                                      (uint32_t)strlen(RT4_RESULT_VAR))));
    items[2] = is_float ? form_float(e->arena, span, 0.0)
                        : form_int(e->arena, span, 0);
    return form_list(e->arena, span, items, 3);
}

/* Try each shape against `subject` under `env`; return the first one a backend
 * proves, or NULL.  Silent: these are probes, not obligations the user wrote,
 * so a failed one reports nothing and counts for nothing. */
static const Form *rt4_infer_return(Elab *e, const Form *subject, TypeKind ret_kind,
                                    RefineEnv *env, Span span) {
    if (!subject || !env) return NULL;
    bool is_float = (ret_kind == TY_FLOAT || ret_kind == TY_FLOAT32 ||
                     ret_kind == TY_FLOAT64);
    /* Ordered most-informative first, so `(> r 0)` wins over `(>= r 0)` when
     * both hold. */
    static const char *const SHAPES[] = { ">", "<", ">=", "<=", "not=" };
    for (size_t i = 0; i < sizeof(SHAPES) / sizeof(SHAPES[0]); i++) {
        const Form *tmpl = rt4_template(e, SHAPES[i], span, is_float);
        RefineObligation probe;
        memset(&probe, 0, sizeof(probe));
        probe.predicate   = tmpl;
        probe.var_name    = RT4_RESULT_VAR;
        probe.subject     = subject;
        probe.base_sort   = rt_sort_of_kind(ret_kind);
        probe.loc         = span;
        probe.env         = env;
        probe.what        = "inferred result refinement";
        probe.speculative = true;
        if (refine_discharge_one(&probe, e->arena)) return tmpl;
    }
    return NULL;
}

/* Collect one return-position obligation and decide it now.  Returns true when
 * a backend proved it, so the caller can skip injecting the runtime check. */
/* True when evaluating `pred` would do something observable -- it calls a
 * function that is not known pure.
 *
 * Eliding a check is only invisible when running the check was invisible.  A
 * predicate like `(>= (tick) 0)`, where `tick` bumps a counter, makes the
 * check itself part of the program's behaviour: with the check emitted the
 * counter advances, without it the counter does not, and the program prints
 * different numbers.  The fuzzer found exactly that as an OUTPUT DIVERGENCE
 * (not a soundness bug -- both runs were internally consistent, they just
 * disagreed).
 *
 * An impure predicate is a mistake in its own right -- `TUR-E0375` exists for
 * it and nothing emits it, so a contract whose evaluation has side effects is
 * accepted today and already behaves differently under `--no-contracts`. That
 * is a contract-layer issue and is reported separately. What this feature owes
 * is narrower and absolute: turning the gate on must not change behaviour, so
 * a check guarding an impure predicate is never elided. */
static bool rt_pred_is_impure(Elab *e, const Form *f) {
    if (!f) return false;
    if (f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && head->as.sym) {
        RefineFnInfo info;
        memset(&info, 0, sizeof(info));
        /* A name that resolves to nothing is an abstract measure -- a
         * mathematical function, so nothing runs.  Builtins (`+`, `and`, ...)
         * land there too, which is correct: they are pure. */
        if (rt_resolve_fn(e, head->as.sym->name, &info) && !info.pure)
            return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_pred_is_impure(e, f->as.list.items[i])) return true;
    return false;
}

/* C2 (#reads): does this predicate reference a `#reads`-annotated measure?
 *
 * Mirrors rt_pred_is_impure, but keys on the reads grant rather than impurity:
 * a measure declared `#reads w` resolves to a RefineFnInfo with a non-zero
 * reads_param_plus1.  rt_inject_param_checks uses this to skip the (impossible)
 * runtime entry contract for such a param -- see the comment at its call site.
 *
 * The recursive walk matches a `#reads` measure anywhere in a compound
 * predicate (e.g. `(and (alive? w x) ...)`): the entry contract must be
 * suppressed as a whole, since the impure sub-term alone makes it unemittable. */
static bool rt_pred_reads_measure(Elab *e, const Form *f) {
    if (!f) return false;
    if (f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && head->as.sym) {
        RefineFnInfo info;
        memset(&info, 0, sizeof(info));
        if (rt_resolve_fn(e, head->as.sym->name, &info) &&
            info.reads_param_plus1 != 0)
            return true;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_pred_reads_measure(e, f->as.list.items[i])) return true;
    return false;
}

/* ------------------------------------------------------------------------- *
 * RT4 branching bodies: prove a return obligation PER PATH.
 *
 * The obligation is `pred[body/r]`.  When the body is a single expression the
 * encoder handles it directly, but `(if c t e)` is not an arithmetic term --
 * there is no value to substitute -- so the whole obligation answered unknown
 * and the check stayed.  That rules out most real function bodies.
 *
 * Splitting recovers them.  `(if c t e)` is discharged when BOTH
 *
 *     c  |- pred[t/r]        and        (not c) |- pred[e/r]
 *
 * hold, which is exactly the path-sensitive reading: on each path the value
 * returned is that branch's, and the condition that selected it is a fact.
 * `(let [x v] body)` adds `x = v` and recurses into the body, so a body that
 * names its result still gets checked.
 *
 * Every sub-obligation is discharged SILENTLY.  A failure here is not a
 * diagnostic -- it means this strategy did not work, and the caller falls back
 * to the ordinary whole-body obligation, which reports in the ordinary place
 * with the ordinary message.  So splitting can only ever prove MORE; it never
 * changes what an unproven function looks like.
 * ------------------------------------------------------------------------- */

#define RT_PATH_MAX_DEPTH 8

/* Discharge one path's obligation without reporting anything. */
static bool rt_prove_silent(Elab *e, const Form *pred, const char *var_name,
                            const Form *subject, TypeKind base_kind,
                            RefineEnv *env, Span loc) {
    if (!pred || !subject) return false;
    RefineObligation *ob = refine_collect_obligation(
        &e->refine_obs, pred, var_name, subject, rt_sort_of_kind(base_kind),
        type_name(type_simple(base_kind, CK_COPY)), loc, env, "path", NULL);
    if (!ob) return false;
    ob->speculative = true;
    ob->path_probe  = true;
    return refine_discharge_one(ob, e->arena);
}

/* True when `f` is a list whose head is the symbol `name`. */
static bool rt_head_is(const Form *f, const char *name) {
    if (!f || f->tag != F_LIST || f->as.list.len == 0) return false;
    const Form *h = f->as.list.items[0];
    if (h->tag != F_SYM || !h->as.sym) return false;
    return strcmp(h->as.sym->name, name) == 0;
}

static bool rt_prove_paths(Elab *e, const Form *pred, const char *var_name,
                           const Form *subject, TypeKind base_kind,
                           RefineEnv *env, Span loc, uint32_t depth);

/* True when `f` contains an assignment anywhere.  Used to decide whether a
 * statement can be stepped over: an assignment may stale a hypothesis already
 * in the environment, and every other form cannot.  Deliberately syntactic and
 * deliberately over-broad -- a name that merely LOOKS like an assignment costs
 * precision, and missing one costs soundness. */
/* 24, matching RT_CS_PATH_MAX_DEPTH: the scan is linear in nodes either way,
 * and a macro EXPANSION is legitimately deeper than the source that spells it
 * -- a 12 limit made rt_form_mentions_set return its conservative "too deep,
 * assume assignment" answer on a clean for-each expansion, spuriously
 * declining every crossing in the caller. */
#define RT_SET_SCAN_MAX_DEPTH 24

static const Form *rt_macro_expansion(const Elab *e, const Form *call);

static bool rt_form_mentions_set(const Elab *e, const Form *f, uint32_t depth) {
    if (!f) return false;
    if (depth >= RT_SET_SCAN_MAX_DEPTH) return true;   /* too deep to vouch for */
    if (f->tag == F_SYM && f->as.sym) {
        const char *n = f->as.sym->name;
        return strcmp(n, "set!") == 0 || strcmp(n, "swap!") == 0 ||
               strcmp(n, "reset!") == 0;
    }
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    /* A macro call scans as its EXPANSION: the source spelling shows no
     * `set!`, but the code that runs is the expansion, and an assignment
     * hidden in a template must decline exactly like a written one.  The hop
     * does NOT consume depth -- it is a lateral move to fresh nodes, not
     * structural nesting, and cannot cycle (an expansion's nodes are newly
     * constructed; only spliced ARGUMENTS are reused, and a call form cannot
     * be its own argument). */
    {
        const Form *mx = rt_macro_expansion(e, f);
        if (mx) return rt_form_mentions_set(e, mx, depth);
    }
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (rt_form_mentions_set(e, f->as.list.items[i], depth + 1)) return true;
    return false;
}

/* ------------------------------------------------------------------------- *
 * A datatype theory for match arms.
 *
 * The VC term language has no constructors and no selectors, and it is not
 * getting any: adding sorts and theory solvers for algebraic data would be a
 * far larger change than what a match arm actually needs.  What it needs is a
 * way to SAY "on this path the scrutinee is a Circle, and `r` is its radius",
 * and that is expressible today -- as ordinary equations over UNINTERPRETED
 * functions, which S1 already decides by congruence closure.
 *
 * So the theory is synthesized at the FORM level, before encoding:
 *
 *   (Circle r)  on scrutinee `s`   ==>   (= (#dt/tag s) 3)
 *                                        (= r (.radius s))
 *
 * `#dt/tag` is a total function from the datatype to its discriminant, which is
 * exactly what `CtorDef.tag` numbers.  A field selector is a total function too
 * -- undefined-in-principle off its own constructor, but an uninterpreted
 * symbol is free to take any value there, and this arm only ever asserts the
 * case where it is defined.  Both facts are true on the path being proved, so
 * asserting them is sound; the solver needs no new theory to use them, because
 * congruence over an uninterpreted symbol is the whole of what they require.
 *
 * THE SYNTHETIC NAME MUST NOT BE SPELLABLE.  `enc_measure` resolves a head name
 * to decide purity, and a name that resolves to a real PURE function would have
 * `(= (#dt/tag s) 3)` assert something about THAT function -- a hypothesis that
 * can be false, and a false hypothesis proves the goal and elides the check.
 * A leading `#` is reader dispatch, so no source symbol can begin with one;
 * the name therefore resolves to nothing, which the encoder reads as an
 * abstract measure -- uninterpreted and congruent, which is what it is.
 * ------------------------------------------------------------------------- */

#define RT_DT_TAG_FN "#dt/tag"

static Form *rt_form_call1(Elab *e, Span sp, const char *fn, const Form *arg) {
    Form **k = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    k[0] = form_sym(e->arena, sp,
                    symtab_intern(e->st, strslice(fn, (uint32_t)strlen(fn))));
    k[1] = (Form *)arg;
    return form_list(e->arena, sp, k, 2);
}

static Form *rt_form_eq(Elab *e, Span sp, const Form *a, const Form *b) {
    Form **k = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    k[0] = form_sym(e->arena, sp, symtab_intern(e->st, strslice("=", 1)));
    k[1] = (Form *)a;
    k[2] = (Form *)b;
    return form_list(e->arena, sp, k, 3);
}

/* True when `f` is exactly the symbol `name`. */
static bool rt_sym_is(const Form *f, const char *name) {
    return f && f->tag == F_SYM && f->as.sym &&
           strcmp(f->as.sym->name, name) == 0;
}

/* The constructor a pattern names, or NULL when the pattern is not a
 * constructor application (a literal, a wildcard, a bare binder). */
static CtorDef *rt_pat_ctor(Elab *e, const Form *pat) {
    if (!pat || pat->tag != F_LIST || pat->as.list.len == 0) return NULL;
    const Form *h = pat->as.list.items[0];
    if (h->tag != F_SYM || !h->as.sym) return NULL;
    return elab_lookup_ctor(e, h->as.sym);
}

/* Copy `f`, renaming every FREE occurrence of `from` to `to`.
 *
 * Path splitting asserts `x = v` in the environment's single flat namespace,
 * so a `let` that rebinds a name already in scope would assert
 * `x = <something mentioning x>` -- a contradiction, which proves the goal and
 * elides a check that was protecting something.  Renaming the binding is what
 * makes those bodies splittable instead of skipped.
 *
 * Returns NULL when the body REBINDS the same name again, which would need a
 * second rename to stay correct; declining is the safe answer and the caller
 * falls back to the whole-body obligation. */
static Form *rt_rename_free(Elab *e, const Form *f,
                            const Symbol *from, const Symbol *to) {
    if (!f) return NULL;
    if (f->tag == F_SYM)
        return (f->as.sym == from)
             ? form_sym(e->arena, f->span, to) : (Form *)f;
    if (f->tag != F_LIST && f->tag != F_VEC) return (Form *)f;

    /* A nested binder of the same name: decline rather than guess. */
    if (f->tag == F_LIST && f->as.list.len >= 2 &&
        f->as.list.items[0]->tag == F_SYM &&
        f->as.list.items[0]->as.sym &&
        strcmp(f->as.list.items[0]->as.sym->name, "let") == 0) {
        const Form *b = f->as.list.items[1];
        if (b && b->tag == F_VEC)
            for (uint32_t i = 0; i + 1 < b->as.list.len; i += 2)
                if (b->as.list.items[i]->tag == F_SYM &&
                    b->as.list.items[i]->as.sym == from)
                    return NULL;
    }

    uint32_t n = f->as.list.len;
    Form **kids = (Form **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Form *));
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        kids[i] = rt_rename_free(e, f->as.list.items[i], from, to);
        if (!kids[i]) return NULL;
        if (kids[i] != f->as.list.items[i]) changed = true;
    }
    if (!changed) return (Form *)f;
    return f->tag == F_LIST ? form_list(e->arena, f->span, kids, n)
                            : form_vec(e->arena, f->span, kids, n);
}

/* Run `body` with `hyp` assumed.  The env's hypothesis list is a cons chain
 * and its name table only ever grows, so a scope is saved and restored by
 * rewinding both -- no copying. */
static bool rt_prove_under(Elab *e, const Form *pred, const char *var_name,
                           const Form *hyp, const Form *body,
                           TypeKind base_kind, RefineEnv *env, Span loc,
                           uint32_t depth) {
    RefineHyp *saved_head  = env->head;
    uint32_t   saved_names = env->n_names;
    if (hyp) refine_env_push(env, hyp, NULL, NULL);
    bool ok = rt_prove_paths(e, pred, var_name, body, base_kind, env, loc, depth + 1);
    env->head    = saved_head;
    env->n_names = saved_names;
    return ok;
}

static bool rt_prove_paths(Elab *e, const Form *pred, const char *var_name,
                           const Form *subject, TypeKind base_kind,
                           RefineEnv *env, Span loc, uint32_t depth) {
    if (!subject || depth >= RT_PATH_MAX_DEPTH) return false;

    /* (if c t e) -- both arms, each under its own path condition.  A one-armed
     * `if` has no value on the false path and is left alone. */
    if (rt_head_is(subject, "if") && subject->as.list.len == 4) {
        const Form *c = subject->as.list.items[1];
        const Form *t = subject->as.list.items[2];
        const Form *f = subject->as.list.items[3];
        Form **notk = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
        notk[0] = form_sym(e->arena, subject->span,
                           symtab_intern(e->st, strslice("not", 3)));
        notk[1] = (Form *)c;
        Form *nc = form_list(e->arena, subject->span, notk, 2);
        return rt_prove_under(e, pred, var_name, c,  t, base_kind, env, loc, depth) &&
               rt_prove_under(e, pred, var_name, nc, f, base_kind, env, loc, depth);
    }

    /* (let [x v] body) -- one binding, one body form.  `x = v` is the fact the
     * body's own obligation needs.  Wider shapes fall through rather than
     * guess which binding the result depends on. */
    if (rt_head_is(subject, "let") && subject->as.list.len == 3) {
        const Form *binds = subject->as.list.items[1];
        const Form *body  = subject->as.list.items[2];
        if (binds && binds->tag == F_VEC && binds->as.list.len == 2 &&
            binds->as.list.items[0]->tag == F_SYM) {
            const Form *x = binds->as.list.items[0];
            const Form *v = binds->as.list.items[1];
            /* SHADOWING IS A CONTRADICTION HERE.  The hypothesis is `x = v` in
             * the environment's single flat namespace, so rebinding a name
             * that is already in scope asserts `x = <something mentioning x>`.
             * `(let [x (- x 1)] x)` becomes `x = x - 1`, which is false for
             * every x -- and a false hypothesis proves the goal, so the check
             * gets elided and the function returns a value violating its own
             * refinement.  That is a miscompile, and this shape is ordinary
             * code, not a corner case.
             *
             * Declining to split is the fix: the whole-body obligation still
             * runs and still answers unknown, exactly as before path
             * splitting existed.  Alpha-renaming the binding would recover
             * these bodies and is the natural follow-up. */
            /* SHADOWING.  The hypothesis is `x = v` in one flat namespace,
             * so rebinding a name already in scope would assert
             * `x = <something mentioning x>` -- `(let [x (- x 1)] x)` becomes
             * `x = x - 1`, false for every x, and a false hypothesis proves
             * the goal.  That was a live miscompile.
             *
             * Alpha-rename instead of declining: the binding becomes a fresh
             * name, the body is rewritten to use it, and `v` keeps referring
             * to the OUTER x because only the body is renamed. */
            bool shadows = false;
            for (uint32_t _i = 0; _i < env->n_names; _i++)
                if (env->names[_i] &&
                    strcmp(env->names[_i], x->as.sym->name) == 0)
                    { shadows = true; break; }
            if (shadows) {
                char fresh[96];
                snprintf(fresh, sizeof(fresh), "%s~%u", x->as.sym->name, depth);
                const Symbol *fs = symtab_intern(
                    e->st, strslice(arena_strdup(e->arena, fresh, strlen(fresh)),
                                    (uint32_t)strlen(fresh)));
                Form *nx = form_sym(e->arena, x->span, fs);
                Form *nbody = rt_rename_free(e, body, x->as.sym, fs);
                if (!nbody) return false;
                x = nx; body = nbody;
            }
            refine_env_declare(env, x->as.sym->name, rt_sort_of_kind(base_kind));

            /* A BRANCHING VALUE splits too.  `(let [m (if c a b)] ...)` used to
             * assert `m = (if c a b)`, and an `if` is not a term the encoder
             * can build -- so the hypothesis was dropped and `m` went into the
             * body completely unconstrained.  `(let [m (if (<= a b) a b)] m)`
             * against `(<= r a)` was Unknown while the identical `if` written
             * directly as the body proved, which is a distinction no one
             * writing the code would expect to matter.
             *
             * Splitting the VALUE is the same rule as splitting the body, one
             * level up: on the true path `m = a`, on the false path `m = b`,
             * and the branch condition is a fact on each. */
            if (rt_head_is(v, "if") && v->as.list.len == 4 &&
                depth + 1 < RT_PATH_MAX_DEPTH) {
                const Form *c  = v->as.list.items[1];
                const Form *vt = v->as.list.items[2];
                const Form *vf = v->as.list.items[3];
                Form **notk = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
                notk[0] = form_sym(e->arena, v->span,
                                   symtab_intern(e->st, strslice("not", 3)));
                notk[1] = (Form *)c;
                Form *nc = form_list(e->arena, v->span, notk, 2);

                RefineHyp *br_head  = env->head;
                uint32_t   br_names = env->n_names;
                refine_env_push(env, c, NULL, NULL);
                bool ok_t = rt_prove_under(e, pred, var_name,
                                           rt_form_eq(e, v->span, x, vt),
                                           body, base_kind, env, loc, depth + 1);
                env->head = br_head; env->n_names = br_names;
                if (!ok_t) return false;

                refine_env_push(env, nc, NULL, NULL);
                bool ok_f = rt_prove_under(e, pred, var_name,
                                           rt_form_eq(e, v->span, x, vf),
                                           body, base_kind, env, loc, depth + 1);
                env->head = br_head; env->n_names = br_names;
                return ok_f;
            }

            return rt_prove_under(e, pred, var_name,
                                  rt_form_eq(e, subject->span, x, v),
                                  body, base_kind, env, loc, depth);
        }
        return false;
    }

    /* (do s1 ... sn) -- the value is the LAST form; the rest are statements.
     *
     * Splitting on the last form is only sound while the statements cannot
     * invalidate the hypotheses already in the environment. Those hypotheses
     * are about parameters, so an assignment is exactly what would stale them:
     * `(do (set! x 0) (if (>= x 0) x 0))` must not be proved using the
     * parameter refinement that held BEFORE the assignment. A Form-level scan
     * for any `set!` in the statements is conservative -- it declines some
     * harmless writes to purely local state -- and cheap, which is the right
     * trade for a rule whose failure mode is a miscompile. */
    if (rt_head_is(subject, "do") && subject->as.list.len >= 2) {
        for (uint32_t i = 1; i + 1 < subject->as.list.len; i++)
            if (rt_form_mentions_set(e, subject->as.list.items[i], 0)) return false;
        return rt_prove_paths(e, pred, var_name,
                              subject->as.list.items[subject->as.list.len - 1],
                              base_kind, env, loc, depth + 1);
    }

    /* (match scrut pat1 [when g1] e1 pat2 [when g2] e2 ...) -- every arm, under
     * everything the arm's selection tells us.
     *
     * Arms are NOT fixed-width: `when` inserts a guard between a pattern and
     * its body, so the list is walked rather than strided in pairs.  Striding
     * misreads a guarded match wholesale -- `when` becomes a pattern and the
     * guard expression becomes a body -- which failed safe (the misread arm
     * never proved) but meant no guarded match was ever split.
     *
     * Four hypothesis sources, all true whenever the arm is the one that runs:
     *
     *   - the CONSTRUCTOR, as `(= (#dt/tag s) <tag>)`;
     *   - each record FIELD bound by the pattern, as `(= b (.field s))`,
     *     which is what lets a getter's postcondition mention the field it
     *     returns;
     *   - a LITERAL pattern, as `(= s <lit>)`;
     *   - the GUARD, verbatim -- a necessary condition for the arm to fire.
     *
     * The guard is only a NECESSARY condition, never sufficient: an arm also
     * requires every earlier arm to have failed, which is not asserted. That
     * direction is the safe one (fewer hypotheses only make a goal harder).
     *
     * Pattern binders that SHADOW a name in scope still decline the split. The
     * encoder has one flat namespace, so the arm's `r` would inherit an outer
     * `r`'s hypotheses -- the same class of bug as the shadowed `let`, and
     * unsound rather than imprecise.  Alpha-renaming would now be possible (a
     * binder finally HAS a fact to rename into), but the rename would have to
     * reach the synthesized selector facts as well, and declining costs only
     * precision. */
    if (rt_head_is(subject, "match") && subject->as.list.len >= 4) {
        const Form *scrut  = subject->as.list.items[1];
        const uint32_t len = subject->as.list.len;
        RefineHyp *saved_head  = env->head;
        uint32_t   saved_names = env->n_names;
        bool ok = true;
        uint32_t i = 2;
        while (ok && i < len) {
            const Form *pat = subject->as.list.items[i++];
            const Form *guard = NULL;
            if (i + 1 < len && rt_sym_is(subject->as.list.items[i], "when")) {
                guard = subject->as.list.items[i + 1];
                i += 2;
            }
            if (i >= len) { ok = false; break; }   /* malformed: no body */
            const Form *arm = subject->as.list.items[i++];

            /* PER-ARM SCOPE.  Each arm's facts must die with the arm: two arms
             * of the same match assert different tags for the same scrutinee,
             * so letting them accumulate would put `tag(s) = 0` and
             * `tag(s) = 1` in one environment -- a contradiction, which proves
             * every later arm's goal and elides its check.  The outer
             * save/restore below is not enough; this is the one that matters. */
            RefineHyp *arm_head  = env->head;
            uint32_t   arm_names = env->n_names;

            CtorDef *cd = rt_pat_ctor(e, pat);
            if (cd)
                refine_env_push(env,
                                rt_form_eq(e, pat->span,
                                           rt_form_call1(e, pat->span,
                                                         RT_DT_TAG_FN, scrut),
                                           form_int(e->arena, pat->span,
                                                    (int64_t)cd->tag)),
                                NULL, NULL);
            else if (pat && (pat->tag == F_INT || pat->tag == F_FLOAT))
                refine_env_push(env, rt_form_eq(e, pat->span, scrut, pat),
                                NULL, NULL);

            /* Binders: declare, decline on shadow, and tie each to its field. */
            if (pat && (pat->tag == F_LIST || pat->tag == F_VEC)) {
                for (uint32_t k = 1; ok && k < pat->as.list.len; k++) {
                    const Form *b = pat->as.list.items[k];
                    if (b->tag != F_SYM || !b->as.sym) continue;
                    for (uint32_t j = 0; j < env->n_names; j++)
                        if (env->names[j] &&
                            strcmp(env->names[j], b->as.sym->name) == 0)
                            { ok = false; break; }
                    if (!ok) break;
                    refine_env_declare(env, b->as.sym->name,
                                       rt_sort_of_kind(base_kind));
                    /* Only a record constructor has a field NAME to select
                     * with; a positional variant has no accessor to speak of,
                     * so its binder stays unconstrained as before. */
                    uint32_t fi = k - 1;
                    if (cd && cd->is_record && fi < cd->n_fields &&
                        cd->fields[fi].name) {
                        char acc[128];
                        snprintf(acc, sizeof(acc), ".%s", cd->fields[fi].name);
                        refine_env_push(env,
                                        rt_form_eq(e, b->span, b,
                                                   rt_form_call1(e, b->span,
                                                                 acc, scrut)),
                                        NULL, NULL);
                    }
                }
            }

            if (ok && guard) refine_env_push(env, guard, NULL, NULL);
            if (ok) ok = rt_prove_paths(e, pred, var_name, arm, base_kind, env,
                                        loc, depth + 1);
            env->head    = arm_head;
            env->n_names = arm_names;
        }
        env->head    = saved_head;
        env->n_names = saved_names;
        return ok;
    }

    /* A leaf: an ordinary expression the encoder can substitute for `r`. */
    return rt_prove_silent(e, pred, var_name, subject, base_kind, env, loc);
}

/* ------------------------------------------------------------------------- *
 * Constructor axioms.
 *
 * The match-arm theory gives a selector its meaning going ONE way: a binder is
 * the field it destructures.  It says nothing about a value that was just
 * BUILT, so `(let [b (Box p 3)] (match b (Box w h) w))` against `(= r p)` had
 * the chain `b = Box(p,3)`, `w = .width(b)`, goal `w = p` -- and no rule
 * connecting `.width` to the `Box` that produced it.
 *
 * The missing fact is the defining equation of a record constructor:
 *
 *     (= (.width (Box p 3)) p)
 *
 * It is universally true -- not path-dependent, not an assumption -- so it can
 * be asserted once per constructor application found anywhere in the obligation
 * rather than per path.  Congruence closure then does the rest: `b` and
 * `Box(p,3)` are equal, so `.width(b)` and `.width(Box(p,3))` are the same
 * term, which is `p`.
 *
 * Only RECORD constructors take part.  A positional variant has no field name,
 * so there is no accessor to write the equation about.
 * ------------------------------------------------------------------------- */

#define RT_CTOR_AXIOM_MAX_DEPTH 8

static void rt_push_ctor_axioms(Elab *e, RefineEnv *env, const Form *f,
                                uint32_t depth) {
    if (!f || depth >= RT_CTOR_AXIOM_MAX_DEPTH) return;
    if (f->tag != F_LIST && f->tag != F_VEC) return;

    if (f->tag == F_LIST && f->as.list.len >= 1) {
        CtorDef *cd = rt_pat_ctor(e, f);
        /* Arity must match exactly: a partial application is a closure, not a
         * value with fields, and over-applying is not this form at all. */
        if (cd && cd->is_record && cd->n_fields == f->as.list.len - 1) {
            for (uint32_t i = 0; i < cd->n_fields; i++) {
                if (!cd->fields[i].name) continue;
                char acc[128];
                snprintf(acc, sizeof(acc), ".%s", cd->fields[i].name);
                refine_env_push(env,
                                rt_form_eq(e, f->span,
                                           rt_form_call1(e, f->span, acc, f),
                                           f->as.list.items[i + 1]),
                                NULL, NULL);
            }
        }
    }

    for (uint32_t i = 0; i < f->as.list.len; i++)
        rt_push_ctor_axioms(e, env, f->as.list.items[i], depth + 1);
}

static bool rt_return_obligation_proven(Elab *e, const Form *pred,
                                        const char *var_name,
                                        const Form *subject, TypeKind base_kind,
                                        RefineEnv *env, const char *what,
                                        const char *fn_name, Span loc) {
    if (!pred) return false;
    /* Never elide a check that is itself observable.  Reported as not-proven
     * rather than diagnosed: an impure predicate keeps the runtime check it
     * would have had anyway, so discharge declines rather than inventing an
     * error. */
    if (rt_pred_is_impure(e, pred)) return false;

    /* Constructor axioms hold on every path, so they go in once, ahead of the
     * split, and are rewound afterwards so nothing leaks into the next
     * function's environment. */
    RefineHyp *ax_head  = env->head;
    uint32_t   ax_names = env->n_names;
    rt_push_ctor_axioms(e, env, subject, 0);

    /* RT4: a branching body is proved per path when it can be.  Tried first
     * and silently; failing costs one extra pass and changes nothing the user
     * sees, because the whole-body obligation below still runs and still
     * reports. */
    if (subject && (rt_head_is(subject, "if") || rt_head_is(subject, "let") ||
                    rt_head_is(subject, "match") || rt_head_is(subject, "do")) &&
        rt_prove_paths(e, pred, var_name, subject, base_kind, env, loc, 0)) {
        refine_note_split_proven();
        env->head = ax_head; env->n_names = ax_names;
        return true;
    }

    RefineObligation *ob =
        refine_collect_obligation(&e->refine_obs, pred, var_name, subject,
                                  rt_sort_of_kind(base_kind), type_name(type_simple(base_kind, CK_COPY)),
                                  loc, env, what, fn_name);
    bool proven = refine_discharge_one(ob, e->arena);
    env->head = ax_head; env->n_names = ax_names;
    return proven;
}

/* ------------------------------------------------------------------------- *
 * RT1 call-site crossings.
 *
 * Passing an argument where the parameter type is `#refine{ v : T | p }` is a
 * crossing INTO the refinement, so the caller owes a proof of `p[arg/v]`.  We
 * record the crossing during elaboration and resolve it afterwards; see the
 * comment on Elab.refine_call_sites for why the deferral is load-bearing.
 * ------------------------------------------------------------------------- */

/* Hash of the (callee, call_form) pointer pair. */
static uint32_t rt_cs_hash(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)a * 0x9e3779b97f4a7c15ull;
    uintptr_t y = (uintptr_t)b * 0xc2b2ae3d27d4eb4full;
    uint32_t h = (uint32_t)((x ^ (y >> 17)) >> 13);
    return h ? h : 1;
}

/* Probe the dedup set; returns true when this pair was already recorded.
 * Inserts `slot` otherwise. */
static bool rt_cs_seen(Elab *e, const Binding *callee, const Form *cf, uint32_t slot) {
    if (e->refine_cs_htab_cap == 0 ||
        (e->n_refine_call_sites + 1) * 2 >= e->refine_cs_htab_cap) {
        uint32_t ncap = e->refine_cs_htab_cap ? e->refine_cs_htab_cap * 2 : 128;
        uint32_t *nt = (uint32_t *)arena_alloc(e->arena, ncap * sizeof(uint32_t));
        memset(nt, 0, ncap * sizeof(uint32_t));
        for (uint32_t i = 0; i < e->n_refine_call_sites; i++) {
            uint32_t h = rt_cs_hash(e->refine_call_sites[i].callee,
                                    e->refine_call_sites[i].call_form);
            uint32_t j = h & (ncap - 1);
            while (nt[j]) j = (j + 1) & (ncap - 1);
            nt[j] = i + 1;
        }
        e->refine_cs_htab = nt;
        e->refine_cs_htab_cap = ncap;
    }
    uint32_t h = rt_cs_hash(callee, cf);
    uint32_t j = h & (e->refine_cs_htab_cap - 1);
    while (e->refine_cs_htab[j]) {
        const RefineCallSite *cs = &e->refine_call_sites[e->refine_cs_htab[j] - 1];
        if (cs->callee == callee && cs->call_form == cf) return true;
        j = (j + 1) & (e->refine_cs_htab_cap - 1);
    }
    e->refine_cs_htab[j] = slot + 1;
    return false;
}

uint32_t refine_note_call_site(Elab *e, const Binding *callee,
                               const Form *call_form, uint32_t arg_offset) {
    if (!e || !callee || !call_form)
        return e ? e->n_refine_call_sites : 0;
    /* A form can be elaborated more than once (macro expansion, specialization
     * retries).  Deduplicate so one source call site yields one diagnostic. */
    if (rt_cs_seen(e, callee, call_form, e->n_refine_call_sites))
        return e->n_refine_call_sites;
    if (e->n_refine_call_sites == e->cap_refine_call_sites) {
        uint32_t ncap = e->cap_refine_call_sites ? e->cap_refine_call_sites * 2 : 16;
        RefineCallSite *nb = (RefineCallSite *)arena_alloc(e->arena,
                                                           ncap * sizeof(RefineCallSite));
        if (e->n_refine_call_sites)
            memcpy(nb, e->refine_call_sites,
                   e->n_refine_call_sites * sizeof(RefineCallSite));
        e->refine_call_sites = nb;
        e->cap_refine_call_sites = ncap;
    }
    /* Prefer the name at the call site over the binding's, which for a
     * typeclass method is the mangled instance symbol.  A leading '.' is the
     * dispatch marker, not part of the method name. */
    const char *display = callee->name ? callee->name->name : "?";
    if (call_form->tag == F_LIST && call_form->as.list.len > 0) {
        const Form *head = call_form->as.list.items[0];
        if (head->tag == F_SYM && head->as.sym && head->as.sym->name) {
            const char *hn = head->as.sym->name;
            display = (hn[0] == '.' && hn[1]) ? hn + 1 : hn;
        }
    }

    RefineCallSite *cs = &e->refine_call_sites[e->n_refine_call_sites++];
    cs->callee         = callee;
    cs->callee_display = display;
    cs->call_form   = call_form;
    cs->arg_offset  = arg_offset;
    cs->env         = NULL;   /* back-filled by refine_fill_call_site_env */
    cs->caller_body = NULL;
    cs->caller_name = NULL;
    cs->class_param_preds = NULL;
    cs->class_param_vars  = NULL;
    cs->n_class_params    = 0;
    /* C2 / #reads: snapshot the borrows LIVE at this crossing.  The borrow
     * checker's scope is authoritative about liveness right here (a `frozen`
     * region's `(& w)` borrow is active); recovering it syntactically later
     * would risk treating an already-ended borrow as live.  A binding borrowed
     * here cannot be `^unique ^mut`-mutated (UT2) or `set!` (borrow-checked),
     * so a `#reads`-it measure is a function of frozen state in this obligation. */
    cs->frozen_names = NULL;
    cs->n_frozen     = 0;
    {
        uint32_t nb = 0;
        for (const Scope *s = e->scope; s; s = s->parent)
            for (const ScopeBorrow *bo = s->borrows; bo; bo = bo->next)
                if (bo->binding && bo->binding->name) nb++;
        if (nb) {
            const char **fn = (const char **)arena_alloc(e->arena, nb * sizeof(char *));
            uint32_t k = 0;
            for (const Scope *s = e->scope; s; s = s->parent)
                for (const ScopeBorrow *bo = s->borrows; bo; bo = bo->next) {
                    Binding *b = bo->binding;
                    if (!b || !b->name) continue;
                    /* Only publish a frozen name that STILL resolves to the
                     * borrowed binding here.  If it has been SHADOWED by an
                     * inner binding of the same spelling, the encoder's
                     * name-based match would otherwise treat the (unfrozen)
                     * shadow as frozen -- an unsoundness the shadow+despawn
                     * fixture pins.  Comparing to scope_lookup's innermost
                     * result closes it. */
                    if (scope_lookup(e->scope, b->name) != b) continue;
                    fn[k++] = b->name->name;
                }
            if (k) { cs->frozen_names = fn; cs->n_frozen = k; }
        }
    }
    cs->loc         = call_form->span;
    return e->n_refine_call_sites;
}

void refine_note_macro_expansion(Elab *e, const Form *call,
                                 const Form *expansion) {
    if (!e || !call || !expansion) return;
    for (uint32_t i = 0; i < e->n_refine_mexps; i++) {
        if (e->refine_mexp_calls[i] == call) {
            e->refine_mexp_bodies[i] = expansion;   /* latest re-elaboration */
            return;
        }
    }
    if (e->n_refine_mexps == e->cap_refine_mexps) {
        uint32_t ncap = e->cap_refine_mexps ? e->cap_refine_mexps * 2 : 16;
        const Form **nc = (const Form **)arena_alloc(e->arena,
                                                     ncap * sizeof(Form *));
        const Form **nb = (const Form **)arena_alloc(e->arena,
                                                     ncap * sizeof(Form *));
        if (e->n_refine_mexps) {
            memcpy(nc, e->refine_mexp_calls, e->n_refine_mexps * sizeof(Form *));
            memcpy(nb, e->refine_mexp_bodies, e->n_refine_mexps * sizeof(Form *));
        }
        e->refine_mexp_calls  = nc;
        e->refine_mexp_bodies = nb;
        e->cap_refine_mexps   = ncap;
    }
    e->refine_mexp_calls[e->n_refine_mexps]  = call;
    e->refine_mexp_bodies[e->n_refine_mexps] = expansion;
    e->n_refine_mexps++;
}

/* The expansion recorded for a macro-call form, or NULL.  Linear scan: the
 * table only holds macro calls seen in refined units, and the path walks that
 * consult it are already bounded (RT_CS_PATH_MAX_DEPTH). */
static const Form *rt_macro_expansion(const Elab *e, const Form *call) {
    if (!e || !call || call->tag != F_LIST) return NULL;
    for (uint32_t i = 0; i < e->n_refine_mexps; i++)
        if (e->refine_mexp_calls[i] == call) return e->refine_mexp_bodies[i];
    return NULL;
}

/* Keyed on (callee, call_form) rather than on the index refine_note_call_site
 * returns: that function yields the CURRENT count when it deduplicates, which
 * names the last crossing recorded rather than the matching one.  Attaching the
 * class predicates to the wrong crossing would lint an unrelated call. */
void refine_note_call_site_class_preds(Elab *e, const Binding *callee,
                                       const Form *call_form,
                                       const Form **preds, const char **vars,
                                       uint32_t n_params) {
    if (!e || !callee || !call_form) return;
    for (uint32_t i = e->n_refine_call_sites; i-- > 0; ) {
        RefineCallSite *cs = &e->refine_call_sites[i];
        if (cs->callee != callee || cs->call_form != call_form) continue;
        cs->class_param_preds = preds;
        cs->class_param_vars  = vars;
        cs->n_class_params    = n_params;
        return;
    }
}

void refine_fill_call_site_env(Elab *e, uint32_t from, RefineEnv *env,
                               const char *caller, const Form *body) {
    if (!e) return;
    for (uint32_t i = from; i < e->n_refine_call_sites; i++) {
        if (e->refine_call_sites[i].env) continue;   /* an inner defn already owns it */
        e->refine_call_sites[i].env         = env;
        e->refine_call_sites[i].caller_name = caller;
        e->refine_call_sites[i].caller_body = body;
    }
}

/* ------------------------------------------------------------------------- *
 * Path conditions for call-site crossings.
 *
 * A crossing is collected during elaboration and discharged after the whole
 * unit -- which is what lets it see every callee's refinement, and is why the
 * deferral is load-bearing.  The cost was that a call inside a branch was
 * checked WITHOUT the condition that selected the branch, so the canonical
 * decreasing-argument recursion could not discharge its own recursive call:
 *
 *     (defn f [n : Nat] : Nat
 *       (if (= n 0) 0 (+ 1 (f (- n 1)))))     ; needs n - 1 >= 0
 *
 * `n >= 0` is in the environment and `n != 0` is not, so `n - 1 >= 0` was
 * Unknown and the crossing kept its check.
 *
 * The conditions are recovered SYNTACTICALLY rather than by threading a
 * condition stack through expression elaboration.  `call_form` is a pointer
 * into the caller's body, so walking the body down to that exact node collects
 * every branch that had to be taken to reach it -- no change to any elab_*
 * function, and the facts are the same ones `rt_prove_paths` already
 * synthesizes for return obligations.
 * ------------------------------------------------------------------------- */

#define RT_CS_PATH_MAX_DEPTH 24
#define RT_CS_PATH_MAX_HYPS  8

/* Crossing identity that survives macro expansion.
 *
 * Pointer equality is the fast path.  But a region written as a MACRO -- the
 * shipped `ecs/freeze` `frozen`, or any macro that wraps a body with `~@body` --
 * expands by quasiquote, which RECONSTRUCTS the body into fresh Form objects.
 * So the crossing that gets elaborated and registered as `cs->call_form` is a
 * different pointer than the crossing node reachable in `cs->caller_body` (the
 * source defn body), and a pointer-only match reports 0 occurrences -- the guard
 * on the path is then dropped and a perfectly good `(if (alive? w e) ...)` no
 * longer discharges its read (see docs/reported/frozen-macro-breaks-refinement-
 * guard-discharge.md).
 *
 * The copy preserves the source SPAN, so we fall back to matching a real
 * source location plus the head symbol and arity.  This only ever RESCUES the
 * "copied once" case: callers keep the `!= 1` ambiguity decline, so a span
 * shared by more than one node (a macro that DUPLICATES a crossing) still bails,
 * and a crossing genuinely absent from the body still matches nothing.  A span
 * match is therefore never less conservative than pointer identity where it
 * mattered -- it strictly recovers copies of the same source crossing, which
 * carry the same guards on the same path. */
static bool rt_form_ident(const Form *a, const Form *b) {
    if (a == b) return true;
    if (!a || !b || a->tag != b->tag) return false;
    if (a->tag != F_LIST && a->tag != F_VEC) return false;   /* only call forms */
    if (span_is_unknown(a->span) || span_is_unknown(b->span)) return false;
    if (a->span.file_id  != b->span.file_id ||
        a->span.off_start != b->span.off_start ||
        a->span.off_end   != b->span.off_end) return false;
    if (a->as.list.len != b->as.list.len || a->as.list.len == 0) return false;
    const Form *ha = a->as.list.items[0], *hb = b->as.list.items[0];
    if (!ha || !hb || ha->tag != F_SYM || hb->tag != F_SYM) return false;
    return ha->as.sym == hb->as.sym;                          /* interned */
}

/* Count occurrences of `target` (pointer- or source-identical), stopping at 2.
 *
 * A macro call counts as its recorded EXPANSION, not its raw arguments: the
 * expansion is the code that elaborated (so a template-GENERATED crossing is
 * only reachable there), and a `~body`-spliced argument appears in BOTH the
 * call form and the expansion -- walking both would double-count one crossing
 * and trip the `!= 1` ambiguity decline. */
static uint32_t rt_form_occurrences(const Elab *e, const Form *node,
                                    const Form *target, uint32_t depth) {
    if (!node || depth >= RT_CS_PATH_MAX_DEPTH) return 0;
    if (rt_form_ident(node, target)) return 1;
    if (node->tag != F_LIST && node->tag != F_VEC) return 0;
    {
        const Form *mx = rt_macro_expansion(e, node);
        if (mx) return rt_form_occurrences(e, mx, target, depth);  /* lateral hop */
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < node->as.list.len && n < 2; i++)
        n += rt_form_occurrences(e, node->as.list.items[i], target, depth + 1);
    return n;
}

/* Walk down to `target`, appending the condition of every `if` branch entered
 * on the way.  Returns true when the target is in this subtree.
 *
 * A target inside the CONDITION of an `if` gets no fact from that `if`: the
 * condition is evaluated before either branch is chosen, so neither it nor its
 * negation holds there. */
/* True when `name` is already declared in the crossing's environment -- i.e.
 * a binder of that name SHADOWS something the hypotheses talk about. */
static bool rt_env_has_name(RefineEnv *env, const Symbol *sym) {
    if (!env || !sym) return false;
    for (uint32_t j = 0; j < env->n_names; j++)
        if (env->names[j] && strcmp(env->names[j], sym->name) == 0) return true;
    return false;
}

/* `*shadowed` is set when a binder on the path rebinds a name the environment
 * already has hypotheses about.  The caller must then abandon the crossing
 * entirely, not merely drop a fact: the encoder has ONE FLAT NAMESPACE, so the
 * argument form's `x` is encoded as the same variable as the outer `x` and
 * silently inherits its hypotheses.  `(let [x (- x x)] (sdiv 10 x))` under
 * `x > 0` "proved" `x != 0` of a value that is zero.
 *
 * That predates path conditions -- the environment always carried the
 * parameter refinements -- and it was never a miscompile, because a crossing
 * is a diagnostic layer and never elides the callee's own entry check, which
 * still fires. What it was is a WRONG ANSWER: --strict-refine accepted a
 * program that panics. */
static bool rt_collect_path_conds(Elab *e, RefineEnv *env, const Form *node,
                                  const Form *target, const Form **hyps,
                                  uint32_t *n, bool *shadowed, uint32_t depth) {
    if (!node || depth >= RT_CS_PATH_MAX_DEPTH) return false;
    if (rt_form_ident(node, target)) return true;
    if (node->tag != F_LIST && node->tag != F_VEC) return false;

    /* A macro call walks as its recorded EXPANSION (mirrors
     * rt_form_occurrences): a guard or crossing the template GENERATED --
     * e.g. `(if (alive? ~w ~e) (get! ~w ~e) -1)` -- exists only there, and
     * an `if` in the expansion contributes its condition exactly like a
     * written one. */
    {
        const Form *mx = rt_macro_expansion(e, node);
        if (mx) return rt_collect_path_conds(e, env, mx, target, hyps, n,
                                             shadowed, depth);   /* lateral hop */
    }

    if (rt_head_is(node, "if") && node->as.list.len == 4) {
        const Form *c = node->as.list.items[1];
        if (rt_collect_path_conds(e, env, c, target, hyps, n, shadowed, depth + 1))
            return true;
        for (uint32_t br = 2; br <= 3; br++) {
            if (!rt_collect_path_conds(e, env, node->as.list.items[br], target,
                                       hyps, n, shadowed, depth + 1))
                continue;
            if (*n >= RT_CS_PATH_MAX_HYPS) return true;  /* deep enough */
            if (br == 2) {
                hyps[(*n)++] = c;
            } else {
                Form **notk = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
                notk[0] = form_sym(e->arena, node->span,
                                   symtab_intern(e->st, strslice("not", 3)));
                notk[1] = (Form *)c;
                hyps[(*n)++] = form_list(e->arena, node->span, notk, 2);
            }
            return true;
        }
        return false;
    }

    /* (let [x v] body) -- `x = v` is a fact for a call in the BODY, and no
     * fact at all for a call inside `v` itself, which runs first.
     *
     * A binding that SHADOWS a name in scope contributes nothing rather than
     * declining the whole path: the equation would be `x = <something
     * mentioning x>` in one flat namespace -- false for every x, and a false
     * hypothesis proves the goal.  The return-obligation splitter alpha-renames
     * instead, which it can because it also rewrites the body; here there is no
     * body to rewrite, only a call form to leave alone, so dropping the one
     * fact is the honest answer.  Every other condition on the path stays. */
    if (rt_head_is(node, "let") && node->as.list.len == 3) {
        const Form *binds = node->as.list.items[1];
        const Form *body  = node->as.list.items[2];
        if (binds && binds->tag == F_VEC && binds->as.list.len == 2 &&
            binds->as.list.items[0]->tag == F_SYM &&
            binds->as.list.items[0]->as.sym) {
            const Form *x = binds->as.list.items[0];
            const Form *v = binds->as.list.items[1];
            if (rt_collect_path_conds(e, env, v, target, hyps, n, shadowed, depth + 1))
                return true;
            if (!rt_collect_path_conds(e, env, body, target, hyps, n, shadowed, depth + 1))
                return false;
            if (rt_env_has_name(env, x->as.sym)) { *shadowed = true; return true; }
            if (*n >= RT_CS_PATH_MAX_HYPS) return true;
            /* A bound FUNCTION is not an arithmetic fact, and asserting it
             * actively costs: the encoder abstracts the lambda to an
             * uninterpreted symbol, and `refine_model_search` declines any VC
             * containing one -- so a goal that was REFUTED with a
             * counterexample degrades to Unknown. `(let [f (fn ...)] (f 0))`
             * against `(> v 0)` stopped being a compile error the moment this
             * equation was added.
             *
             * The general shape of that hazard is worth remembering: adding a
             * hypothesis can never make a goal easier to PROVE incorrectly,
             * but it can make it harder to REFUTE. Hypotheses are not free. */
            if (!rt_head_is(v, "fn")) hyps[(*n)++] = rt_form_eq(e, node->span, x, v);
            return true;
        }
        /* A wider binding list: descend without claiming any equation. */
    }

    /* (match scrut pat [when g] body ...) -- an arm contributes what selected
     * it.  Only the two sources that introduce no NAMES are collected here: a
     * literal pattern's `(= scrut <lit>)` and a guard, verbatim.  A
     * constructor's tag and its field selectors are deliberately left out --
     * they come with pattern BINDERS, and a binder that shadows an outer name
     * would silently inherit its hypotheses in this flat namespace, which is
     * the unsound direction rather than the imprecise one. */
    if (rt_head_is(node, "match") && node->as.list.len >= 4) {
        const Form *scrut = node->as.list.items[1];
        const uint32_t len = node->as.list.len;
        if (rt_collect_path_conds(e, env, scrut, target, hyps, n, shadowed, depth + 1))
            return true;
        uint32_t i = 2;
        while (i < len) {
            const Form *pat = node->as.list.items[i++];
            const Form *guard = NULL;
            if (i + 1 < len && rt_sym_is(node->as.list.items[i], "when")) {
                guard = node->as.list.items[i + 1];
                i += 2;
            }
            if (i >= len) return false;            /* malformed: no body */
            const Form *arm = node->as.list.items[i++];
            /* A call inside the guard runs before the arm is chosen. */
            if (guard &&
                rt_collect_path_conds(e, env, guard, target, hyps, n, shadowed, depth + 1))
                return true;
            if (!rt_collect_path_conds(e, env, arm, target, hyps, n, shadowed, depth + 1))
                continue;
            if (pat && (pat->tag == F_LIST || pat->tag == F_VEC))
                for (uint32_t k = 1; k < pat->as.list.len; k++) {
                    const Form *b = pat->as.list.items[k];
                    if (b->tag == F_SYM && rt_env_has_name(env, b->as.sym)) {
                        *shadowed = true;
                        return true;
                    }
                }
            if (guard && *n < RT_CS_PATH_MAX_HYPS) hyps[(*n)++] = guard;
            if (pat && (pat->tag == F_INT || pat->tag == F_FLOAT) &&
                *n < RT_CS_PATH_MAX_HYPS)
                hyps[(*n)++] = rt_form_eq(e, pat->span, scrut, pat);
            return true;
        }
        return false;
    }

    for (uint32_t i = 0; i < node->as.list.len; i++)
        if (rt_collect_path_conds(e, env, node->as.list.items[i], target, hyps,
                                  n, shadowed, depth + 1))
            return true;
    return false;
}

/* The caller's WHOLE body, as one form the path walk can descend.
 *
 * `caller_body` used to be the defn's LAST body form -- correct for its other
 * job (the return obligation's subject) and wrong here.  A crossing in any
 * earlier form was then invisible to `rt_collect_path_conds`, which walks down
 * to `call_form` and gives up when it does not find it, so the call was
 * checked without the branch that guards it.  A zero-parameter caller showed
 * it plainly: `main`'s last form is the literal `0`, and the walk searched `0`
 * for the call.  See
 * docs/archive/refine-callsite-path-conds-lost-multi-form-body.md.
 *
 * A single-form body is passed through unwrapped, so the common case allocates
 * nothing and the resulting tree is byte-identical to before.  A multi-form
 * body is wrapped in a synthetic `(do ...)`: `do` is not one of the three
 * heads the walk treats specially, so it falls to the generic descent, which
 * is exactly the "a later body form is not guarded by an earlier one"
 * semantics wanted here.
 *
 * Widening also widens the two vetoes in rt_push_cs_path_conds, in the safe
 * direction both times: a `set!` anywhere in the body now declines every
 * crossing in it (it used to be checked only against the last form, so an
 * assignment in an earlier form could invalidate a condition unnoticed), and
 * a `call_form` node reachable from two body forms now counts as ambiguous
 * rather than unique. */
static const Form *rt_whole_body(Elab *e, const Form *call, uint32_t body_start) {
    if (!call || call->as.list.len <= body_start) return NULL;
    uint32_t nb = call->as.list.len - body_start;
    if (nb == 1) return call->as.list.items[body_start];
    Form **items = (Form **)arena_alloc(e->arena, (nb + 1) * sizeof(Form *));
    items[0] = form_sym(e->arena, call->span,
                        symtab_intern(e->st, strslice("do", 2)));
    for (uint32_t i = 0; i < nb; i++)
        items[i + 1] = call->as.list.items[body_start + i];
    return form_list(e->arena, call->span, items, nb + 1);
}

/* Push this crossing's path conditions onto `cs->env`, returning the saved
 * head so the caller can rewind.  Declines -- pushing nothing -- when the body
 * assigns anywhere, since a condition mentioning a reassigned name may no
 * longer hold at the call, and when `call_form` is reachable by more than one
 * route, which a macro that shares a node can produce and which would make
 * "the" path ambiguous. */
static RefineHyp *rt_push_cs_path_conds(Elab *e, RefineCallSite *cs,
                                        bool *skip) {
    RefineHyp *saved = cs->env ? cs->env->head : NULL;
    *skip = false;
    if (!cs->env || !cs->caller_body || !cs->call_form) return saved;
    if (rt_form_mentions_set(e, cs->caller_body, 0)) return saved;
    if (rt_form_occurrences(e, cs->caller_body, cs->call_form, 0) != 1) return saved;

    const Form *hyps[RT_CS_PATH_MAX_HYPS];
    uint32_t n = 0;
    bool shadowed = false;
    if (!rt_collect_path_conds(e, cs->env, cs->caller_body, cs->call_form,
                               hyps, &n, &shadowed, 0))
        return saved;
    if (shadowed) { *skip = true; return saved; }
    for (uint32_t i = 0; i < n; i++)
        refine_env_push(cs->env, hyps[i], NULL, NULL);
    return saved;
}

/* TUR-W0377: the resolved instance accepts this argument, but the CLASS
 * signature rejects it outright, so the call is relying on that instance
 * demanding less than its class.
 *
 * Only a DEFINITE violation warns.  `refine_model_search` returning a model
 * with no free variables means the goal folded to false on the values written
 * at the site; an OPEN model only says "not for every input", which for an
 * argument the instance genuinely accepts is not worth a diagnostic.  That
 * keeps the lint silent on `(.scale-by 3 n)` and loud on `(.scale-by 3 0)`.
 *
 * `speculative` keeps the obligation off the ordinary reporting path: its
 * failure is an interface-versus-implementation disagreement with its own
 * wording, not a TUR-E0371 about a value the program cannot supply. */
static void rt_lint_class_leniency(Elab *e, RefineCallSite *cs, uint32_t p,
                                   const Form *cls_pred, const Form *arg,
                                   TypeKind pk, const RefineSubst *subst,
                                   uint32_t n_subst) {
    if (!cls_pred || !cs) return;
    const Binding *callee = cs->callee;
    /* An instance that restated its class predicate has nothing to disagree
     * with -- the two are the same Form. */
    if (callee->refine_param_preds && p < callee->n_refine_params &&
        callee->refine_param_preds[p] == cls_pred) return;

    const char *cvar = (cs->class_param_vars && p < cs->n_class_params)
                     ? cs->class_param_vars[p] : NULL;
    RefineObligation *ob = refine_collect_obligation(
        &e->refine_obs, cls_pred, cvar, arg,
        rt_sort_of_kind(pk), type_name(type_simple(pk, CK_COPY)),
        cs->loc, cs->env, "class signature", cs->caller_name);
    if (!ob) return;
    refine_obligation_set_subst(ob, e->arena, subst, n_subst);
    ob->speculative = true;
    if (refine_discharge_one(ob, e->arena)) return;   /* class admits it too */
    if (!ob->vc) return;
    RefineModel *m = refine_model_search(ob->vc, e->arena);
    if (!m || m->n != 0) return;                      /* not a definite violation */

    const char *cname = cs->callee_display
                      ? cs->callee_display
                      : (callee->name ? callee->name->name : "?");
    diag_emit_with_code(DIAG_WARNING, cs->loc, TUR_W0377_REFINE_INSTANCE_LENIENCY,
                        "argument %u of '%s' is accepted by the resolved "
                        "instance but violates the class signature's refinement",
                        p + 1, cname);
    diag_emit(DIAG_NOTE, cs->loc,
              "the instance demands less than its class, which is legal; this "
              "call would fail if dispatch were dynamic or a stricter instance "
              "existed");
}

void refine_resolve_call_sites(Elab *e) {
    if (!e) return;
    for (uint32_t i = 0; i < e->n_refine_call_sites; i++) {
        RefineCallSite *cs = &e->refine_call_sites[i];
        const Binding *callee = cs->callee;
        /* A crossing is worth walking when EITHER side has a predicate.  An
         * instance that explicitly demands nothing publishes no arrays of its
         * own, and that is exactly the shape TUR-W0377 exists to lint. */
        bool has_inst_preds = callee->refine_param_preds &&
                              callee->n_refine_params > 0;
        bool has_class_preds = cs->class_param_preds && cs->n_class_params > 0;
        if (!has_inst_preds && !has_class_preds) continue;

        const Form *cf = cs->call_form;
        if (!cf || cf->tag != F_LIST) continue;
        uint32_t n_args = cf->as.list.len > cs->arg_offset
                        ? cf->as.list.len - cs->arg_offset : 0;

        /* The branches that had to be taken to reach this call are facts here.
         * Rewound below so one crossing's path never leaks into the next --
         * several crossings share one caller's env. */
        bool cs_skip = false;
        RefineHyp *cs_saved = rt_push_cs_path_conds(e, cs, &cs_skip);
        if (cs_skip) continue;   /* shadowed binder: no honest answer here */

        /* Every callee parameter name maps to the caller's argument in that
         * slot, so a predicate mentioning a SIBLING parameter is checked
         * against the real argument rather than a free variable. */
        RefineSubst subst[MAX_FN_ARITY];
        uint32_t n_subst = 0;
        for (uint32_t p = 0; has_inst_preds && p < callee->n_refine_params &&
                             p < n_args && n_subst < MAX_FN_ARITY; p++) {
            if (!callee->refine_param_names[p]) continue;
            subst[n_subst].name = callee->refine_param_names[p];
            subst[n_subst].form = cf->as.list.items[cs->arg_offset + p];
            n_subst++;
        }

        uint32_t n_slots = has_inst_preds ? callee->n_refine_params : 0;
        if (has_class_preds && cs->n_class_params > n_slots)
            n_slots = cs->n_class_params;
        for (uint32_t p = 0; p < n_slots && p < n_args; p++) {
            const Form *pred = (has_inst_preds && p < callee->n_refine_params)
                             ? callee->refine_param_preds[p] : NULL;
            const Form *cls_pred = (has_class_preds && p < cs->n_class_params)
                                 ? cs->class_param_preds[p] : NULL;
            if (!pred && !cls_pred) continue;
            const Form *arg = cf->as.list.items[cs->arg_offset + p];
            TypeKind pk = (callee->type.kind == TY_FN &&
                           callee->type.as.fn.arg_kinds &&
                           p < callee->type.as.fn.arity)
                        ? callee->type.as.fn.arg_kinds[p] : TY_INT;
            if (!pred) {
                /* The instance demands nothing here.  Nothing to prove -- but
                 * the class may still reject this argument. */
                rt_lint_class_leniency(e, cs, p, cls_pred, arg, pk, subst, n_subst);
                continue;
            }

            const char *cname = cs->callee_display ? cs->callee_display
                              : (callee->name ? callee->name->name : "?");
            char what[160];
            if (cs->caller_name)
                snprintf(what, sizeof(what), "argument %u of '%s' in '%s'",
                         p + 1, cname, cs->caller_name);
            else
                snprintf(what, sizeof(what), "argument %u of '%s'", p + 1, cname);
            const char *what_owned = arena_strdup(e->arena, what, strlen(what));

            RefineObligation *ob = refine_collect_obligation(
                &e->refine_obs, pred, callee->refine_param_vars[p], arg,
                rt_sort_of_kind(pk), type_name(type_simple(pk, CK_COPY)),
                cs->loc, cs->env, what_owned, cs->caller_name);
            if (!ob) continue;
            refine_obligation_set_subst(ob, e->arena, subst, n_subst);
            /* C2 / #reads: carry this crossing's frozen (borrowed) bindings so
             * the encoder can grant a `#reads w` measure congruence when w is
             * one of them. */
            refine_obligation_set_frozen(ob, cs->frozen_names, cs->n_frozen);
            /* The callee checks its own parameters on entry, so an argument
             * we cannot prove is the ordinary case, not news.  What still
             * errors is an argument that is DEFINITELY wrong -- a closed goal
             * that evaluates false -- which is `(safe-div 10 0)` becoming a
             * compile-time failure instead of a runtime panic.
             *
             * C2 / #reads exception: a `#reads`-measure predicate is impure, so
             * the callee's entry check is TUR-E0375-unemittable and is
             * suppressed (see rt_inject_param_checks) -- there is NO runtime
             * backstop for this crossing.  Mark it proof-only so an unproven
             * one is reported (a warning in non-strict, an error in strict)
             * rather than silently trusted, and so its W0372 does not claim a
             * runtime check was kept. */
            bool reads_crossing = rt_pred_reads_measure(e, pred);
            ob->runtime_guarded = !reads_crossing;
            ob->reads_no_runtime = reads_crossing;
            bool inst_ok = refine_discharge_one(ob, e->arena);

            /* Reading B + lint: the obligation above is the resolved INSTANCE's,
             * which is the more precise of the two contracts and the one this
             * call actually has to satisfy.  But when the instance demands less
             * than its class and the argument is one the CLASS rejects
             * outright, the call is leaning on that instance's private
             * leniency -- which is not part of the interface, and is gone the
             * moment dispatch goes dynamic or a stricter instance is added.
             *
             * Only when the instance obligation itself was PROVED: an argument
             * that failed the instance check already has its own E0371 at this
             * span, and a second diagnostic about the class would just be
             * noise. */
            if (inst_ok)
                rt_lint_class_leniency(e, cs, p, cls_pred, arg, pk, subst, n_subst);
        }
        if (cs->env) cs->env->head = cs_saved;
    }
}

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

static Type *fn_type_from_form_impl(Elab *e, const Form *form,
                                    const Symbol **type_params,
                                    Kind *type_param_kinds,
                                    uint8_t n_type_params);

/* Public entry for value-type annotations (defn/fn params + returns, defstruct
 * fields, let-bindings). A bare type-level row (`#row{...}`, kind [*]) is
 * rejected here: a value cannot have row type. A row nested inside a
 * constructor application -- `(Query #row{...})` -- is resolved by
 * type_expr_from_form and surfaces as the application's result kind `*`, so it
 * passes. Internal self-calls below go through this wrapper too, so a row is
 * rejected in every value-type sub-position (e.g. an arrow argument). The
 * innermost offender emits once and returns NULL, which propagates without a
 * second diagnostic. See docs/reported/row-type-in-value-position-loses-elements.md. */
Type *fn_type_from_form(Elab *e, const Form *form,
                        const Symbol **type_params,
                        Kind *type_param_kinds,
                        uint8_t n_type_params) {
    Type *t = fn_type_from_form_impl(e, form, type_params, type_param_kinds,
                                     n_type_params);
    if (t && t->hkt_kind == KIND_TYPEROW) {
        diag_emit_with_code(DIAG_ERROR, form ? form->span : (Span){0},
            TUR_E0012_KIND_MISMATCH,
            "kind mismatch (TUR-E0012): `#row{...}` is a type-level row "
            "(kind [*]); a value cannot have row type. A row may only appear "
            "as a type argument to a row-kinded ('^&') constructor parameter");
        return NULL;
    }
    return t;
}

static Type *fn_type_from_form_impl(Elab *e, const Form *form,
                                    const Symbol **type_params,
                                    Kind *type_param_kinds,
                                    uint8_t n_type_params) {
    if (!form) return NULL;
    if (form->tag == F_TYPE_ANN && form->as.list.len > 0) {
        return fn_type_from_form_impl(e, form->as.list.items[0],
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
        /* CC4: a bare flavored continuation annotation -- :cont (cloneable),
         * :escape-cont (call/cc / escape), or :serial-cont.  type_expr_from_form
         * only knows the bare name "cont" (cloneable); the escape/serial flavors
         * must be preserved here so (k v) sugar dispatches to the right resume
         * runtime (tur_escape_resume for an undelimited call/cc landing). */
        {
            int cflav = cont_flavor_from_name(sym->name);
            if (cflav >= 0) {
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_cont_flavored(TY_INT, (ContFlavor)cflav);
                if (cont_name_is_multishot(sym->name)) t->copy_kind = CK_MULTISHOT;
                return t;
            }
        }
        /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
         * annotation reached through the spaced `: rc<T>` (F_TYPE_ANN) path or a
         * nested type position.  Resolve it here too, so it does not fall through
         * to type_expr_from_form and become a fresh tyvar named "rc<int>".  The
         * fused `:rc<T>` keyword path in the defn/fn ladders handles the keyword
         * form directly. */
        if (sym) {
            Type *rt = rc_family_type_from_keyword_name(e, sym->name, sym->len,
                                                        form->span, NULL,
                                                        type_params, type_param_kinds,
                                                        n_type_params);
            if (rt) return rt;
        }
        Type *t = type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        return t;
    }
    if (form->tag == F_LIST && form->as.list.len >= 1 &&
        form->as.list.items[0]->tag == F_SYM) {
        const Symbol *head = form->as.list.items[0]->as.sym;
        /* Variadic HKT rows (Layer 5): (row-concat/row-union/row-intersect ...)
         * are type-level row operations handled by type_expr_from_form, not
         * generic type applications. */
        if (head->name && (strcmp(head->name, "row-concat") == 0 ||
                           strcmp(head->name, "row-union") == 0 ||
                           strcmp(head->name, "row-intersect") == 0 ||
                           /* L6 follow-up D: row-canon, the permutation
                            * canonicaliser, is a row-level form like the
                            * other algebra ops -- route it through
                            * type_expr_from_form so signature annotations
                            * see (row-canon #row{...}) as a row, not as a
                            * generic type application. */
                           strcmp(head->name, "row-canon") == 0)) {
            return type_expr_from_form(e, form, NULL, type_params, type_param_kinds, n_type_params);
        }
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
            head == e->sym_arrow || head == e->sym_fn || head == e->sym_c_fn ||
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
        /* CC4 / value-typed cont: (cont R) / (escape-cont R) / (serial-cont R)
         * -- a flavored continuation whose result type is R. `cont` is not a
         * generic arrow-kind constructor, so handle the application here.
         *
         * slice 4: the two-arg form (cont BodyT ResetT) additionally pins the
         * resume-value type BodyT, so (k v) checks `v : BodyT`. The one-arg form
         * leaves BodyT unknown (unchecked), preserving existing signatures. */
        {
            int cflav = cont_flavor_from_name(head->name);
            bool cmulti = head->name && cont_name_is_multishot(head->name);
            if (cflav >= 0 && form->as.list.len == 2) {
                Type *ret = fn_type_from_form_impl(e, form->as.list.items[1],
                                              type_params, type_param_kinds, n_type_params);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_cont_flavored(ret ? ret->kind : TY_INT, (ContFlavor)cflav);
                if (cmulti) t->copy_kind = CK_MULTISHOT;
                return t;
            }
            if (cflav >= 0 && form->as.list.len == 3) {
                Type *bt = fn_type_from_form_impl(e, form->as.list.items[1],
                                              type_params, type_param_kinds, n_type_params);
                Type *rt = fn_type_from_form_impl(e, form->as.list.items[2],
                                              type_params, type_param_kinds, n_type_params);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = type_cont_arg_flavored(bt ? bt->kind : TY_UNKNOWN,
                                            rt ? rt->kind : TY_INT, (ContFlavor)cflav);
                if (cmulti) t->copy_kind = CK_MULTISHOT;
                return t;
            }
        }
        /* assoc-types-plan: type-level associated-type projection in annotation
         * position, e.g. `x : (Elem (Vec int))`.  fn_type_from_form builds type
         * applications itself (it never reaches type_expr_from_form's app path),
         * so resolve the projection here too.  Fires only when `head` is a
         * declared associated type member, leaving every ordinary application
         * unchanged. */
        if (form->as.list.len >= 2) {
            uint8_t assoc_idx;
            if (typeclass_env_find_assoc_type(&e->typeclass_env, head, &assoc_idx)) {
                /* assoc-types-2 (MP4): collect N projection arguments for a
                 * multi-parameter class; `(Elem (Vec int))` is the n == 1 path. */
                uint8_t n_args = (uint8_t)(form->as.list.len - 1);
                Type arg_buf[MAX_FN_ARITY];
                if (n_args > MAX_FN_ARITY) {
                    diag_emit(DIAG_ERROR, form->span,
                              "associated type '%s' projected over too many arguments",
                              head->name);
                    return NULL;
                }
                for (uint32_t i = 0; i < n_args; i++) {
                    Type *a = fn_type_from_form(e, form->as.list.items[i + 1],
                                                type_params, type_param_kinds, n_type_params);
                    if (!a) return NULL;
                    arg_buf[i] = *a;
                }
                const Type *bound = typeclass_env_resolve_assoc_type_n(
                    &e->typeclass_env, head, arg_buf, n_args);
                if (!bound) {
                    diag_emit(DIAG_ERROR, form->span,
                              "no instance binding for associated type '%s' at this type",
                              head->name);
                    return NULL;
                }
                Type *out = (Type *)arena_alloc(e->arena, sizeof(Type));
                *out = *bound;
                return out;
            }
        }
        Type *cur = fn_type_from_form_impl(e, form->as.list.items[0],
                                      type_params, type_param_kinds, n_type_params);
        if (!cur) return NULL;
        /* Original constructor head -- carries the positional parameter kinds
         * used for row-kind validation below (cur becomes a TY_APP after the
         * first argument, losing the StructDef). */
        Type head_type = *cur;
        for (uint32_t i = 1; i < form->as.list.len; i++) {
            Form *arg_form = form->as.list.items[i];
            Type *arg = NULL;
            /* SZ8 non-GADT: accept Size GADT literals (Static N) / (Add s s) /
             * (Mul s s) as a type-app argument; lower to a TY_INT placeholder.
             * The real size info lives in the retained Form (param/return
             * annotation), where size_term_from_form recovers it for SZ8
             * cross-parameter unification.  Mirrors the same lifting added to
             * type_expr_from_form's type-app loop. */
            if (arg_form->tag == F_LIST &&
                    arg_form->as.list.len >= 1 &&
                    arg_form->as.list.items[0]->tag == F_SYM) {
                const char *op = arg_form->as.list.items[0]->as.sym->name;
                bool is_size_op = (strcmp(op, "Static") == 0 ||
                                   strcmp(op, "Add")    == 0 ||
                                   strcmp(op, "Mul")    == 0);
                if (is_size_op &&
                        size_term_from_form(e->arena, arg_form, NULL, NULL) != NULL) {
                    Type *ph = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *ph = type_from_kind(TY_INT);
                    arg = ph;
                }
            }
            if (!arg) {
                arg = fn_type_from_form_impl(e, arg_form,
                                          type_params, type_param_kinds, n_type_params);
            }
            if (!arg) return NULL;
            /* Same peel type_expr_from_form's app loop does -- this loop is the
             * one a `defn`/`fn` parameter annotation actually goes through, so
             * fixing only the other one leaves `(Box #refine{...})` broken in
             * the position people write it. */
            arg = rt_peel_type_arg_contract(arg, arg_form->span);
            if (!check_row_type_arg_kind(head_type, (uint8_t)(i - 1), *arg,
                                         form->as.list.items[i]->span))
                return NULL;
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
        /* Preserve the rest tyvar's *name* so the call site can substitute it
         * in the declared result type (e.g. recover A=int from `(list-of 7)`
         * into the `(List A)` return).  The call-site reader special-cases a
         * TY_TYVAR rest_full_type to keep the homogeneity acceptance path
         * (any single type) rather than treating it as a type-identity match. */
        Type *tv = (Type *)arena_alloc(e->arena, sizeof(Type));
        *tv = type_tyvar_named(sym ? sym->name : NULL);
        *out_full = tv;
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
        case TY_FN:
            /* poly-hof-constrained-arg-baked-carrier: a function-typed parameter
             * whose signature carries a named tyvar (e.g. `f : (fn [A] int)` on
             * a HOF quantified over A) must stay on the regular generic TY_FN
             * path so it monomorphizes per instantiation -- demoting it to the
             * tur_poly_fn_t carrier bakes the int64-carrier ABI and miscompiles
             * a by-value struct argument.  Without this TY_FN recursion the
             * nested tyvar went undetected and `carrier_ok` wrongly fired. */
            if (fn_type_has_named_tyvar(t->as.fn.result_full_type)) return true;
            if (t->as.fn.arg_full_types) {
                for (uint32_t i = 0; i < t->as.fn.arity; i++) {
                    if (fn_type_has_named_tyvar(t->as.fn.arg_full_types[i])) return true;
                }
            }
            return false;
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
                        strcmp(e->sig_tyvars[i], t->as.tyvar_.name) == 0) {
                        /* van-laarhoven-lens-composition: a later occurrence with
                         * a constructor kind (e.g. `f` heading `(f int)`) upgrades
                         * a kind-* record so a nested lambda recovers `* -> *`. */
                        if (t->hkt_kind != KIND_STAR &&
                            e->sig_tyvar_kinds[i] == KIND_STAR)
                            e->sig_tyvar_kinds[i] = t->hkt_kind;
                        return;
                    }
                }
                if (e->n_sig_tyvars < 32) {
                    e->sig_tyvar_kinds[e->n_sig_tyvars] = t->hkt_kind;
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
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
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
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
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
    /* structdef-retirement DS-C: the parallel scan over the (always-empty)
     * struct_defs registry is dead -- every parametric type is an ADT now. */
    return false;
}

static bool form_mentions_type_param(const Form *form, const Symbol *sym) {
    if (!form || !sym) return false;
    switch (form->tag) {
        case F_SYM:
        case F_KEYWORD:
            return form->as.sym == sym;
        case F_CONTRACT_TYPE:
            /* items: [var, base-type, "|", predicate].  Only the BASE TYPE is
             * a type expression.  The bound variable and the predicate are
             * value-level: their free names are ordinary values, and treating
             * them as type-variable mentions makes the implicit-type-param
             * inference swallow the very parameters the predicate constrains
             * (`[v : int, n : int]` with a `(len v)` refinement lost both
             * parameters and then failed to parse their annotations). */
            return form->as.list.len > 1 &&
                   form_mentions_type_param(form->as.list.items[1], sym);
        case F_TYPE_ANN:
        case F_LIST:
        case F_VEC:
        case F_MAP:
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

/* Phase RT: does `t` (or any nested type) reference the named type variable?
 * Used to classify a `where (Class a)` constraint as argument-resolved (the
 * tyvar appears in some parameter type) vs return-resolved (it does not). */
static bool type_mentions_named_tyvar(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return type_mentions_named_tyvar(t->as.app.fn, name) ||
                   type_mentions_named_tyvar(t->as.app.arg, name);
        default:
            return false;
    }
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

/* bare-fat-param-non-int-result inference
 * (docs/upcoming/bare-fat-result-type-inference-plan.md, Phase A):
 * A bare `^fat g` parameter (TY_PTR_VOID + is_fat, no fn-type annotation) has
 * no recorded result type, so a direct call (g x) is elaborated with an
 * int64_t result (elab_call.c, the bare-^fat dispatch path).  When such a call
 * sits in a function's result position and the declared return type is a
 * float-register-class type, emitting `return <int64 expr>;` into a
 * double-returning C function would read the wrong register (rax vs xmm0).
 * (cstr/ptr returns share the integer register and round-trip, so only the
 * float-class case is wrong.)
 *
 * Codegen already lowers the bare-^fat dispatch using the call expression's
 * `e->type` (emit_expr.c, ~1799), so it is correct for any result type -- the
 * only thing wrong is that elaboration stamped the call TYPE_INT.  This pass
 * re-stamps the call's result type from the declared (return / binding) type
 * once it is known.  Soundness rests on the tail-only walk below: only result
 * positions are visited, so a non-tail bare-^fat call (e.g. an argument to
 * another call) is never retyped. */

/* Integer-register carriers (int/cstr/ptr/rc/...) round-trip through the
 * int64 slot; only xmm/by-value-distinct returns need retyping. */
bool kind_is_non_int_register_class(TypeKind k) {
    return k == TY_FLOAT /* || k == TY_FLOAT32 || ... (extend as carriers arise) */;
}

/* F5 (fn-first-class typed carrier): a `(fn [A...] : R)` parameter is routed
 * through the tur_poly_fn_t {env, fn} carrier only when every argument and the
 * result is a single-register scalar kind -- an integer/bool/float in a GP/xmm
 * register, or a pointer (cstr / ptr<T> / ptr<void>).  These are exactly the
 * kinds whose ABI is captured by their TypeKind, so the call site can cast
 * fn.fn to the concrete R(*)(void*, A...) and the make_poly_wrapper thunk can
 * carry each argument in its native kind.  A struct/ADT/union passed by value,
 * a nested function result, a type variable, etc. would lose information through
 * the kind-only carrier (e.g. type_from_kind drops a struct's def), so those
 * keep the nominal bare-function-pointer TY_FN representation. */
static bool fn_kind_is_carrier_scalar(TypeKind k) {
    switch (k) {
        case TY_NIL:  case TY_BOOL: case TY_INT:  case TY_FLOAT:
        case TY_CSTR: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* All arg kinds + the result kind of a concrete TY_FN are carrier scalars, and
 * the signature carries no substructural / fat / borrow argument annotations
 * (those discipline bits live only on the nominal TY_FN and are enforced at the
 * higher-order call site, e.g. the LT2 linear-function-type check -- the carrier
 * has no slot for them, so a function type that declares any must keep its
 * nominal representation). */
static bool fn_type_is_carrier_safe(const Type *ft) {
    if (!ft || ft->kind != TY_FN) return false;
    if (ft->as.fn.result_fat) return false;
    if (!fn_kind_is_carrier_scalar(ft->as.fn.result_kind)) return false;
    for (uint32_t i = 0; i < ft->as.fn.arity; i++) {
        if (!fn_kind_is_carrier_scalar(ft->as.fn.arg_kinds[i])) return false;
        if ((ft->as.fn.arg_flags[i] &
             (FA_LINEAR | FA_UNIQUE | FA_UNIQUE_MUT | FA_AFFINE |
              FA_RELEVANT | FA_BORROW | FA_FAT)) != 0)
            return false;
    }
    return true;
}

/* Re-stamp bare-^fat int64 calls in `tail`'s result position(s) to `target`
 * (a concrete non-int register-class kind).  Returns true if any call was
 * retyped.  Tail-precise: only result positions are visited, so a non-tail
 * bare-^fat call is never touched. */
bool retype_bare_fat_tail_calls(Expr *tail, TypeKind target) {
    if (!tail) return false;
    switch (tail->kind) {
        case EX_DO: {
            if (tail->as.do_.n == 0) return false;
            Expr *last = tail->as.do_.items[tail->as.do_.n - 1];
            bool changed = retype_bare_fat_tail_calls(last, target);
            if (changed) tail->type = last->type;   /* keep wrapper consistent */
            return changed;
        }
        case EX_LET:
        case EX_LETREC: {
            bool changed = retype_bare_fat_tail_calls(tail->as.let_.body, target);
            if (changed) tail->type = tail->as.let_.body->type;
            return changed;
        }
        case EX_IF: {
            bool c1 = retype_bare_fat_tail_calls(tail->as.if_.then_, target);
            bool c2 = retype_bare_fat_tail_calls(tail->as.if_.else_or_null, target);
            /* The if's type follows its (now-consistent) then-branch. */
            if ((c1 || c2) && tail->as.if_.then_) tail->type = tail->as.if_.then_->type;
            return c1 || c2;
        }
        case EX_CALL: {
            Binding *b = tail->as.call_.fn_binding;
            if (b && b->is_fat && b->type.kind == TY_PTR_VOID &&
                tail->type.kind == TY_INT) {
                tail->type = type_from_kind(target);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

/* bare-fat-sink-poly-box-slot0-int64-mismatch.md: recover the argument kinds
 * of a bare `^fat` param `g`'s tail-position invoke `(g x0 x1 ...)`, mirroring
 * the tail walk above.  Returns the arg count (>= 0) and fills `out`, or -1
 * when no tail-position call to `g` is found.  Used to synthesize `g`'s fn
 * signature on the enclosing function's type so a caller that boxes a
 * tur_poly_fn_t into this ^fat slot (EX_POLY_TO_FAT) can pick a slot-0 shim
 * whose ABI matches the typed-thunk cast the retyped invoke applies. */
static int bare_fat_tail_call_arg_kinds(Arena *arena, const Expr *tail,
                                        const Binding *g, TypeKind **out) {
    if (!tail) return -1;
    switch (tail->kind) {
        case EX_DO:
            return tail->as.do_.n
                ? bare_fat_tail_call_arg_kinds(arena, tail->as.do_.items[tail->as.do_.n - 1], g, out)
                : -1;
        case EX_LET:
        case EX_LETREC:
            return bare_fat_tail_call_arg_kinds(arena, tail->as.let_.body, g, out);
        case EX_IF: {
            int a = bare_fat_tail_call_arg_kinds(arena, tail->as.if_.then_, g, out);
            if (a >= 0) return a;
            return bare_fat_tail_call_arg_kinds(arena, tail->as.if_.else_or_null, g, out);
        }
        case EX_CALL:
            if (tail->as.call_.fn_binding == g) {
                /* Arena-allocate the arg-kind vector sized to the actual call --
                 * no fixed ceiling on the tail-called function's arity. */
                uint32_t n = tail->as.call_.n_args;
                TypeKind *buf = n ? (TypeKind *)arena_alloc(arena, n * sizeof(TypeKind)) : NULL;
                for (uint32_t i = 0; i < n; i++)
                    buf[i] = tail->as.call_.args[i]->type.kind;
                *out = buf;
                return (int)n;
            }
            return -1;
        default:
            return -1;
    }
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

/* captureless-algebra-arm-thin-through-carrier: classify a function's
 * tail/return leaves by closure representation.  Returns a bitmask: bit 0 set
 * iff some tail leaf is a *thin* bare fn pointer (an EX_VAR of non-boxed TY_FN
 * -- a captureless lifted lambda or a bare defn reference), bit 1 set iff some
 * tail leaf is a *fat* closure box (EX_CLOSURE / EX_FN_TO_FAT, or any boxed
 * TY_FN value).  A function returning the closure carrier whose leaves are
 * *mixed* (both bits set) returns a non-uniform value: the carrier-crossing
 * caller fat-dispatches every leaf, so a thin-pointer arm is read as a fat box
 * (slot-0 of a code address) and jumped into -> SIGSEGV.  Walks through
 * tail-position structure only (do/if/let/match/ascribe). */
/* fn-value-carrier-fat-seam-residuals (cell 1): the tail walkers classify
 * leaves by BINDING, so a let-ALIAS of a param loses its provenance -- e.g.
 * `(let [w v] w)` where `v` is a carrier (tur_poly_fn_t) fn param: `w` is a
 * local non-param binding of non-boxed TY_FN (or ptr<void>), so the stage-2
 * normalizer either thin-shims the by-value aggregate (invalid C) or skips
 * it entirely (the merge temp assign is invalid C instead).  The walkers
 * therefore carry a stack env of the let bindings they descend PAST, and a
 * leaf var resolves transitively through recorded inits (peeking through
 * ascriptions) back to the classifiable origin.  Only tail-position lets are
 * recorded -- exactly the ones whose aliases can BE a tail leaf. */
typedef struct FnTailAlias {
    const struct FnTailAlias *up;
    const Binding *binding;
    const Expr *init;
} FnTailAlias;

static const Expr *fn_tail_resolve_alias(const Expr *x, const FnTailAlias *env) {
    int guard = 32;  /* alias chains are tiny; guard against binding cycles */
    while (x && guard-- > 0) {
        while (x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
        if (x->kind != EX_VAR || !x->as.var.binding) return x;
        const Binding *vb = x->as.var.binding;
        if (vb->is_param) return x;
        const Expr *init = NULL;
        for (const FnTailAlias *a = env; a; a = a->up)
            if (a->binding == vb) { init = a->init; break; }
        if (!init) return x;
        x = init;
    }
    return x;
}

/* Push one let form's bindings onto the alias env.  The storage array lives
 * in the caller's block scope; the recursive walk into the let body happens
 * inside that scope, so the nodes stay alive exactly as long as needed.
 * Lets wider than the fixed cap simply skip recording (the resolver then
 * conservatively leaves aliases unresolved -- current behavior). */
#define FN_TAIL_PUSH_LET_ALIASES(x, env, storage)                            \
    const FnTailAlias *env##_new = (env);                                    \
    do {                                                                     \
        uint32_t _n = (x)->as.let_.n;                                        \
        if (_n > 16) _n = 0;                                                 \
        for (uint32_t _i = 0; _i < _n; _i++) {                               \
            storage[_i].binding = (x)->as.let_.bindings[_i].binding;         \
            storage[_i].init = (x)->as.let_.bindings[_i].init;               \
            storage[_i].up = (_i == 0) ? (env) : &storage[_i - 1];           \
        }                                                                    \
        if (_n > 0) env##_new = &storage[_n - 1];                            \
    } while (0)

static unsigned fn_tail_fn_leaf_kinds(const Expr *x, const FnTailAlias *env) {
    if (!x) return 0;
    switch (x->kind) {
        case EX_DO:
            return x->as.do_.n > 0
                       ? fn_tail_fn_leaf_kinds(x->as.do_.items[x->as.do_.n - 1],
                                               env)
                       : 0;
        case EX_IF:
            return fn_tail_fn_leaf_kinds(x->as.if_.then_, env) |
                   (x->as.if_.else_or_null
                        ? fn_tail_fn_leaf_kinds(x->as.if_.else_or_null, env)
                        : 0);
        case EX_LET:
        case EX_LETREC: {
            FnTailAlias _st[16];
            FN_TAIL_PUSH_LET_ALIASES(x, env, _st);
            return fn_tail_fn_leaf_kinds(x->as.let_.body, env_new);
        }
        case EX_ASCRIBE:
            return fn_tail_fn_leaf_kinds(x->as.ascribe_.inner, env);
        case EX_MATCH: {
            unsigned acc = 0;
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                acc |= fn_tail_fn_leaf_kinds(x->as.match_.arms[i].body, env);
            return acc;
        }
        case EX_CLOSURE:
        case EX_FN_TO_FAT:
            return 2u;  /* fat box */
        case EX_VAR: {
            /* fn-value-fat-normalization: binding-aware classification.  A
             * carrier (is_poly_fn) fn param is a by-value tur_poly_fn_t --
             * neither thin nor fat (bit 4).  A ^fat param or a stage-1
             * NORMALIZED nominal param already holds a fat handle even though
             * its static type is a non-boxed TY_FN -- classifying those as
             * thin would double-box them.  A local ALIAS resolves to its
             * origin first (cell 1). */
            const Expr *r = fn_tail_resolve_alias(x, env);
            if (r != x && r->kind != EX_VAR)
                return fn_tail_fn_leaf_kinds(r, env);
            if (r->kind == EX_VAR && r->as.var.binding) {
                const Binding *vb = r->as.var.binding;
                if (vb->is_poly_fn && vb->is_param) return 4u;
                if (vb->is_fat ||
                    (vb->is_param && vb->type.kind == TY_FN &&
                     fn_param_type_is_fat_normalized(&vb->type)))
                    return 2u;
            }
            if (x->type.kind == TY_FN)
                return x->type.as.fn.boxed ? 2u : 1u;  /* boxed=fat, bare=thin */
            return 0;
        }
        default:
            if (x->type.kind == TY_FN && x->type.as.fn.boxed)
                return 2u;
            return 0;
    }
}

/* captureless-algebra-arm-thin-through-carrier: rewrite every *thin* bare-fn
 * tail leaf (an EX_VAR of non-boxed TY_FN) into a fat closure box via
 * EX_FN_TO_FAT, in place at *slot.  Applied only when the body is known to be
 * mixed (see fn_tail_fn_leaf_kinds), so the function's closure-carrier result
 * is uniformly fat across all return leaves and a fat-dispatch consumer reads a
 * valid { thunk, ... } layout on every arm.  Symmetric with the ^fat-result
 * auto-shim and the parametric-ADT-field shim in elab_call.c. */
static void elab_box_thin_fn_tail_leaves(Elab *e, Expr **slot,
                                         const FnTailAlias *env) {
    Expr *x = *slot;
    if (!x) return;
    switch (x->kind) {
        case EX_DO:
            if (x->as.do_.n > 0)
                elab_box_thin_fn_tail_leaves(e, &x->as.do_.items[x->as.do_.n - 1],
                                             env);
            return;
        case EX_IF:
            elab_box_thin_fn_tail_leaves(e, &x->as.if_.then_, env);
            if (x->as.if_.else_or_null)
                elab_box_thin_fn_tail_leaves(e, &x->as.if_.else_or_null, env);
            return;
        case EX_LET:
        case EX_LETREC: {
            FnTailAlias _st[16];
            FN_TAIL_PUSH_LET_ALIASES(x, env, _st);
            elab_box_thin_fn_tail_leaves(e, &x->as.let_.body, env_new);
            return;
        }
        case EX_ASCRIBE:
            elab_box_thin_fn_tail_leaves(e, &x->as.ascribe_.inner, env);
            return;
        case EX_MATCH:
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                elab_box_thin_fn_tail_leaves(e, &x->as.match_.arms[i].body, env);
            return;
        default:
            break;
    }
    if (x->kind == EX_VAR && x->type.kind == TY_FN && !x->type.as.fn.boxed &&
        x->type.as.fn.arity >= 1 && x->type.as.fn.arity <= 5) {
        /* stage-1 normalized params and ^fat params already hold fat handles
         * despite their non-boxed static type -- shimming them here would
         * double-box (mirrors the classifier's binding-aware cases).  A local
         * ALIAS resolves to its origin first (seam-residuals cell 1): an
         * alias of a fat/normalized/carrier value must not be thin-shimmed. */
        const Expr *r = fn_tail_resolve_alias(x, env);
        const Binding *rb = (r && r->kind == EX_VAR) ? r->as.var.binding : NULL;
        if (rb &&
            (rb->is_fat || (rb->is_poly_fn && rb->is_param) ||
             (rb->is_param &&
              fn_param_type_is_fat_normalized(&rb->type))))
            return;
        if (r && r != x && r->kind != EX_VAR &&
            (r->kind == EX_CLOSURE || r->kind == EX_FN_TO_FAT ||
             (r->type.kind == TY_FN && r->type.as.fn.boxed)))
            return;  /* alias of a fat producer -- already fat */
        Type bt = x->type;
        bt.as.fn.boxed = true;
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, bt, x->span);
        shim->as.fn_to_fat_.inner = x;
        *slot = shim;
    }
}

/* fn-value-fat-normalization stage 2: normalize EVERY tail leaf of a defn
 * whose declared result is a concrete effect-free fn type
 * (fn_param_type_is_fat_normalized) onto the fat handle: a thin bare-fn leaf
 * is shimmed (EX_FN_TO_FAT), a carrier (tur_poly_fn_t) param leaf is boxed
 * (EX_POLY_TO_FAT -- previously `return (int64_t)(intptr_t)v;` on the
 * aggregate, a hard cc error), and already-fat leaves pass through.  The
 * caller then marks the declared result `boxed`, which steers the signature
 * and every consumer onto the existing fat-result plumbing. */
static void elab_normalize_fn_tail_leaves(Elab *e, Expr **slot,
                                          Type *sink_fn_type,
                                          const FnTailAlias *env) {
    Expr *x = *slot;
    if (!x) return;
    switch (x->kind) {
        case EX_DO:
            if (x->as.do_.n > 0)
                elab_normalize_fn_tail_leaves(e,
                    &x->as.do_.items[x->as.do_.n - 1], sink_fn_type, env);
            return;
        case EX_IF:
            elab_normalize_fn_tail_leaves(e, &x->as.if_.then_, sink_fn_type,
                                          env);
            if (x->as.if_.else_or_null)
                elab_normalize_fn_tail_leaves(e, &x->as.if_.else_or_null,
                                              sink_fn_type, env);
            return;
        case EX_LET:
        case EX_LETREC: {
            FnTailAlias _st[16];
            FN_TAIL_PUSH_LET_ALIASES(x, env, _st);
            elab_normalize_fn_tail_leaves(e, &x->as.let_.body, sink_fn_type,
                                          env_new);
            return;
        }
        case EX_MATCH:
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                elab_normalize_fn_tail_leaves(e, &x->as.match_.arms[i].body,
                                              sink_fn_type, env);
            return;
        default:
            break;
    }
    if (x->kind != EX_VAR || !x->as.var.binding) return;
    /* seam-residuals cell 1: resolve a local alias to its origin so the
     * classification below sees the value's REAL representation.  The
     * conversion (if any) is applied to the ORIGINAL leaf `x` -- its emitted
     * value carries the same representation as the origin (a let alias of a
     * tur_poly_fn_t param is a by-value tur_poly_fn_t copy). */
    const Expr *r = fn_tail_resolve_alias(x, env);
    Binding *vb = (r->kind == EX_VAR && r->as.var.binding)
                      ? r->as.var.binding
                      : x->as.var.binding;
    if (vb->is_poly_fn && vb->is_param) {
        Expr *conv = expr_new(e->arena, EX_POLY_TO_FAT, TYPE_PTR_VOID, x->span);
        conv->as.poly_to_fat_.inner = x;
        conv->as.poly_to_fat_.sink_fn_type =
            (sink_fn_type && sink_fn_type->kind == TY_FN) ? sink_fn_type : NULL;
        *slot = conv;
        return;
    }
    if (vb->is_fat ||
        (vb->is_param && vb->type.kind == TY_FN &&
         fn_param_type_is_fat_normalized(&vb->type)))
        return;  /* already fat */
    if (r != x && r->kind != EX_VAR &&
        (r->kind == EX_CLOSURE || r->kind == EX_FN_TO_FAT ||
         (r->type.kind == TY_FN && r->type.as.fn.boxed)))
        return;  /* alias of a fat producer -- already fat */
    if (x->type.kind == TY_FN && !x->type.as.fn.boxed &&
        x->type.as.fn.arity <= 5) {
        Type bt = x->type;
        bt.as.fn.boxed = true;
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, bt, x->span);
        shim->as.fn_to_fat_.inner = x;
        *slot = shim;
    }
}

Expr *elab_defn(Elab *e, const Form *call) {
    /* Phase R5: Check for #[no-unwind] attribute before name.
     * #[used]: retain with external C linkage (see Binding.retain_c_linkage).
     * Both are bare-symbol attributes and may appear in either order. */
    uint32_t name_idx = 1;  /* index of name in items (after 'defn') */
    bool no_unwind = false;
    bool retain_c_linkage = false;
    Form *name_f = call->as.list.items[name_idx];
    while (name_f->tag == F_SYM &&
           (name_f->as.sym == e->sym_no_unwind_attr ||
            name_f->as.sym == e->sym_used_attr)) {
        if (name_f->as.sym == e->sym_no_unwind_attr) no_unwind = true;
        else retain_c_linkage = true;
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
    /* TUR-W0042 (docs/archive/defn-shadows-return-special-form.md): defining a
     * function whose name is a reserved special form (`return`, `match`, ...)
     * is accepted and bound, but every bare call site dispatches to the special
     * form instead.  Warn HERE, at the definition, rather than leaving the
     * author to decode a type error against the caller's argument.  Suppressed
     * during stdlib auto-load and for specialization clones (which re-elaborate
     * the same Form and would double-report). */
    if (!e->in_stdlib_load && !e->bare_fat_spec_active)
        tur_warn_if_shadows_special_form(name_f->as.sym, name_f->span, "defn");

    Binding *existing = scope_lookup(e->scope, name_f->as.sym);
    /* bare-fat-result-monomorphization: a specialized clone re-elaborates the
     * same `(defn ...)` Form under a mangled name, so the original (lazy)
     * binding is still in scope under the source name.  Do not treat it as a
     * redefinition / forward decl -- the clone gets a fresh binding below. */
    if (e->bare_fat_spec_active) existing = NULL;
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
    /* Gap H item 1: collect class-constraint forms from an optional middle
     * vector when the user spells the function as the plan-canonical
     *   (defn name [TypeVars] [(Class1 V) (Class2 V) ...] [params] :ret body)
     * shape. Each constraint form is `(ClassName TypeVar1 TypeVar2 ...)`;
     * we stash them here and register them once the typeclass-resolution
     * machinery is reached below. */
    const Form *constraint_forms[8];
    uint8_t n_constraint_forms = 0;
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
            const Symbol *psym = tp->as.sym;
            /* L6 follow-up B: `^&name` in the explicit defn type-param
             * vector marks a row-kinded ([*]) type variable, the defn analog
             * of defstruct/deftype/defgadt/defdata's `^&`. The `^&` is
             * stripped so parameter and return type annotations reference
             * the bare name, and the kind is stashed so call-site row-arg
             * validation (check_row_type_arg_kind) and uses in `#row{...}`
             * elements both see kind [*]. */
            if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 2, psym->len - 2));
                fn_type_params[i] = bare;
                fn_type_param_kinds[i] = KIND_TYPEROW;
            } else if (psym->len > 1 && psym->name[0] == '^') {
                /* MB2 (constrained-hkt-forall-mode-b-plan): a `^f` type parameter
                 * is higher-kinded (`* -> *`), the defn analog of the
                 * `(defclass Functor [^f] ...)` marker -- so the body may use it
                 * applied as `(f a)`.  Strip the caret; annotations reference the
                 * bare name.  (Higher arities via `^^f` are not needed yet.) */
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 1, psym->len - 1));
                fn_type_params[i] = bare;
                fn_type_param_kinds[i] = KIND_ARROW;
            } else {
                fn_type_params[i] = psym;
                fn_type_param_kinds[i] = KIND_STAR;
            }
        }
        params_idx = name_idx + 2;

        /* Gap H item 1: detect the three-vec form
         *   (defn name [TypeVars] [(Class V) ...] [params] :ret body)
         * by looking for a third F_VEC at name_idx + 3 whose payload looks
         * like a parameter list. If we see one, treat name_idx + 2 as the
         * constraint vector and bump params_idx to name_idx + 3.
         * Each constraint must be an F_LIST `(ClassName TyVar1 TyVar2 ...)`. */
        if (call->as.list.len > name_idx + 3 &&
            call->as.list.items[name_idx + 3]->tag == F_VEC) {
            Form *maybe_constraints = call->as.list.items[name_idx + 2];
            bool looks_like_constraints = (maybe_constraints->as.list.len > 0);
            for (uint32_t ci = 0; looks_like_constraints && ci < maybe_constraints->as.list.len; ci++) {
                Form *cf = maybe_constraints->as.list.items[ci];
                if (cf->tag != F_LIST || cf->as.list.len < 1 ||
                    cf->as.list.items[0]->tag != F_SYM) {
                    looks_like_constraints = false;
                }
            }
            if (looks_like_constraints) {
                if (maybe_constraints->as.list.len > 8) {
                    diag_emit(DIAG_ERROR, maybe_constraints->span,
                              "defn: too many class constraints (max 8)");
                    return NULL;
                }
                for (uint32_t ci = 0; ci < maybe_constraints->as.list.len; ci++) {
                    constraint_forms[n_constraint_forms++] =
                        maybe_constraints->as.list.items[ci];
                }
                params_idx = name_idx + 3;
            }
        }
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
    uint32_t n_params = 0;
    /* Per-param scratch is sized to the parameter-vector length -- a safe upper
     * bound on the parameter count (each param spends >= 1 vector slot) -- and
     * arena-allocated rather than a fixed [MAX_FN_ARITY] stack array, so a defn
     * may declare any number of positional parameters (arbitrary-fn-arity: no
     * hard cap). */
    uint32_t pcap = params_f->as.list.len ? params_f->as.list.len : 1;
    TypeKind *param_kinds = (TypeKind *)arena_alloc(e->arena, pcap * sizeof(TypeKind));
    /* Phase HRT1: full type annotations for rank-2 poly params (NULL if not poly) */
    Type **param_poly_types = (Type **)arena_alloc(e->arena, pcap * sizeof(Type *));
    for (uint32_t _i = 0; _i < pcap; _i++) param_poly_types[_i] = NULL;
    /* sized-types-cross-param-unification: retain each parameter's raw type
     * annotation Form so call-site unification can re-extract size-index
     * templates (e.g. `(SizedVec n)`).  NULL when a param has no list-form
     * annotation. */
    const Form **param_type_forms_buf = (const Form **)arena_alloc(e->arena, pcap * sizeof(const Form *));
    for (uint32_t _i = 0; _i < pcap; _i++) param_type_forms_buf[_i] = NULL;

    /* CT0: Contract type predicates from param annotations { v : T | p }.
     * Collected during param parsing; injected as pre-checks before the body. */
    const Form **ct_param_preds     = (const Form **)arena_alloc(e->arena, pcap * sizeof(const Form *));
    const char **ct_param_varnames  = (const char **)arena_alloc(e->arena, pcap * sizeof(const char *));
    uint32_t    *ct_param_param_idx = (uint32_t *)arena_alloc(e->arena, pcap * sizeof(uint32_t));
    uint32_t n_ct_param_preds = 0;
    for (uint32_t _ci = 0; _ci < pcap; _ci++) {
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

    /* Gap H item 1: register the (Class TyVar) forms collected from the
     * optional middle vector now that constraint_list is allocated. Each
     * form looks like `(HasPos W)`; we look up the class and stash a
     * TypeConstraint pointing at it. param_idx stays -1 until the param
     * loop binds W; type_arg stays TYPE_UNKNOWN. */
    if (n_constraint_forms > 0) {
        TypeConstraint *new_list = (TypeConstraint *)arena_alloc(e->arena,
            n_constraint_forms * sizeof(TypeConstraint));
        for (uint8_t ci = 0; ci < n_constraint_forms; ci++) {
            const Form *cf = constraint_forms[ci];
            const Symbol *class_sym = cf->as.list.items[0]->as.sym;
            TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, class_sym);
            if (!tc) {
                diag_emit(DIAG_ERROR, cf->span,
                          "defn: typeclass '%s' in constraint is not defined",
                          class_sym->name);
                return NULL;
            }
            const Symbol *tyvar_sym = NULL;
            if (cf->as.list.len >= 2 && cf->as.list.items[1]->tag == F_SYM) {
                tyvar_sym = cf->as.list.items[1]->as.sym;
            }
            new_list[ci].typeclass = tc;
            new_list[ci].type_arg = TYPE_UNKNOWN;
            new_list[ci].param_idx = -1;
            new_list[ci].tyvar = tyvar_sym;
            new_list[ci].return_resolved = false;
        }
        constraint_list = new_list;
        n_constraints = n_constraint_forms;
    }

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
    /* LB1: ^borrow annotation marks the next parameter as a non-consuming borrow */
    bool next_param_borrow    = false;
    /* A#1: ^fat annotation marks the next parameter as a fat-closure consumer */
    bool next_param_fat       = false;
    /* AR5: variadic rest parameter state */
    bool is_variadic = false;
    TypeKind rest_kind = TY_INT;  /* default rest element type */
    Type *rest_full_type = NULL;  /* typed-variadic: full Type for user-defined rest */
    /* closure-drop-glue (mw-compose-of): a `^borrow` immediately before `&`
     * marks the whole rest list as borrowed -- the callee reads/invokes each
     * element but retains none, so the caller may free a fresh uniquely-owned
     * closure passed as a rest arg at its own scope exit.  Recorded only under
     * the experiment; flag-off it stays false so the fn type is unchanged. */
    bool rest_borrow_flag = false;

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
                /* Accept fused `& rest :int` (F_KEYWORD) and spaced
                 * `& rest : int` (F_TYPE_ANN{inner: F_SYM/F_KEYWORD}). */
                Form *type_eff = type_p;
                if (type_p->tag == F_TYPE_ANN && type_p->as.list.len == 1 &&
                    (type_p->as.list.items[0]->tag == F_SYM ||
                     type_p->as.list.items[0]->tag == F_KEYWORD)) {
                    type_eff = type_p->as.list.items[0];
                }
                if (type_eff->tag == F_KEYWORD || type_eff->tag == F_SYM) {
                    if (!resolve_variadic_rest_type(e, type_eff,
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
            /* closure-drop-glue (mw-compose-of): consume a pending `^borrow`
             * (`^borrow & rest ...`) as the rest-list borrow marker. */
            if (next_param_borrow) {
                rest_borrow_flag = true;
            }
            next_param_borrow = false;
            /* Add rest param as a regular int binding (cons-list pointer at runtime) */
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
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

        /* LB1: ^borrow annotation marks the next parameter as a non-consuming
         * borrow.  A linear/affine argument passed to this parameter is read
         * without discharging its single-consumption obligation (see the
         * call-site handling in elab_call.c).  Used by stdlib resource-handle
         * accessors (fs/tmpfile-path, mutex-lock, ...). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_borrow) {
            next_param_borrow = true;
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
            /* This is a type variable that the pending constraints apply to.
             *
             * dispatch-tyvar-name-independence: the binder name (`B` in
             * `^Backend B`) MUST be registered as a function type parameter.
             * Without it, a later parameter annotated with that name (`b : B`)
             * does not resolve against fn_type_params, so it falls through to
             * the KB-026 demotion pass below -- which rewrites the parameter's
             * type to an anonymous `<struct>` placeholder unless the name
             * happens to collide with some loaded ADT/struct's type-param name
             * (fn_name_is_adt_tyvar).  That accidental collision is exactly why
             * `^Backend B` resolved for B/A/.../K/V but not for T/X/Z or any
             * multi-letter binder: dispatch depended on the tyvar's *spelling*.
             * Registering the binder makes `b : B` a genuine named TY_TYVAR for
             * every spelling, so constraint-driven dispatch no longer hinges on
             * the name. */
            const Symbol *binder = p->as.sym;
            bool already_param = false;
            for (uint8_t k = 0; k < n_fn_type_params; k++) {
                if (fn_type_params[k] == binder) { already_param = true; break; }
            }
            if (!already_param && n_fn_type_params < 8) {
                fn_type_params[n_fn_type_params] = binder;
                fn_type_param_kinds[n_fn_type_params] = KIND_STAR;
                n_fn_type_params++;
            }

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
                /* type_arg is resolved later from the call-site argument; the
                 * binder name is recorded so the constraint knows which type
                 * variable (and hence which parameter) it dispatches on. */
                constraint_list[n_constraints + c].typeclass = pending_constraints[c];
                constraint_list[n_constraints + c].type_arg = TYPE_UNKNOWN;
                constraint_list[n_constraints + c].param_idx = -1;
                constraint_list[n_constraints + c].tyvar = binder;
                constraint_list[n_constraints + c].return_resolved = false;
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
            /* For F_TYPE_ANN, unwrap to the inner type form first */
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            /* sized-types-cross-param-unification: record the raw type form so
             * call sites can re-extract the size-index template. */
            if (n_params > 0 && n_params <= MAX_FN_ARITY)
                param_type_forms_buf[n_params - 1] = type_form;
            /* SZ8 projection-size recovery: also stash the declared annotation
             * Form on the parameter binding itself, so a callee whose argument
             * is this parameter (or a field projection of it) can recover its
             * static size index at the call site. */
            params[n_params - 1]->decl_type_form = type_form;
            /* Phase CCL (fn-first-class-application): a spaced `: fn` annotation
             * (the bare `fn` keyword/symbol) is the first-class poly-closure
             * carrier -- mirror the fused `:fn` keyword handling above. */
            if ((type_form->tag == F_SYM || type_form->tag == F_KEYWORD) &&
                type_form->as.sym->len == 2 &&
                memcmp(type_form->as.sym->name, "fn", 2) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = NULL;
                continue;
            }
            /* Parse as a type expression — supports (forall [a] (-> a a)), (-> a b), etc. */
            Type *ann = fn_type_from_form(e, type_form,
                                          fn_type_params, fn_type_param_kinds, n_fn_type_params);
            if (!ann) return NULL;
            /* CT0: a contract-typed parameter takes its BASE type at the C
             * level, and its predicate becomes an entry check (CT1) plus --
             * under `refined` -- a hypothesis for the return obligation (RT1).
             *
             * Collecting from the RESOLVED TYPE rather than from the raw Form
             * is what makes all three spellings work uniformly: the bare
             * brace form `[x {v : int | p}]`, the current `[x : #refine{...}]`
             * (which the reader wraps in an F_TYPE_ANN -- the Form-shaped
             * check missed this one entirely, so such a parameter silently got
             * NO runtime check at all), and a NAMED refinement alias
             * `[x : Nat]` declared with `deftype` (stdlib/refine.tur), which
             * has no contract Form at the use site at all. */
            if (ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
                TypeKind base_kind = ann->as.contract_.base_type->kind;
                param_kinds[n_params - 1] = base_kind;
                params[n_params - 1]->type = *ann->as.contract_.base_type;
                if (ann->as.contract_.predicate && n_ct_param_preds < MAX_FN_ARITY) {
                    ct_param_preds[n_ct_param_preds]     = ann->as.contract_.predicate;
                    ct_param_varnames[n_ct_param_preds]  = ann->as.contract_.var_name;
                    ct_param_param_idx[n_ct_param_preds] = n_params - 1;
                    n_ct_param_preds++;
                }
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
                Binding *pb = params[n_params - 1];
                /* F5 (fn-first-class-application, typed carrier): a plain
                 * (non-fat, non-substructural) `(fn [A...] : R)` parameter is the
                 * *typed* first-class `:fn` carrier -- the same tur_poly_fn_t
                 * {env, fn} representation as a bare `:fn`, but with a known
                 * concrete signature.  Routing it through the poly carrier rather
                 * than the bare-function-pointer regular path lets it uniformly
                 * hold a named fn, a lambda, OR a capturing closure (each boxed
                 * into {env, fn}), and round-trip float/cstr/ptr argument and
                 * result kinds via the concrete-signature cast at the call site
                 * (see elab_poly_call + emit_expr.c is_poly_call).  Fat (^fat)
                 * and substructural params keep their nominal TY_FN type -- they
                 * have their own calling conventions/discipline checks.
                 * See docs/reported/fn-first-class-float-carrier-gap.md. */
                bool plain = !pb->is_fat && !pb->is_borrow && !pb->is_unique &&
                             !pb->is_mut && !pb->is_linear && !pb->is_affine &&
                             !pb->is_relevant;
                /* A function type carrying an effect row (e.g. `(fn [:cstr]
                 * #{Write} :nil)`) must keep its nominal TY_FN type so the
                 * effect row propagates to callers when the param is invoked --
                 * the tur_poly_fn_t carrier has no effect-row slot.  Likewise a
                 * type variable in the signature (rank-2 / polymorphic element)
                 * needs the arg_full_types path.  Stay on the regular path for
                 * those; only fully-concrete, effect-free signatures become the
                 * typed carrier. */
                bool effectful = ann->as.fn.effect_row != NULL;
                /* typed-c-abi-function-pointers: a cfnptr is a *bare* C function
                 * pointer, not a tur_poly_fn_t {env, fn} carrier -- it must keep
                 * its nominal TY_FN (cfnptr) type so the param lowers to the
                 * concrete `R (*)(A...)` typedef and only captureless callbacks
                 * are admitted.  Never demote it onto the poly carrier. */
                bool carrier_ok = plain && !effectful && !ann->as.fn.cfnptr &&
                                  !ann->as.fn.is_variadic &&
                                  ann->as.fn.arity <= (MAX_FN_ARITY - 1) &&
                                  !fn_type_has_named_tyvar(ann) &&
                                  fn_type_is_carrier_safe(ann);
                /* fn-value-fat-normalization stage 2: a NESTED concrete
                 * effect-free fn RESULT inside a fn-typed param annotation is
                 * marked boxed, recursively -- stage-2 producers return fat
                 * handles for such types, so an annotation left thin would
                 * make `((f 1) 2)` thin-dispatch a fat handle (the
                 * curried-fn-typed-param SIGSEGV found by the stage-2 suite
                 * measurement). */
                {
                    Type *nr = ann->as.fn.result_full_type;
                    while (nr && nr->kind == TY_FN && !nr->as.fn.boxed &&
                           fn_result_type_is_fat_normalized(nr)) {
                        nr->as.fn.boxed = true;
                        nr = nr->as.fn.result_full_type;
                    }
                }
                /* repr-trace (representation-consolidation-meta-plan increment
                 * 0): with --emit-abi-trace, print the representation this
                 * fn-typed parameter was routed onto, and -- for the thin
                 * nominal TY_FN fallthrough -- which gate forced it there.
                 * This makes the per-boundary decision diffable: a
                 * consolidation increment can assert "only the intended
                 * boundaries moved" by diffing traces. */
                if (g_emit_abi_trace) {
                    const char *repr;
                    const char *why = "";
                    if (carrier_ok)                repr = "carrier";
                    else if (pb->is_fat)           repr = "fat";
                    else if (ann->as.fn.cfnptr)    repr = "cfnptr";
                    else {
                        repr = "thin-fn";
                        if (effectful)                              why = " effect-row";
                        else if (ann->as.fn.is_variadic)            why = " variadic";
                        else if (fn_type_has_named_tyvar(ann))      why = " tyvar-sig";
                        else if (!fn_type_is_carrier_safe(ann))     why = " non-scalar-sig";
                        else if (!plain)                            why = " substructural";
                        else                                        why = " arity";
                    }
                    fprintf(stderr, "repr-trace %u:%u fn-param %s %s%s\n",
                            p->span.line, p->span.col_start,
                            pb->name ? pb->name->name : "_", repr, why);
                }
                if (carrier_ok) {
                    param_kinds[n_params - 1] = TY_PTR_VOID;
                    pb->type = TYPE_PTR_VOID;
                    pb->is_poly_fn = true;
                    pb->poly_type = ann;        /* concrete signature (not a forall) */
                    param_poly_types[n_params - 1] = ann;
                    continue;
                }
                /* Plain function type annotation */
                param_kinds[n_params - 1] = TY_FN;
                params[n_params - 1]->type = *ann;
                /* LT2 / Defect-A: always store full TY_FN type in param_poly_types so
                 * it round-trips into arg_full_types for all callers -- including the
                 * early-update path (lines 1896-1904) that propagates arg_full_types to
                 * the forward-declaration binding before the body is elaborated.  Without
                 * this, a sibling defn that forward-references a ^fat fn-typed parameter
                 * sees arg_full_types=NULL on the forward binding and cannot subtype-check
                 * against the declared (fn [...] ret) signature.  The -Xlinear gate was
                 * a red herring: the full type is needed for all callers, not just linear
                 * ones.  Non-fat plain TY_FN parameters that go through the carrier_ok
                 * path above already set param_poly_types unconditionally. */
                param_poly_types[n_params - 1] = ann;
            } else {
                /* Other type annotation — use the kind */
                param_kinds[n_params - 1] = ann->kind;
                params[n_params - 1]->type = *ann;
                /* IT1: For union-typed parameters, store full type in param_poly_types
                 * so it propagates into arg_full_types for subtyping checks at call sites. */
                if (ann->kind == TY_UNION) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* IT2: For intersection-typed parameters, same propagation. */
                if (ann->kind == TY_INTERSECTION) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* PH1.1: For handler-typed parameters, store the full type so
                 * the declared handled-effect row + value/result kinds reach
                 * arg_full_types at the call site, enabling row-precise
                 * argument checking (PH1.2) and a precise mismatch diagnostic
                 * (PH1.3) instead of a kind-only handler<?, ?, ?> comparison. */
                if (ann->kind == TY_HANDLER) {
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
            if (params[n_params - 1]->type.copy_kind == CK_LINEAR) {
                params[n_params - 1]->is_linear = true;
            }
            /* ST2: ref<T> params without an explicit discipline annotation are
             * inferred as SK_LINEAR. */
            if (!params[n_params - 1]->is_linear
                    && !params[n_params - 1]->is_affine
                    && !params[n_params - 1]->is_relevant
                    && params[n_params - 1]->type.kind == TY_REF) {
                params[n_params - 1]->is_linear = true;
                params[n_params - 1]->type.substruct = SK_LINEAR;
            }
            /* UT0: Propagate uniqueness from the type annotation */
            if (next_param_unique) {
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
            /* Phase CCL (fn-first-class-application): a bare `:fn` parameter is a
             * first-class poly-closure carrier (tur_poly_fn_t {env, fn}), the same
             * representation typeclass-method `:fn` params use.  Marking it
             * is_poly_fn (with a NULL poly_type to distinguish it from a rank-2
             * forall) makes it directly callable -- `(g x)` routes through
             * elab_poly_call -- and coercible into a `^fat` sink (EX_POLY_TO_FAT)
             * or constructed from a lambda/closure (EX_POLY_WRAP) at call sites. */
            if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = NULL;
                continue;
            }
            /* CC4 (cps-transform-plan): a flavored continuation parameter --
             * :cont (cloneable), :escape-cont, or :serial-cont. The flavor
             * selects which runtime (k v) application sugar resumes against. */
            {
                int _cflav = cont_flavor_from_name(kw->name);
                if (_cflav >= 0) {
                    param_kinds[n_params - 1] = TY_CONT;
                    params[n_params - 1]->type =
                        type_cont_flavored(TY_INT, (ContFlavor)_cflav);
                    if (cont_name_is_multishot(kw->name))
                        params[n_params - 1]->type.copy_kind = CK_MULTISHOT;
                    continue;
                }
            }
            /* ptr-generic-parameterised-type: a typed `:ptr<T>` parameter. */
            {
                Type *pt = ptr_type_from_keyword_name(e, kw->name, kw->len,
                                                      p->span, NULL,
                                                      fn_type_params,
                                                      fn_type_param_kinds,
                                                      n_fn_type_params);
                if (pt) {
                    param_kinds[n_params - 1] = TY_PTR_VOID;
                    params[n_params - 1]->type = *pt;
                    continue;
                }
            }
            /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
             * `:rc<T>` / `:weak<T>` / `:ref<T>` / `:lref<T>` parameter.  Resolve
             * it to the real carrier instead of a fresh tyvar named "rc<int>".
             * :ref / :lref carry the same linear-by-default discipline the bare
             * keyword forms apply below. */
            {
                Type *rt = rc_family_type_from_keyword_name(e, kw->name, kw->len,
                                                            p->span, NULL,
                                                            fn_type_params,
                                                            fn_type_param_kinds,
                                                            n_fn_type_params);
                if (rt) {
                    param_kinds[n_params - 1] = rt->kind;
                    params[n_params - 1]->type = *rt;
                    if (rt->kind == TY_REF) {
                        if (!params[n_params - 1]->is_linear
                                && !params[n_params - 1]->is_affine
                                && !params[n_params - 1]->is_relevant) {
                            params[n_params - 1]->is_linear = true;
                            params[n_params - 1]->type.substruct = SK_LINEAR;
                        }
                    } else if (rt->kind == TY_LREF) {
                        params[n_params - 1]->is_linear = true;
                    }
                    continue;
                }
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
                    if (!params[n_params - 1]->is_linear
                            && !params[n_params - 1]->is_affine
                            && !params[n_params - 1]->is_relevant) {
                        params[n_params - 1]->is_linear = true;
                        params[n_params - 1]->type.substruct = SK_LINEAR;
                    }
                } else if (_kw_kind == TY_LREF) {
                    params[n_params - 1]->type = type_lref(TY_INT);
                    params[n_params - 1]->is_linear = true;
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
                /* ST2: :ref params without an explicit discipline annotation are
                 * inferred as SK_LINEAR. */
                if (!params[n_params - 1]->is_linear
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
                params[n_params - 1]->is_linear = true;
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
                    /* Phase TA1/TA2: check defalias table.  Copy the full
                     * target type so a composite alias keeps its payload
                     * (element types, struct def, fn signature). */
                    const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                    const Type *at = NULL;
                    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                        if (e->type_alias_names[ai] == ksym) { at = e->type_alias_types[ai]; break; }
                    }
                    if (at) {
                        param_kinds[n_params - 1] = at->kind;
                        params[n_params - 1]->type = *at;
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
                        /* structdef-retirement DS-C: the Phase-D struct-name
                         * lookup here (scan struct_defs -> type_struct) is dead --
                         * struct_defs is always empty and a struct name resolves
                         * to the param_adt branch above.
                         * Phase HRT/G2: an unknown keyword is an implicit type
                         * variable.  A parameter annotation like :a where 'a' is
                         * not a known type or ADT is an implicit type variable.
                         * Mark the binding TY_TYVAR so that inside a GADT match
                         * arm, the per-arm skolem env can resolve it to a concrete
                         * type. */
                        param_kinds[n_params - 1] = TY_TYVAR;
                        params[n_params - 1]->type = type_tyvar_named(kw->name);
                        param_poly_types[n_params - 1] = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *param_poly_types[n_params - 1] = params[n_params - 1]->type;
                    }
                    } /* end TA1 else */
                }
            }
            continue;
        }

        /* No hard parameter-count ceiling: per-param scratch is arena-sized to
         * the param vector and the fn type's arg arrays are out of line, so a
         * defn may take arbitrarily many positional parameters (the emitted C
         * function has no limit of its own).  Exceeding the historical 16 is a
         * lint nudge (arbitrary-fn-arity Phase 6), fired once on the 17th. */
        if (n_params == HIGH_ARITY_SOFT_LIMIT) {
            diag_emit_with_code(DIAG_WARNING, p->span, TUR_W0041_HIGH_ARITY,
                      "defn has more than %d positional parameters; prefer a defstruct "
                      "options value or a '& rest :type' variadic (see the Function "
                      "Arity Style Guide)", HIGH_ARITY_SOFT_LIMIT);
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
        /* LB1: If the previous ^borrow annotation applied to this parameter, mark
         * it as a non-consuming borrow.  The parameter keeps its declared
         * (nominal) handle type; the borrow semantics are enforced at the call
         * site, where a linear/affine argument is read without being consumed. */
        if (next_param_borrow) {
            b->is_borrow = true;
            next_param_borrow = false;
        }
        /* A#1: If the previous ^fat annotation applied to this parameter, mark it
         * as a fat-closure consumer so call sites auto-shim bare fn arguments.
         * closure-representation-unification (Phase 0): a bare ^fat parameter
         * (no fn-type annotation) defaults to TY_INT, which is not directly
         * callable -- (g x) then errors "not a function".  A ^fat parameter
         * holds a fat-closure box, so default it to :ptr<void>, which routes
         * (g x) through the fat-dispatch path.  An explicit type annotation
         * (e.g. ^fat g :(fn [...] ...)) still overrides this default below. */
        if (next_param_fat) {
            b->is_fat = true;
            b->type = TYPE_PTR_VOID;
            param_kinds[n_params] = TY_PTR_VOID;
            next_param_fat = false;
            /* bare-fat-result-monomorphization: stamp the kind a direct call
             * `(g x)` yields.  The canonical body uses the int64 carrier (TY_INT,
             * the prior behavior); a specialized clone uses the incoming
             * closure's result kind so the call types correctly off the tail. */
            b->bare_fat_result_kind = e->bare_fat_spec_active
                                    ? e->bare_fat_spec_kind : TY_INT;
        }
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
        }
        params[n_params++] = b;
    }
    
    /* Phase 13: Lifetime annotations parsing deferred - restore original simple parsing */

    /* Parse return type annotation and body */
    /* body_start is the index of the first element after params (could be return type or body) */
    TypeKind return_kind = TY_NIL;
    AdtDef *return_adt_def = NULL; /* Phase G3: set when return type is an ADT name */
    /* structdef-retirement DS-C: return_struct_def (LT4) removed -- a struct
     * return name is a record ADT (return_adt_def); no StructDef is produced. */
    Type *return_session_type = NULL; /* SS3a/SS7: full session/role return type */
    Type *return_app_type = NULL; /* PTC4: full TY_APP return type for concrete type threading */
    Type *return_exists_type = NULL; /* F1-1: full TY_EXISTS/TY_FORALL return type so callers see the forall_ payload (without it elab_open SEGVs reading body) */
    Type *return_fn_type = NULL; /* Issue 1b: full TY_FN return type so callers see the complete function signature (arity, result_kind) rather than a zeroed TY_FN shell */
    Type *return_tyvar_type = NULL; /* GS4: full TY_TYVAR return type for call-site substitution */
    Type *return_borrow_type = NULL; /* LS2: full borrow return type (&'a T) so lifetime IDs survive */
    const Form *return_type_form_kept = NULL; /* SZ8 non-GADT: retain raw return-type Form for size-index inference at call sites */
    /* CT0/RT0: a contract RETURN type -- `: #refine{ r : T | p }`.  The
     * declared return kind is the BASE type T (mirroring how a contract
     * parameter annotation is handled); the predicate becomes a postcondition
     * on the result, checked at runtime and -- under the `refined` experiment
     * -- proved statically when a backend can, in which case no runtime check
     * is emitted at all.  Before this, a contract return type left
     * return_kind == TY_CONTRACT and every caller failed to type the call. */
    const Form *ct_ret_pred = NULL;
    const char *ct_ret_var  = NULL;
    /* RT4: a return refinement the solver PROVED about the body, when none was
     * declared.  Published on the binding so call sites can use it; never
     * turned into a runtime check. */
    const Form *rt_inferred_ret = NULL;
    const char *rt_inferred_var = NULL;
    /* True when a DECLARED return refinement is actually guaranteed of every
     * value this function returns -- either it was proved statically, or the
     * runtime check that enforces it is being emitted.  Neither holds when
     * contracts are stripped (`--no-contracts`, or a release build without
     * --keep-contracts), and assuming it then would let a call site prove a
     * goal from a fact nothing enforces. */
    bool rt_ret_guaranteed = false;
    uint32_t body_start = params_idx + 1;  /* params_idx = params vector */

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type.
     * Uppercase names are concrete effects; lowercase are row variables.
     * The row is stored as ERK_UNRESOLVED and resolved after PASS_EFFECT_LOWER. */
    EffectRow *declared_effect_row_defn = NULL;
    bool defn_has_construct_attr = false;
    bool defn_has_byval_attr = false;
    /* C2 / #reads: captured here, stamped onto the binding alongside the
     * refine_* metadata below.  1-based; 0 = no #reads annotation. */
    uint32_t reads_param_plus1_defn = 0;
    if (call->as.list.len >= body_start + 1) {
        Form *maybe_row = call->as.list.items[body_start];
        if (maybe_row->tag == F_MAP) {
            warn_legacy_fx_row(maybe_row);
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    /* M2a: pluck #{Construct} out of the effect row so it
                     * doesn't leak into PASS_EFFECT_LOWER as a phantom effect. */
                    if (item->as.sym == e->sym_construct_attr) {
                        defn_has_construct_attr = true;
                        continue;
                    }
                    /* M5 residual-straddle: pluck #{ByVal} similarly. */
                    if (item->as.sym == e->sym_byval_attr) {
                        defn_has_byval_attr = true;
                        continue;
                    }
                    syms[n_valid++] = item->as.sym;
                }
            }
            declared_effect_row_defn = effect_row_unresolved(e->arena, syms, n_valid);
            body_start++;  /* skip past the effect row map */
        }
    }
    /* C2 / #reads <sym>: a read-frame annotation at the same signature position
     * as #fx{...} (and, when both are present, after it).  Resolve the named
     * symbol to a parameter index and record it on the binding; the refinement
     * encoder consults it to grant congruence inside a frozen region.  Inert
     * until that grant lands. */
    if (call->as.list.len >= body_start + 1) {
        Form *maybe_reads = call->as.list.items[body_start];
        if (maybe_reads->tag == F_LIST &&
            maybe_reads->fx_prov == (uint8_t)PROV_READS) {
            const Form *psym = (maybe_reads->as.list.len == 2)
                                   ? maybe_reads->as.list.items[1] : NULL;
            if (!psym || psym->tag != F_SYM || !psym->as.sym) {
                diag_emit(DIAG_ERROR, maybe_reads->span,
                          "#reads must name a parameter: `#reads <param>`");
            } else {
                uint32_t found = 0;
                for (uint32_t pi = 0; pi < n_params; pi++) {
                    if (params[pi] && params[pi]->name == psym->as.sym) {
                        found = pi + 1;
                        break;
                    }
                }
                if (!found) {
                    diag_emit(DIAG_ERROR, psym->span,
                              "#reads names '%s', which is not a parameter of this function",
                              psym->as.sym->name);
                } else {
                    reads_param_plus1_defn = found;
                }
            }
            body_start++;  /* skip past the #reads annotation */
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

    /* A#1 (return position): an optional ^fat marker may precede the return
     * type, marking a fat-closure RESULT.  A non-capturing fn returned from
     * this function is auto-shimmed into a fat closure (EX_FN_TO_FAT) at the
     * tail, symmetric with the ^fat parameter marker.  Consume the marker here
     * and advance past it so the return-type parser sees the real type form. */
    bool result_fat = false;
    if (call->as.list.len >= (body_start + 1)) {
        Form *fat_f = call->as.list.items[body_start];
        if (fat_f->tag == F_SYM && fat_f->as.sym == e->sym_caret_fat) {
            result_fat = true;
            body_start++;
        }
    }

    /* Check for : return-type annotation */
    if (call->as.list.len >= (body_start + 1)) {
        Form *ret_f = call->as.list.items[body_start];
        /* Spaced `: T` where T is a single symbol or keyword: treat as if
         * fused so the full F_KEYWORD lookup ladder (alias / ADT / struct /
         * sized prim / type-param / opaque-fallback) runs.  Compound
         * `: (-> a b)` etc. still falls through to the F_TYPE_ANN branch. */
        if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
            (ret_f->as.list.items[0]->tag == F_KEYWORD ||
             ret_f->as.list.items[0]->tag == F_SYM)) {
            Form *inner = ret_f->as.list.items[0];
            ret_f = inner;
            if (ret_f->tag == F_SYM) {
                Form *kf = (Form *)arena_alloc(e->arena, sizeof(Form));
                *kf = *ret_f;
                kf->tag = F_KEYWORD;
                ret_f = kf;
            }
        }
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            /* ptr-generic-parameterised-type: a typed `:ptr<T>` return type. */
            Type *ptr_ret = ptr_type_from_keyword_name(e, kw->name, kw->len,
                                                       ret_f->span, NULL,
                                                       fn_type_params,
                                                       fn_type_param_kinds,
                                                       n_fn_type_params);
            if (ptr_ret) {
                return_kind = TY_PTR_VOID;
                return_app_type = ptr_ret;  /* threads result_full_type for codegen */
                body_start++;
                goto done_return_annotation;
            }
            /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
             * `: rc<T>` / `: weak<T>` / `: ref<T>` / `: lref<T>` return type. */
            {
                Type *rc_ret = rc_family_type_from_keyword_name(e, kw->name, kw->len,
                                                                ret_f->span, NULL,
                                                                fn_type_params,
                                                                fn_type_param_kinds,
                                                                n_fn_type_params);
                if (rc_ret) {
                    return_kind = rc_ret->kind;
                    return_app_type = rc_ret;  /* threads result_full_type for codegen */
                    body_start++;
                    goto done_return_annotation;
                }
            }
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
                    /* Phase TA1/TA2: check defalias table.  A composite target
                     * has to thread its full type through the same capture
                     * variables the `: (type-expr)` path uses below, or the
                     * payload (struct def, fn signature, app args) is lost and
                     * call sites see a bare shell of the right kind. */
                    bool alias_found = false;
                    {
                        const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                        for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                            if (e->type_alias_names[ai] == ksym) {
                                Type *at = e->type_alias_types[ai];
                                return_kind = at->kind;
                                switch (at->kind) {
                                case TY_ADT:       return_adt_def       = at->as.adt_.def; break;
                                case TY_SESSION:
                                case TY_ROLE:      return_session_type  = at; break;
                                case TY_APP:       return_app_type      = at; break;
                                case TY_EXISTS:
                                case TY_FORALL:    return_exists_type   = at; break;
                                case TY_FN:        return_fn_type       = at; break;
                                case TY_TYVAR:     return_tyvar_type    = at; break;
                                case TY_REF_IMMUT:
                                case TY_REF_MUT:   return_borrow_type   = at; break;
                                default: break;
                                }
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
                    /* structdef-retirement DS-C: the LT4 struct-name lookup
                     * (scan struct_defs -> return_struct_def/TY_STRUCT) is dead --
                     * struct_defs is always empty; a struct return name resolves
                     * to return_adt_def above. */
                    if (!return_adt_def) {
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
                /* SZ8 non-GADT: retain the raw inner form so call sites can
                 * recover a phantom Size index (e.g. `(Dense (Static 2) A)`). */
                return_type_form_kept = ret_f->as.list.items[0];
                Type *ann = fn_type_from_form(e, ret_f->as.list.items[0],
                                              fn_type_params, fn_type_param_kinds, n_fn_type_params);
                /* CT0/RT0: peel a contract return type down to its base type.
                 * Everything downstream (return_kind, the ADT/session/forall
                 * capture below, codegen, call sites) then sees exactly the
                 * type it would have seen without the refinement, and the
                 * predicate rides along as a postcondition on the result.
                 * Before this, a `: #refine{...}` return left return_kind ==
                 * TY_CONTRACT and every call site failed to type the result. */
                if (ann && ann->kind == TY_CONTRACT) {
                    ct_ret_pred = ann->as.contract_.predicate;
                    ct_ret_var  = ann->as.contract_.var_name;
                    ann = ann->as.contract_.base_type;
                }
                if (ann) {
                    return_kind = ann->kind;
                    if (ann->kind == TY_TYVAR) {
                        return_tyvar_type = ann;
                    }
                    /* SS3a: Capture full session return type so callers see the complete
                     * protocol type (e.g. Session[Rec[self, ...]]) rather than a bare
                     * TY_SESSION shell with a NULL protocol pointer. */
                    if (ann->kind == TY_SESSION || ann->kind == TY_ROLE) {
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

    /* Ergonomics: a misplaced effect annotation -- `: int #{Unsafe}` instead
     * of `#{Unsafe} : int` -- leaves the `#{...}` set as the first body form.
     * It would otherwise elaborate as an F_MAP and emit the misleading "map
     * literals are parsed but not yet supported" diagnostic, pointing the user
     * at data-literals rather than at the real cause (annotation ordering).
     * The leading effect-row parser above already consumed any correctly
     * placed `#{...}`, so an `#{...}` whose items are all bare symbols sitting
     * in body position is almost certainly a trailing effect annotation. Name
     * the real problem. */
    if (body_start < call->as.list.len) {
        Form *stray = call->as.list.items[body_start];
        if (stray->tag == F_MAP && stray->as.list.len >= 1) {
            bool all_syms = true;
            for (uint32_t j = 0; j < stray->as.list.len; j++) {
                if (stray->as.list.items[j]->tag != F_SYM) { all_syms = false; break; }
            }
            if (all_syms) {
                const Symbol *first = stray->as.list.items[0]->as.sym;
                diag_emit(DIAG_ERROR, stray->span,
                          "defn: effect annotation '#{%s%s}' must precede the "
                          "return type -- write `#{...} : <type>`, not "
                          "`: <type> #{...}`",
                          first->name,
                          stray->as.list.len > 1 ? " ..." : "");
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
    }

    /* Phase RT: parse an optional `where` clause attaching typeclass
     * constraints to this defn:
     *
     *   (defn decode [raw :ptr<void>] : (Result a (Vec SchemaError))
     *     where (HasSchema a)
     *     ...)
     *
     * The `where` symbol is followed by one or more (Class tyvar) clauses.
     * Each clause becomes a TypeConstraint appended to constraint_list.  A
     * constraint whose tyvar appears only in the return type (no parameter
     * carries it) is flagged return_resolved so the call elaborator resolves
     * it from the expected result type (RT3).  Absent a `where` clause this
     * block is a no-op -- existing programs are unaffected. */
    if (body_start < call->as.list.len) {
        Form *maybe_where = call->as.list.items[body_start];
        if (maybe_where->tag == F_SYM &&
            maybe_where->as.sym->len == 5 &&
            memcmp(maybe_where->as.sym->name, "where", 5) == 0) {
            body_start++;  /* consume `where` */
            /* Parse one or more (Class tyvar) clauses. */
            while (body_start < call->as.list.len) {
                Form *clause = call->as.list.items[body_start];
                if (clause->tag != F_LIST || clause->as.list.len != 2 ||
                    clause->as.list.items[0]->tag != F_SYM ||
                    clause->as.list.items[1]->tag != F_SYM) {
                    break;  /* not a constraint clause -- start of body */
                }
                const Symbol *cls_sym = clause->as.list.items[0]->as.sym;
                const Symbol *tv_sym  = clause->as.list.items[1]->as.sym;
                TypeClass *tc =
                    typeclass_env_lookup_typeclass(&e->typeclass_env, cls_sym);
                if (!tc) {
                    diag_emit(DIAG_ERROR, clause->span,
                              "defn: typeclass '%s' in where clause is not defined",
                              cls_sym->name);
                    e->scope = inner.parent;
                    scope_free(&inner);
                    return NULL;
                }
                /* return_resolved when no parameter type mentions the tyvar. */
                bool in_param = false;
                for (uint32_t pi = 0; pi < n_params; pi++) {
                    if (type_mentions_named_tyvar(&params[pi]->type, tv_sym->name)) {
                        in_param = true;
                        break;
                    }
                }
                uint8_t new_count = n_constraints + 1;
                TypeConstraint *new_list = (TypeConstraint *)arena_alloc(
                    e->arena, new_count * sizeof(TypeConstraint));
                if (constraint_list && n_constraints > 0) {
                    memcpy(new_list, constraint_list,
                           n_constraints * sizeof(TypeConstraint));
                }
                constraint_list = new_list;
                constraint_list[n_constraints].typeclass       = tc;
                constraint_list[n_constraints].type_arg        = TYPE_UNKNOWN;
                constraint_list[n_constraints].param_idx       = -1;
                constraint_list[n_constraints].tyvar           = tv_sym;
                constraint_list[n_constraints].return_resolved = !in_param;
                n_constraints = new_count;
                body_start++;
            }
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
        /* Grow the forward-declared type to the real arity.  Its pass-1
         * out-of-line arg arrays were sized to the pass-1 arity, so allocate
         * fresh arrays of n_params rather than write past them.  The FN_ARG_SET
         * flag writes below then land in the fresh arg_flags. */
        existing->type.as.fn.arity = n_params;
        existing->type.as.fn.arg_kinds = tur_fn_args_alloc(n_params);
        existing->type.as.fn.arg_flags = tur_fn_args_alloc(n_params);
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            existing->type.as.fn.arg_kinds[_ei] = param_kinds[_ei];
        }
        /* AR6: propagate variadic flag early so call sites in the same body
         * see is_variadic=true even if body elaboration fails before b->type=fn_type. */
        existing->type.as.fn.is_variadic = is_variadic;
        existing->type.as.fn.rest_kind   = rest_kind;
        existing->type.as.fn.rest_full_type = rest_full_type;
        existing->type.as.fn.rest_borrow = rest_borrow_flag; /* closure-drop-glue (mw-compose-of) */
        bool _any_poly = false;
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            if (param_poly_types[_ei]) { _any_poly = true; break; }
        }
        if (_any_poly) {
            Type **_aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint32_t _ei = 0; _ei < n_params; _ei++) _aFT[_ei] = param_poly_types[_ei];
            existing->type.as.fn.arg_full_types = _aFT;
        }
        /* Propagate the `:fn` poly-closure marker early too, so a *recursive*
         * call inside the body boxes its function/lambda/`:fn` argument into the
         * tur_poly_fn_t carrier (and a pass-through `:fn` value is recognised as
         * already-a-carrier) -- mirroring the same arg_poly_fn assignment applied
         * to the final fn_type below.  Without this a self-call would fall back to
         * the generic int64/void* arg cast and miscompile (aggregate-as-integer). */
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            FN_ARG_SET(existing->type.as.fn, _ei, FA_POLY_FN,
                params[_ei]->is_poly_fn &&
                (params[_ei]->poly_type == NULL ||
                 params[_ei]->poly_type->kind == TY_FN));
        }
        /* A#1 (recursion): propagate the ^fat param markers early too, so a
         * *recursive* call inside the body that passes a :ptr<void> fat box
         * into one of this function's own ^fat fn-typed parameters is accepted
         * (elab_call.c gates the :ptr<void> -> ^fat coercion on arg_fat[i]).
         * Without this the self-call sees the stale pass-1 arg_fat=false and
         * rejects the argument with a spurious "(fn []), got ptr<void>" mismatch.
         * Mirrors the final fn_type.arg_fat / result_fat assignment below. */
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            FN_ARG_SET(existing->type.as.fn, _ei, FA_FAT, params[_ei]->is_fat);
        }
        existing->type.as.fn.result_fat = result_fat;
        /* LB1 (recursion): propagate the ^borrow param markers early too, so a
         * *recursive* call inside the body that passes one of this function's
         * own linear/affine parameters back into a ^borrow slot is recognised as
         * a non-consuming read and rolls back the consumption the var-use path
         * recorded (elab_call.c gates that rollback on arg_borrow[i]).  Without
         * this, the self-call sees the stale pass-1 arg_borrow=false, leaves the
         * argument marked consumed, and -- when the recursion sits in one arm of
         * a branch -- the merge wrongly reports TUR-E0104 "consumed in one
         * branch but not the other".  The remaining substructural/ownership
         * markers are propagated alongside it so recursive self-calls see the
         * complete discipline signature, mirroring the final fn_type assignment
         * below. */
        for (uint32_t _ei = 0; _ei < n_params; _ei++) {
            FN_ARG_SET(existing->type.as.fn, _ei, FA_BORROW,     params[_ei]->is_borrow);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_LINEAR,     params[_ei]->is_linear);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_AFFINE,     params[_ei]->is_affine);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_RELEVANT,   params[_ei]->is_relevant);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_UNIQUE,     params[_ei]->is_unique);
            FN_ARG_SET(existing->type.as.fn, _ei, FA_UNIQUE_MUT, params[_ei]->is_unique &&
                                                                 params[_ei]->is_mut);
        }
        /* RR1 (recursion): propagate the *result* shape early too.  Without
         * this, a recursive self-call inside the body reads the stale pass-1
         * forward-decl result (the int64 carrier `TY_INT`) and is typed `int`
         * instead of the declared return -- surfacing as a spurious branch
         * mismatch when the self-call sits in one arm of an `if` whose other
         * arm is the real return type (e.g. `then=Box else=int`).  Mirror the
         * result_kind / result_full_type assigned to the final fn_type below.
         *
         * Only the *annotated* return is forwarded here.  An inferred return
         * (return_kind still TY_NIL / TY_TYVAR, resolved from body->type after
         * elaboration) is the genuinely mutually-dependent case and is left to
         * the post-body fn_type construction. */
        if (return_kind != TY_NIL && return_kind != TY_TYVAR) {
            existing->type.as.fn.result_kind = return_kind;
            Type *rft = NULL;
            if (return_adt_def) {
                rft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *rft = type_adt(return_adt_def);
            }
            /* structdef-retirement DS-C: the return_struct_def branch (build a
             * TY_STRUCT full-type) is dead -- return_struct_def is always NULL. */
            if (return_session_type) rft = return_session_type;
            if (return_tyvar_type)   rft = return_tyvar_type;
            if (return_fn_type && !rft) rft = return_fn_type;
            if (return_app_type)     rft = return_app_type;
            if (return_exists_type)  rft = return_exists_type;
            if (return_borrow_type && !rft) rft = return_borrow_type;
            if (rft) existing->type.as.fn.result_full_type = rft;
        }
    }

    /* Push params into the inner scope (created earlier for kind-var bindings). */
    for (uint32_t i = 0; i < n_params; i++) {
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
        for (uint32_t i = 0; i < n_params; i++) {
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
            for (uint32_t j = 0; j < n_params; j++) {
                const Type *jt = param_poly_types[j]
                    ? param_poly_types[j] : &params[j]->type;
                if (fn_type_mentions_named(jt, nm)) occ++;
            }
            if (fn_type_mentions_named(return_tyvar_type, nm) ||
                fn_type_mentions_named(return_app_type, nm) ||
                fn_type_mentions_named(return_fn_type, nm) ||
                fn_type_mentions_named(return_exists_type, nm)) occ++;
            if (occ >= 2) continue;  /* genuine: relates >=2 type positions */
            /* structdef-retirement slice 5 B2 (P1): a single-occurrence bare
             * type-param is an unresolved type variable, not a nominal struct.
             * Emit a named TY_TYVAR carrying the param name rather than the
             * legacy def-less TY_STRUCT placeholder (which codegen lowers to the
             * same int64 carrier).  See ty-struct-null-def-inventory.md P1. */
            Type unresolved = type_tyvar_named(nm);
            unresolved.copy_kind = CK_MOVE;
            unresolved.hkt_kind = KIND_STAR;
            params[i]->type = unresolved;
            param_kinds[i] = TY_TYVAR;
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
            for (uint32_t j = 0; j < n_params; j++) {
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
    for (uint32_t i = 0; i < n_params; i++) {
        if (param_poly_types[i]) fn_collect_sig_tyvars(e, param_poly_types[i]);
        else                     fn_collect_sig_tyvars(e, &params[i]->type);
    }
    fn_collect_sig_tyvars(e, return_tyvar_type);
    fn_collect_sig_tyvars(e, return_app_type);
    fn_collect_sig_tyvars(e, return_fn_type);
    fn_collect_sig_tyvars(e, return_exists_type);

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    /* bare-fat-result-monomorphization: a defn with a bare `^fat` param (no fn
     * annotation) may have a body that only typechecks once the closure's
     * result kind is known (e.g. a :float result consumed off the tail).  Try
     * the canonical (int-carrier) body under a diagnostic-capture frame; if it
     * does not typecheck, defer it -- emit no canonical FnDef and re-elaborate
     * per call site (elab_specialize_bare_fat).  Specialization is the only
     * sound way to type `(g x)` as a non-int register class off the tail. */
    bool has_bare_fat = false;
    for (uint32_t _i = 0; _i < n_params; _i++)
        if (params[_i]->is_fat && params[_i]->type.kind == TY_PTR_VOID) {
            has_bare_fat = true;
            break;
        }
    bool needs_lazy_probe = has_bare_fat && !e->bare_fat_spec_active
                            && !e->bare_fat_force_canonical;
    bool lazy_defer = false;
    uint32_t fsd_mark = e->n_file_scope_defs;
    if (needs_lazy_probe) diag_push_capture();

    e->fn_body_depth++;
    /* Phase R6: Track current function name for linting */
    e->current_fn_name = name_f->as.sym;
    if (fn_declared_unsafe) e->unsafe_depth++;
    /* van-laarhoven-lens-composition: expose this fn's single higher-kinded
     * constraint (`^Functor f`, tyvar `f : * -> *`) to its body so a nested call
     * to another constrained rank-2 fn at the same abstract functor can forward
     * this fn's dict.  Saved/restored around the body (paired with the
     * current_fn_name resets) so a nested defn's own constraint shadows it. */
    TypeClass  *saved_cur_hkt_class = e->cur_hkt_constraint_class;
    const char *saved_cur_hkt_tyvar = e->cur_hkt_constraint_tyvar;
    Binding    *saved_cur_hkt_dict  = e->cur_hkt_dict_binding;
    e->cur_hkt_constraint_class = NULL;
    e->cur_hkt_constraint_tyvar = NULL;
    e->cur_hkt_dict_binding     = NULL;
    /* constrained-hkt-pure-and-byvalue-carriers (gap 1): the ambient constraint
     * used to be recorded only for a SINGLE-constraint fn, so the moment a body
     * needed two classes on the same type constructor -- `[^Monad m ^Applicative
     * m ...]`, which is what any `bind`-then-`pure` combinator needs -- the
     * ambient went unset and return-directed dispatch had nothing to key on.
     * Accept N constraints as long as they all pin the SAME higher-kinded type
     * variable; the ambient class/dict stay the first one (the dict-clone path
     * below is still single-constraint by construction, see make_dict_clone),
     * but `cur_hkt_constraint_tyvar` now correctly reports "this body abstracts
     * over m". Constraints on DIFFERENT tyvars keep the old behaviour of
     * recording no ambient at all. */
    bool hkt_constraints_share_tyvar = (n_constraints >= 1 && constraint_list);
    for (uint8_t ci = 1; ci < n_constraints && hkt_constraints_share_tyvar; ci++) {
        if (!constraint_list[ci].typeclass || !constraint_list[ci].tyvar ||
            constraint_list[ci].tyvar != constraint_list[0].tyvar)
            hkt_constraints_share_tyvar = false;
    }
    if (hkt_constraints_share_tyvar &&
        constraint_list[0].typeclass && constraint_list[0].tyvar) {
        const Symbol *ctv = constraint_list[0].tyvar;
        for (uint8_t tpi = 0; tpi < n_fn_type_params; tpi++) {
            if (fn_type_params[tpi] == ctv &&
                fn_type_param_kinds[tpi] != KIND_STAR) {
                e->cur_hkt_constraint_class = constraint_list[0].typeclass;
                e->cur_hkt_constraint_tyvar = ctv->name;
                /* Synthetic ambient-dict binding for this fn's `Functor f` dict.
                 * Referencing it forwards the enclosing dict (Gap B2); an adapter
                 * lambda captures it through the ordinary free-var machinery. */
                const Symbol *adsym = symtab_intern(e->st,
                    strslice("__hkt_dict", 10));
                Binding *adb = binding_new(e, adsym, type_from_kind(TY_INT),
                                           false, false, call->span);
                adb->is_ambient_dict = true;
                for (TypeClassInstance *it = e->typeclass_env.instances; it;
                     it = it->next)
                    if (it->typeclass == e->cur_hkt_constraint_class) {
                        adb->ambient_repr = it; break;
                    }
                e->cur_hkt_dict_binding = adb;
                break;
            }
        }
    }
    /* generic-return-type-not-inferred-from-context: push the declared
     * return type onto the expected-type channel for the body's tail.
     * elab_call clears it before sub-arg elaboration, so this only binds
     * the body's outermost tail call -- exactly mirroring how (:: e T)
     * propagates to the call directly under the ascription.  We skip the
     * TY_TYVAR case because a bare type-variable return cannot bind a
     * callee's tyvar (no concrete witness to substitute). */
    /* RT1: everything the body records from here on is a call-site crossing
     * inside THIS function, so its hypotheses are this function's.  Marking the
     * range now and back-filling once the environment exists avoids keeping a
     * mutable "current env" that elab_defn's many error returns would have to
     * unwind correctly. */
    uint32_t rt_cs_start = e->n_refine_call_sites;

    Type *prev_body_expected = e->expected_type;
    Type *body_expected = NULL;
    /* structdef-retirement DS-C: the return_struct_def branch (a TY_STRUCT
     * expected body type) is dead -- return_struct_def is always NULL. */
    if (return_adt_def) {
        body_expected = (Type *)arena_alloc(e->arena, sizeof(Type));
        *body_expected = type_adt(return_adt_def);
    } else if (return_app_type) {
        body_expected = return_app_type;
    } else if (return_fn_type) {
        body_expected = return_fn_type;
    } else if (return_exists_type) {
        body_expected = return_exists_type;
    } else if (return_tyvar_type) {
        /* defopaque-struct-payload-fails-through-unsafe-helper: push a BARE
         * type-variable return as the body's expected type too.  It carries
         * no concrete witness here (so the ground-binding branch in elab_call
         * still declines), but a return-only-polymorphic tail call -- e.g.
         * `(__get b idx)` whose own result is a bare tyvar -- needs the
         * enclosing tyvar as a witness so elab can record the callee->caller
         * tyvar mapping.  emit then composes that mapping through the active
         * specialization to mint a per-instantiation clone instead of
         * silently lowering a by-value struct result to the int64 carrier. */
        body_expected = return_tyvar_type;
    }
    if (body_expected) e->expected_type = body_expected;
    {
        /* Internal defines: splice (define name init) into nested let forms. */
        Form *spliced = splice_internal_defines(e,
                            &call->as.list.items[body_start], n_body, call->span);
        if (spliced) {
            body = elab_form(e, spliced);
            if (!body) {
                if (needs_lazy_probe) { lazy_defer = true; body = e_nil(e, call->span); }
                else {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->expected_type = prev_body_expected;
                e->current_fn_name = NULL;
                e->cur_hkt_constraint_class = saved_cur_hkt_class;
                e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
                e->cur_hkt_dict_binding = saved_cur_hkt_dict;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
                }
            }
        } else if (n_body == 1) {
            body = elab_form(e, call->as.list.items[body_start]);
            if (!body) {
                if (needs_lazy_probe) { lazy_defer = true; body = e_nil(e, call->span); }
                else {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->n_sig_tyvars = saved_n_sig_tyvars;
                e->expected_type = prev_body_expected;
                /* Phase R6: Reset current function name */
                e->current_fn_name = NULL;
                e->cur_hkt_constraint_class = saved_cur_hkt_class;
                e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
                e->cur_hkt_dict_binding = saved_cur_hkt_dict;
                e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
                }
            }
        } else {
            Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
            for (uint32_t i = 0; i < n_body; i++) {
                items[i] = elab_form(e, call->as.list.items[body_start + i]);
                if (!items[i]) {
                    if (needs_lazy_probe) { lazy_defer = true; break; }
                    if (fn_declared_unsafe) e->unsafe_depth--;
                    e->fn_body_depth--;
                    e->n_sig_tyvars = saved_n_sig_tyvars;
                    e->expected_type = prev_body_expected;
                    /* Phase R6: Reset current function name */
                    e->current_fn_name = NULL;
                    e->cur_hkt_constraint_class = saved_cur_hkt_class;
                    e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
                    e->cur_hkt_dict_binding = saved_cur_hkt_dict;
                    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;
                    e->scope = inner.parent;
                    scope_free(&inner);
                    return NULL;
                }
            }
            if (lazy_defer) {
                body = e_nil(e, call->span);
            } else {
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
    }
    e->expected_type = prev_body_expected;
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    e->n_sig_tyvars = saved_n_sig_tyvars;
    /* Phase R6: Reset current function name */
    e->current_fn_name = NULL;
    e->cur_hkt_constraint_class = saved_cur_hkt_class;
    e->cur_hkt_constraint_tyvar = saved_cur_hkt_tyvar;
    e->cur_hkt_dict_binding = saved_cur_hkt_dict;
    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;

    /* bare-fat-result-monomorphization: close the canonical-body capture frame.
     * Suppressed errors (cerr > 0) -- or any sub-form that bailed -- mean the
     * body is not int-typeable; defer it.  Roll back any file-scope defs the
     * failed attempt registered (e.g. a nested lambda) so they do not leak into
     * emission and then re-appear from the specialization. */
    if (needs_lazy_probe) {
        uint32_t cerr = diag_pop_capture();
        if (cerr > 0 || lazy_defer) {
            lazy_defer = true;
            body = e_nil(e, call->span);
            e->n_file_scope_defs = fsd_mark;
        } else {
            lazy_defer = false;
        }
    }

    /* TY2.2: return-position widening to `any`.  A function declared `: any`
     * whose body yields a narrower type must box the result, otherwise the
     * raw value leaks into a tur_tagged_t slot and breaks C codegen.  Mirror
     * the call-argument widening via the shared coercion helper. */
    if (return_kind == TY_ANY && body && body->type.kind != TY_ANY &&
        body->type.kind != TY_NEVER) {
        body = elab_coerce_to_any(e, body);
    }

    /* bare-fat-param-non-int-result: a bare `^fat g` call in result position
     * has no recorded result type (typed int64).  When the function declares a
     * non-int register-class return, infer the closure's result type from the
     * declared return and re-stamp the tail call(s) so codegen reads the right
     * register.  Tail-precise; sound under an honest signature (see
     * docs/upcoming/bare-fat-result-type-inference-plan.md). */
    if (kind_is_non_int_register_class(return_kind)) {
        if (retype_bare_fat_tail_calls(body, return_kind) &&
            body->type.kind == TY_INT) {
            body->type = type_from_kind(return_kind);
        }
    }

    /* carrier-aware-return-unification Phase 1: reject a genuine return-position
     * conflict between the declared return and the elaborated body via the
     * shared `return_position_conflict` dispatcher (nominal -> register-class ->
     * pointer-scalar).  An ordinary `defn` is a COMMITTED position: the
     * register-class check runs symmetrically.  Skip the deferred (lazy) probe,
     * whose body is a nil placeholder, and inline-C bodies, whose value type is
     * fiat TY_NIL (trusted to match the declared return).  The int-literal ->
     * float coercion is widened in place first so it stays a coercion. */
    if (!lazy_defer && body && body->kind != EX_INLINE_C) {
        rc_widen_int_literal_to_float_return(return_kind, body);
        /* carrier-aware-return-unification Phase 2: a defn is a genuinely
         * COMMITTED position only when it does not participate in the int64
         * carrier ABI -- i.e. it is monomorphic (no type params, implicit ones
         * included in n_fn_type_params) and not `#{Unsafe}`.  A generic or
         * `#{Unsafe}` defn is RET_CLASS_CARRIER_FN: it keeps the symmetric
         * register-class (float) check but tolerates the reverse pointer-scalar
         * carrier-handle bridge. */
        ReturnClass ret_cls = (n_fn_type_params == 0 && !fn_declared_unsafe)
                                  ? RET_CLASS_COMMITTED
                                  : RET_CLASS_CARRIER_FN;
        ReturnConflict rc = return_position_conflict(
            return_adt_def, return_kind, body->type, ret_cls);
        if (rc != RET_CONFLICT_NONE) {
            const char *want = return_adt_def ? return_adt_def->name
                             : typekind_to_string(return_kind);
            Buf gb; buf_init(&gb);
            type_print(&gb, body->type);
            buf_putc(&gb, '\0');
            switch (rc) {
                case RET_CONFLICT_NOMINAL:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0001_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_REGISTER_CLASS:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- a float and a non-float live in different "
                        "register classes (xmm vs general-purpose), so this is a "
                        "register-class miscompile, not a tolerable carrier bridge",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_POINTER_SCALAR:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH,
                        "function '%s' declares return type 'cstr' but its body "
                        "returns %s -- a bare integer is never a valid string "
                        "pointer, so this is a type-erasure bug, not a tolerable "
                        "carrier bridge",
                        name_f->as.sym->name, gb.data);
                    break;
                case RET_CONFLICT_TYPE_REVERSE:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0709_RETURN_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- a string pointer is never a valid integer, "
                        "and this committed monomorphic function has no carrier to "
                        "bridge it",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_BOOL_INTEGER:
                    diag_emit_with_code(DIAG_ERROR, body->span,
                        TUR_E0709_RETURN_TYPE_MISMATCH,
                        "function '%s' declares return type '%s' but its body "
                        "returns %s -- bool and the integer family are distinct "
                        "types (boolean constants are true/false, not 0/1), and "
                        "this committed monomorphic function has no carrier to "
                        "bridge them",
                        name_f->as.sym->name, want, gb.data);
                    break;
                case RET_CONFLICT_NONE: break;  /* unreachable */
            }
            buf_free(&gb);
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
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
        bool should_check = true;
#ifdef NDEBUG
        if (!g_keep_contracts_in_release) should_check = false;
#endif
        /* Phase C2: --no-contracts strips refinement-type ({ x : T | pred })
         * contract injection too, so the flag removes *all* contract checks. */
        if (g_no_contracts) should_check = false;

        /* RT1: under the `refined` experiment, build the hypothesis
         * environment (parameter refinements + `:pre`) once and use it twice --
         * to decide this function's return-position obligations, and (via the
         * back-fill below) as the hypotheses for every call-site crossing its
         * body produced.  A proved obligation gets no runtime check emitted; an
         * unproved one keeps exactly the check it would have had with contract
         * types alone.
         *
         * The parameter's own ENTRY check is still always emitted, even when
         * every visible call site is proved: eliding it would need whole-program
         * knowledge of the call graph (including exported and indirect callers),
         * and getting that wrong drops a check that was protecting something.
         * The call-site obligations here are a diagnostic layer on top -- they
         * turn `(safe-div 10 0)` into a compile error -- not a licence to
         * remove the callee's guard. */
        const Form *rt_subject = (call->as.list.len > body_start)
                               ? call->as.list.items[call->as.list.len - 1] : NULL;
        /* Path-condition recovery uses the WHOLE body (rt_whole_body below),
         * not just the return subject -- both sides of the 2026-07-26 merge
         * implemented that fix; main's helper form is kept. */
        bool rt_ret_proven  = false;
        bool rt_post_proven = false;
        RefineEnv *rt_env = rt_build_env(e, params, n_params, ct_param_preds,
                                         ct_param_varnames, ct_param_param_idx,
                                         n_ct_param_preds, ct_pre_form);
        /* Unconditional: even a function with no refinements of its own is
         * the named caller of the crossings its body produced, and its
         * parameters still need declared sorts in the environment. */
        refine_fill_call_site_env(e, rt_cs_start, rt_env,
                                  name_f->as.sym ? name_f->as.sym->name : NULL,
                                  rt_whole_body(e, call, body_start));
        const char *rt_fn = name_f->as.sym ? name_f->as.sym->name : "?";
        char rt_what[128];
        if (ct_ret_pred) {
            snprintf(rt_what, sizeof(rt_what), "the return value of '%s'", rt_fn);
            rt_ret_proven = rt_return_obligation_proven(
                e, ct_ret_pred, ct_ret_var, rt_subject, return_kind, rt_env,
                arena_strdup(e->arena, rt_what, strlen(rt_what)), rt_fn,
                call->span);
            rt_ret_guaranteed = rt_ret_proven || should_check;
        }
        if (ct_post_form) {
            snprintf(rt_what, sizeof(rt_what), "the postcondition of '%s'", rt_fn);
            rt_post_proven = rt_return_obligation_proven(
                e, ct_post_form, "result", rt_subject, return_kind, rt_env,
                arena_strdup(e->arena, rt_what, strlen(rt_what)), rt_fn,
                call->span);
        }
        /* RT4: with no DECLARED return refinement, try to infer one from
         * the parameter refinements and the body.  Scope is deliberately
         * narrow (the plan's): a single-expression body over a numeric
         * result, at most four refined parameters.  A branching body would
         * need a path-sensitive join at the merge point, which is deferred.
         * Nothing is emitted for an inferred refinement -- it is extra
         * knowledge published to call sites, not a new runtime check. */
        if (!ct_ret_pred && n_body == 1 && n_ct_param_preds > 0 &&
            n_ct_param_preds <= 4 && rt_subject &&
            (return_kind == TY_INT || return_kind == TY_FLOAT ||
             return_kind == TY_FLOAT32 || return_kind == TY_FLOAT64)) {
            rt_inferred_ret = rt4_infer_return(e, rt_subject, return_kind,
                                               rt_env, call->span);
            if (rt_inferred_ret) rt_inferred_var = RT4_RESULT_VAR;
        }

        if (should_check && body) {
            /* Look up tur-contract-check binding */
            Binding *check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);

            /* CT1: parameter contract predicates -- entry checks.  Shared
             * with `fn` so a lambda's contract parameters behave identically. */
            body = rt_inject_param_checks(e, body, check_fn, params, n_params,
                                          ct_param_preds, ct_param_varnames,
                                          ct_param_param_idx, n_ct_param_preds,
                                          call->span);

            /* CT1: :pre — prepend (tur-contract-check pre_pred "Precondition failed") */
            if (ct_pre_form && check_fn) {
                Expr *pred_e = elab_form(e, (Form *)ct_pre_form);
                rt_diag_impure_pred(e, pred_e, call->span);
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

            /* CT1: :post and a contract RETURN type — wrap body as:
             *   (let [<var> body] (tur-contract-check pred "...failed") <var>)
             *
             * `:post` binds the result as `result`; a `: #refine{ r : T | p }`
             * return type binds it as the contract's own variable (`r`).  RT3:
             * an obligation a backend proved emits NO check at all.
             *
             * Both can be present; the return contract wraps outermost. */
            for (int ct_pass = 0; ct_pass < 2 && check_fn; ct_pass++) {
                const Form *pred_form = ct_pass == 0 ? ct_post_form : ct_ret_pred;
                if (!pred_form) continue;
                if (ct_pass == 0 ? rt_post_proven : rt_ret_proven) continue;
                body = rt_wrap_return_check(
                    e, body, check_fn, pred_form,
                    ct_pass == 1 ? ct_ret_var : NULL,
                    ct_pass == 0 ? "Postcondition failed"
                                 : "Return contract violated",
                    call->span);
            }
        }
    }

    /* LT1: At function scope exit, verify all linear params were consumed.
     *
     * Exception: an inline-C body is opaque to the linearity checker -- the C
     * code is trusted to consume (free/close/hand off) the handle, and the
     * exactly-once obligation is enforced at the Turmeric call site instead (a
     * caller's linear binding is marked consumed when passed here). Without
     * this carve-out every resource-handle accessor written in inline-C
     * (file-close, promise-fulfill, ...) would spuriously report TUR-E0100 on
     * its own linear parameter. See linear-ffi fixture for the call-site model. */
    bool lt1_param_fail = false;
    bool body_is_inline_c = (body && body->kind == EX_INLINE_C);
    if (body && !body_is_inline_c) {
        for (uint32_t _li = 0; _li < n_params; _li++) {
            /* LB1: a ^borrow parameter is non-consuming -- the caller retains
             * ownership and the borrow auto-releases at scope exit -- so it
             * carries no consumption obligation. Without this exemption a
             * Turmeric-bodied accessor that merely forwards its borrowed handle
             * to another ^borrow parameter (e.g. a thin delegating wrapper)
             * would spuriously report TUR-E0100, even though inline-C-bodied
             * borrow accessors are already exempt via body_is_inline_c. */
            if (params[_li]->is_borrow) continue;
            /* UT0: a ^unique parameter is affine (at-most-once), not linear
             * (exactly-once) -- dropping it without consuming is legal.  A
             * ref<T> param annotated ^unique can otherwise also pick up the
             * implicit ST2 linear inference; the explicit uniqueness annotation
             * wins, so exempt it from the linear must-consume obligation. */
            if (params[_li]->is_unique) continue;
            if (params[_li]->is_linear && !params[_li]->is_linear_consumed && !params[_li]->is_moved) {
                /* SS0b: Session channels get a distinct error code.
                 * SS1: include the current protocol state in the message. */
                if (params[_li]->type.kind == TY_SESSION) {
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
    if (body) {
        for (uint32_t _li = 0; _li < n_params; _li++) {
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
        if (body->type.kind == TY_ROLE && !return_session_type) {
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
    TypeKind *arg_kinds = (TypeKind *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(TypeKind));
    for (uint32_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);
    /* AR6: mark variadic functions in their type */
    fn_type.as.fn.is_variadic = is_variadic;
    fn_type.as.fn.rest_kind   = rest_kind;
    fn_type.as.fn.rest_full_type = rest_full_type;
    fn_type.as.fn.rest_borrow = rest_borrow_flag; /* closure-drop-glue (mw-compose-of) */

    /* Phase G3: attach full ADT return type if declared (for proper def propagation) */
    if (return_adt_def) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        *rft = type_adt(return_adt_def);
        fn_type.as.fn.result_full_type = rft;
    }
    /* structdef-retirement DS-C: the LT4 struct-return full-type attach is dead
     * -- return_struct_def is always NULL; a struct return is a record ADT
     * (handled by the return_adt_def branch above). */
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
            for (uint32_t i = 0; i < n_params; i++) {
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
            for (uint32_t i = 0; i < n_params; i++) {
                if (params[i]->type.kind == TY_REF_IMMUT
                        || params[i]->type.kind == TY_REF_MUT) {
                    tied = (int8_t)i;
                    break;
                }
            }
        }
        fn_type.as.fn.result_borrow_arg = tied;
    }

    /* Phase HRT1: attach full poly types for rank-2 params.
     *
     * Populate arg_full_types for ALL params, not just polymorphic ones, so
     * the saturated positional checker (elab_call.c) can enforce nominal
     * type identity for struct/opaque/ADT args. For a non-poly param we fall
     * back to the param binding's own full type. &params[i]->type is
     * arena-stable (each binding is arena-allocated), so the pointer outlives
     * the call. See docs/upcoming/positional-nominal-type-identity-fix-plan.md. */
    {
        Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
        for (uint32_t i = 0; i < n_params; i++)
            aFT[i] = param_poly_types[i] ? param_poly_types[i] : &params[i]->type;
        /* bare-fat-sink-poly-box-slot0-int64-mismatch.md: a bare `^fat` param
         * carries no fn signature (its full type is TY_PTR_VOID), so a caller
         * boxing a tur_poly_fn_t into this slot could not see that the sink
         * invokes it through a non-int64 typed-thunk cast -- slot 0 kept the
         * int64 __tur_poly_to_fat1 shim while the (retyped) invoke cast to
         * e.g. double, a register-class mismatch masked only by xmm0 luck.
         * When the bare-fat result-type inference above retyped this param's
         * tail call to a non-int register class, synthesize `(fn [args] : R)`
         * here so the box site (elab_call.c, sink_fn_type) selects the typed
         * slot-0 shim that matches the invoke. */
        if (kind_is_non_int_register_class(return_kind)) {
            for (uint32_t i = 0; i < n_params; i++) {
                if (!(params[i]->is_fat && params[i]->type.kind == TY_PTR_VOID))
                    continue;
                TypeKind *akinds = NULL;
                int n = bare_fat_tail_call_arg_kinds(e->arena, body, params[i], &akinds);
                if (n < 0) continue;
                Type *ft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *ft = type_fn(akinds, (uint32_t)n, return_kind);
                aFT[i] = ft;
            }
        }
        fn_type.as.fn.arg_full_types = aFT;
    }

    /* LT2: Store arg_linear flags from param bindings into fn_type */
    {
        bool any_linear = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_linear) { any_linear = true; break; }
        }
        if (any_linear) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_LINEAR, params[i]->is_linear);
            }
        }
    }

    /* UT0: Store arg_unique flags from param bindings into fn_type */
    {
        bool any_unique = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique) { any_unique = true; break; }
        }
        if (any_unique) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_UNIQUE, params[i]->is_unique);
            }
        }
    }
    /* UT2: Store arg_unique_mut flags (^unique ^mut) from param bindings into fn_type */
    {
        bool any_unique_mut = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique && params[i]->is_mut) { any_unique_mut = true; break; }
        }
        if (any_unique_mut) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_UNIQUE_MUT, params[i]->is_unique && params[i]->is_mut);
            }
        }
    }
    /* ST0: Store arg_affine flags from param bindings into fn_type */
    {
        bool any_affine = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_affine) { any_affine = true; break; }
        }
        if (any_affine) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_AFFINE, params[i]->is_affine);
            }
        }
    }
    /* ST0: Store arg_relevant flags from param bindings into fn_type */
    {
        bool any_relevant = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_relevant) { any_relevant = true; break; }
        }
        if (any_relevant) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_RELEVANT, params[i]->is_relevant);
            }
        }
    }
    /* LB1: Store arg_borrow flags from param bindings into fn_type so call sites
     * can borrow (read without consuming) a linear/affine argument. */
    {
        bool any_borrow = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_borrow) { any_borrow = true; break; }
        }
        if (any_borrow) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_BORROW, params[i]->is_borrow);
            }
        }
    }
    /* A#1: Store arg_fat flags from param bindings into fn_type so call sites
     * can auto-shim bare fn arguments into fat closures. */
    {
        bool any_fat = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_fat) { any_fat = true; break; }
        }
        if (any_fat) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_FAT, params[i]->is_fat);
            }
        }
    }
    /* Phase CCL (fn-first-class-application): propagate the `:fn` poly-closure
     * marker into fn_type so call sites box function/lambda/closure arguments
     * into the tur_poly_fn_t carrier.  Both the bare `:fn` carrier (poly_type ==
     * NULL, int64-default signature) and the F5 typed carrier (poly_type is a
     * concrete TY_FN signature) take this path.  A rank-2 forall param (is_poly_fn
     * with a TY_FORALL poly_type) is handled by the arg_full_types path instead. */
    for (uint32_t i = 0; i < n_params; i++) {
        if (params[i]->is_poly_fn &&
            (params[i]->poly_type == NULL ||
             params[i]->poly_type->kind == TY_FN)) {
            FN_ARG_SET(fn_type.as.fn, i, FA_POLY_FN, true);
        }
    }
    /* A#1 (return position): propagate the ^fat result marker into fn_type so
     * the emitter shims a returned non-capturing fn into a fat closure. */
    fn_type.as.fn.result_fat = result_fat;

    /* sized-types-cross-param-unification: copy the per-parameter raw type
     * annotation Forms into an arena-allocated array on fn_type so call-site
     * elaboration can re-extract size-index templates without walking back
     * through FnDef.  NULL entries (params with no list-form annotation) are
     * fine -- the call-site walk just skips them. */
    if (n_params > 0) {
        const Form **ptf = (const Form **)arena_alloc(
            e->arena, n_params * sizeof(const Form *));
        bool any = false;
        for (uint32_t i = 0; i < n_params; i++) {
            ptf[i] = param_type_forms_buf[i];
            if (ptf[i]) any = true;
        }
        fn_type.as.fn.param_type_forms = any ? ptf : NULL;
    }
    /* SZ8 non-GADT: stash the retained return-type annotation form so call
     * sites can extract a phantom Size index from the declared return type
     * (e.g. `(Dense (Static 2) A)` -> SZT_CONST 2).  Independent of n_params
     * because nullary callers (zero-arg `mk-dense-2 [] : (Dense (Static 2) A)`)
     * are the canonical case for opaque-carrier size literals.  NULL when no
     * compound return annotation was captured -- the call site then falls
     * back to the existing CtorDef-based path for sized GADT constructors. */
    fn_type.as.fn.result_type_form = return_type_form_kept;

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
        /* bare-fat-result-monomorphization: a clone takes a distinct mangled
         * name (e.g. run-with__bf_float) so it emits as its own C symbol. */
        const Symbol *bind_name = (e->bare_fat_spec_active && e->bare_fat_spec_name)
                                  ? e->bare_fat_spec_name : name_f->as.sym;
        b = binding_new(e, bind_name, fn_type, false, true, name_f->span);
        scope_add(&e->global, b);
    }
    /* bare-fat-result-monomorphization: hand the freshly-built clone binding
     * back to elab_specialize_bare_fat, which redirects the call site to it. */
    if (e->bare_fat_spec_active) e->bare_fat_spec_result = b;
    /* cps-backend-n6 cross-function resume: retain the source form of a fn with a
     * continuation parameter, so a named-fn resuming receiver used cross-function
     * (the reset is in a caller) can be re-elaborated into a
     * `multishot-effect-cont` specialization (elab_specialize_cont_receiver).
     * Cheap (one pointer) and scoped to cont-param fns; nothing else reads it. */
    if (!b->defn_form) {
        for (uint32_t _pi = 0; _pi < n_params; _pi++) {
            if (params[_pi] && params[_pi]->type.kind == TY_CONT) {
                b->defn_form = call;
                break;
            }
        }
    }
    /* RT1: publish the per-parameter refinement predicates on the binding so
     * call sites can check their arguments against them.  This is the same
     * Binding object pass 1 forward-declared, so a call elaborated BEFORE this
     * defn sees the predicates too -- the resolution pass runs after the whole
     * unit.  Parameter names ride along because a predicate may mention a
     * sibling parameter, which the call site replaces with that slot's
     * argument. */
    if (n_params > 0) {
        const Form **rp = (const Form **)arena_alloc(e->arena, n_params * sizeof(Form *));
        const char **rv = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        const char **rn = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        for (uint32_t _pi = 0; _pi < n_params; _pi++) {
            rp[_pi] = NULL;
            rv[_pi] = NULL;
            rn[_pi] = (params[_pi] && params[_pi]->name) ? params[_pi]->name->name : NULL;
        }
        for (uint32_t _ci = 0; _ci < n_ct_param_preds; _ci++) {
            uint32_t _pi = ct_param_param_idx[_ci];
            if (_pi >= n_params) continue;
            rp[_pi] = ct_param_preds[_ci];
            rv[_pi] = ct_param_varnames[_ci];
        }
        b->refine_param_preds = rp;
        b->refine_param_vars  = rv;
        b->refine_param_names = rn;
        b->n_refine_params    = n_params;
    }
    /* C2 / #reads: unconditional -- a `#reads` measure need not carry param
     * refinements, so this must not sit inside the block above. */
    b->reads_param_plus1 = reads_param_plus1_defn;
    /* A declared return refinement wins -- but only when something actually
     * enforces it (see rt_ret_guaranteed).  An INFERRED one is always safe to
     * publish: RT4 only records what a backend proved. */
    if (ct_ret_pred && rt_ret_guaranteed) {
        b->refine_return_pred = ct_ret_pred;
        b->refine_return_var  = ct_ret_var;
    } else if (!ct_ret_pred && rt_inferred_ret) {
        b->refine_return_pred = rt_inferred_ret;
        b->refine_return_var  = rt_inferred_var;
    }

    /* Phase R5: Store #[no-unwind] attribute on the binding */
    b->no_unwind = no_unwind;
    /* #[used]: retain with external C linkage under separate compilation */
    b->retain_c_linkage = retain_c_linkage;
    /* closure-drop-glue S1c (non-retaining fn-param inference): a fn-typed / ^fat
     * parameter that the body only CALLS -- never lets escape as a value -- does
     * not retain a capturing-closure argument, so that argument's heap env may be
     * freed at the call scope's exit (like a ^borrow param).  Infer the mask now,
     * from the just-elaborated body; the conservative escape analysis only ever
     * clears the bit (a false "escapes" merely preserves the status-quo leak). */
    b->nonretain_param_mask = 0;
    /* An inline-C body can STORE a fn-param invisibly to the AST escape analysis
     * (a param is a C-visible formal, not an AST capture), so a body containing
     * any inline-C is never treated as non-retaining -- otherwise its stored
     * closure arg would be freed while the C-side copy is still live (UAF). */
    /* catch-box-reader-confinement-whitelist: the same inference, for the
     * pointer-carrying scalars (cstr / ptr<void>) that a caught-Result box
     * hands out.  Trusting a hardcoded print-family name list made the
     * confinement check a soundness-maintenance footgun AND needlessly leaked
     * for a user-defined logger that is every bit as safe; inferring it from
     * the body makes it a checked property.  The inline-C guard above is
     * load-bearing here too -- a C body can stash the pointer where no AST
     * walk can see it.
     *
     * The result gate mirrors catch_box_binding_reader_confined: the param may
     * only be treated as non-retained if the function's own result cannot carry
     * it back out. */
    b->nonretain_ptr_param_mask = 0;
    if (body && !expr_subtree_has_inline_c(body)) {
        for (uint32_t _pi = 0; _pi < n_params && _pi < 32; _pi++) {
            Binding *_pb = params[_pi];
            if (!_pb) continue;
            bool _is_fnparam = _pb->is_fat || _pb->is_poly_fn ||
                               _pb->type.kind == TY_FN;
            if (_is_fnparam && !closure_binding_escapes(body, _pb))
                b->nonretain_param_mask |= (1u << _pi);
            bool _is_ptr_scalar = _pb->type.kind == TY_CSTR ||
                                  _pb->type.kind == TY_PTR_VOID;
            if (_is_ptr_scalar) {
                TypeKind _rk = (b->type.kind == TY_FN) ? b->type.as.fn.result_kind
                                                       : TY_UNKNOWN;
                bool _result_safe = false;
                switch (_rk) {
                    case TY_NIL: case TY_INT: case TY_BOOL: case TY_FLOAT:
                    case TY_INT64: case TY_UINT64: case TY_INT32: case TY_UINT32:
                    case TY_INT16: case TY_UINT16: case TY_INT8: case TY_UINT8:
                    case TY_FLOAT64: case TY_FLOAT32:
                        _result_safe = true; break;
                    default: break;
                }
                if (_result_safe && ptr_param_is_nonretaining(body, _pb, true))
                    b->nonretain_ptr_param_mask |= (1u << _pi);
            }
        }
    }
    b->returns_closure_fn_binding = expr_closure_fn_binding(body);

    /* closure-drop-glue S1c (fresh-closure-returning fn): a fn whose body is a
     * bare capturing EX_CLOSURE constructs a FRESH, uniquely-owned heap env on
     * every call and returns it.  When such a call result is consumed by a
     * non-retaining fn-param, the caller can free that env at scope exit (the
     * make-scaler headline shape).  Restricted to scalar (Copy) captures and a
     * scalar closure result so a bare `free(env)` is fully safe: no owning
     * capture to double-free/leak, and the result cannot alias the env. */
    b->returns_fresh_closure = false;
    {
        const Expr *_fc = body;
        while (_fc && _fc->kind == EX_ASCRIBE) _fc = _fc->as.ascribe_.inner;
        /* closure-drop-glue (mw-compose-of): peel a trailing `(let [...] <closure>)`
         * wrapper so a factory like `(defn make-mw [tag] (let [_t tag] (fn [n] ...)))`
         * is still recognised as fresh-closure-returning.  Every call allocates a
         * fresh env (Turmeric lets are not memoised); the let-bound intermediates
         * are the factory's own scope and do not affect the returned env, whose
         * captures/result are still checked scalar-Copy below. */
        while (_fc && (_fc->kind == EX_LET || _fc->kind == EX_ASCRIBE))
            _fc = (_fc->kind == EX_ASCRIBE) ? _fc->as.ascribe_.inner
                                            : _fc->as.let_.body;
        if (_fc && _fc->kind == EX_CLOSURE && _fc->as.closure_.closure &&
            _fc->as.closure_.closure->n_captures > 0 &&
            _fc->as.closure_.closure->fn) {
            struct Closure *_c = _fc->as.closure_.closure;
            bool _ok = fn_result_kind_is_scalar_copy(_c->fn->return_type.kind);
            for (uint8_t _ci = 0; _ok && _ci < _c->n_captures; _ci++)
                if (!_c->captures[_ci] ||
                    !fn_result_kind_is_scalar_copy(_c->captures[_ci]->type.kind))
                    _ok = false;
            b->returns_fresh_closure = _ok;
        }
    }
    b->closure_return_dispatches = expr_closure_return_dispatches(body);
    b->closure_return_dispatches_untyped = expr_closure_return_dispatches_untyped(body);
    /* let-bound-sf-loses-outer-arg-type: record whether the return *value* is
     * itself a fat closure box (capturing lambda body) vs a thin fn pointer. */
    b->returns_boxed_closure = (body && body->type.kind == TY_FN &&
                                body->type.as.fn.boxed);
    /* boxed-fn-typed-closure-return: a plain defn whose declared return is a
     * non-boxed function type but whose body yields a *capturing* closure (a
     * fat box) must carry that return value as the void* fat-closure carrier,
     * not the function's result type.  type_c_name(TY_FN non-boxed) lowers to
     * the function's *result* type's C name ("double" for (fn [float] float)),
     * but the body actually returns the heap fat box (void*) -- a hard `cc`
     * error for float/cstr results and an int-only "works by luck" otherwise.
     * Marking the declared result type `boxed` steers the signature, forward
     * declaration, and consumer let-binding all onto the void* carrier in
     * lockstep (type_c_name(TY_FN boxed) == "void *").  Typeclass-method impls
     * (__inst_) keep their dict-driven carrier path (emit_inst_fn_return_carrier),
     * and ^fat returns are already shimmed into a fat box at the call site.
     *
     * Scope this narrowly to the exact mis-lowered shape: type_c_name(TY_FN)
     * only picks the *result* type's C name when the result kind is a concrete
     * leaf (not TY_FN/TY_UNKNOWN).  A curried/nested closure return
     * (result_kind == TY_FN or TY_UNKNOWN) is already carried as int64_t there,
     * so it is correct as-is and must not be re-spelled to void*. */
    if (b->returns_boxed_closure &&
        fn_type.as.fn.result_full_type &&
        fn_type.as.fn.result_full_type->kind == TY_FN &&
        !fn_type.as.fn.result_full_type->as.fn.boxed &&
        fn_type.as.fn.result_full_type->as.fn.result_kind != TY_FN &&
        fn_type.as.fn.result_full_type->as.fn.result_kind != TY_UNKNOWN &&
        !fn_type.as.fn.result_fat &&
        !(b->name && b->name->name &&
          strncmp(b->name->name, "__inst_", 7) == 0)) {
        fn_type.as.fn.result_full_type->as.fn.boxed = true;
    }
    /* Phase M6: Store ^:export-as C name on the binding */
    b->c_export_name = c_export_name;
    /* F4: Store ^deprecated attribute on the binding */
    b->is_deprecated = is_deprecated_attr;
    b->deprecation_message = deprecation_msg;
    /* M2a: propagate #{Construct} marker */
    b->is_construct_template = defn_has_construct_attr;
    /* M5 residual-straddle: propagate #{ByVal} marker */
    b->prefer_byvalue_spec = defn_has_byval_attr;

    /* captureless-algebra-arm-thin-through-carrier: when a function returns the
     * closure carrier (declared result type is a function type) and its tail
     * leaves mix fat closure boxes (capturing arms) with thin bare fn pointers
     * (captureless arms), box the thin arms so every return leaf is uniformly
     * fat.  The carrier-crossing caller fat-dispatches the result, so a thin
     * arm left bare would be read as a fat box and jumped into -> SIGSEGV.
     * Gated on the mix: an all-thin (uniformly captureless) body stays thin and
     * is correctly thin-callable; only a fat/thin mix is non-uniform. */
    {
        bool result_is_fn =
            (return_fn_type != NULL) ||
            (fn_type.as.fn.result_full_type &&
             fn_type.as.fn.result_full_type->kind == TY_FN) ||
            (fn_type.as.fn.result_kind == TY_FN);
        if (result_is_fn && body) {
            unsigned leaves = fn_tail_fn_leaf_kinds(body, NULL);
            if ((leaves & 1u) && (leaves & 2u)) {
                elab_box_thin_fn_tail_leaves(e, &body, NULL);
            }
        }
    }

    /* fn-value-fat-normalization stage 2: a defn whose declared result is a
     * concrete effect-free fn type returns a fat handle, ALWAYS -- tail
     * leaves are normalized (thin shimmed, carrier param boxed via
     * poly-to-fat, fat passed through) and the result is marked boxed so
     * every consumer rides the existing boxed-result plumbing.  Same
     * narrowed claim as stage 1 (the shared predicate); the nested-result
     * (result_kind == TY_FN/TY_UNKNOWN) and ^fat-result carve-outs mirror
     * the returns_boxed_closure block above. */
    {
        Type *rft = fn_type.as.fn.result_full_type;
        if (body && rft && rft->kind == TY_FN && !rft->as.fn.boxed &&
            !fn_type.as.fn.result_fat &&
            rft->as.fn.result_kind != TY_FN &&
            rft->as.fn.result_kind != TY_UNKNOWN &&
            fn_result_type_is_fat_normalized(rft) &&
            !(b->name && b->name->name &&
              strncmp(b->name->name, "__inst_", 7) == 0)) {
            elab_normalize_fn_tail_leaves(e, &body, rft, NULL);
            rft->as.fn.boxed = true;
        }
    }

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(fd, 0, sizeof(FnDef));
    fd->binding = b;
    if (b) b->source_fn_def = fd;  /* MB1: Binding -> FnDef link */
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = is_variadic;  /* AR5: propagate variadic flag */
    if (is_variadic) {
        extern bool g_has_variadics;
        g_has_variadics = true;  /* AR8: tell emit_module to include __tur_cons_of */
    }
    /* Mirror emit_fns.c:377's predicate on the binding so call sites can
     * detect an inline-C callee without walking back to the FnDef. */
    b->body_is_inline_c = (body && body->kind == EX_INLINE_C);
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
    for (uint32_t i = 0; i < n_params; i++) {
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
        } else if (param_kinds[i] == TY_PTR_VOID
                   && params[i]->type.kind == TY_PTR_VOID
                   && params[i]->type.as.ptr.inner) {
            /* ptr-generic-parameterised-type: preserve the pointee type so
             * emit.c lowers a `:ptr<T>` parameter to `T *`, not `void *`. */
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_INT && params[i]->type.kind == TY_ADT
                   && params[i]->type.as.adt_.def) {
            /* CONV-S1/B2: an untyped param (defaulted to TY_INT) that was used as
             * a match scrutinee had its binding refined to the inferred ADT during
             * body elaboration (elab_match).  The body runs before this realiser,
             * so preserve that ADT type instead of collapsing back to int64 -- the
             * parameter and the by-value match body then agree once the gate flips.
             * type_c_name(TY_ADT) is int64_t while the gate is off, so the emitted
             * signature is unchanged today. */
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_APP && params[i]->type.kind == TY_APP
                   && params[i]->type.as.app.fn) {
            /* multi-param-struct-annotation-degenerate-tyapp: preserve the
             * spined type application (`(Map K V)`, `(MutableMap K V)`) built
             * by fn_type_from_form via type_app -- its `app.fn` chain reaches
             * the head StructDef.  Without this the realiser rebuilt the param
             * as type_from_kind(TY_APP), a spineless shell whose `app.fn`/
             * `app.arg` are NULL, defeating every spine-walking predicate
             * (type_extract_struct_app / type_is_heap_struct / type_is_heap_vec).
             * Single-param `(Vec A)` already kept its spine through a different
             * realisation path; this brings multi-param apps in line. */
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
                     : (return_app_type && return_app_type->kind == TY_APP
                        && return_app_type->as.app.fn)
                         ? *return_app_type
                         : type_from_kind(return_kind);
    /* Phase 15: Store collected constraints */
    fd->constraints.constraints = constraint_list;
    fd->constraints.n_constraints = n_constraints;
    fd->constraints.cap_constraints = n_constraints;
    /* class-defn-constraint-not-discharged-at-call-site: backlink the
     * constraint set onto the function's binding so a call site can re-discharge
     * each `^Class T` obligation at the concrete type its arguments pin.  Only a
     * constrained defn gets the link; NULL (the memset default) means "no
     * obligations to check". */
    if (fd->binding && n_constraints > 0) {
        fd->binding->fn_constraints = &fd->constraints;
    }

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
        for (uint32_t _ci = 0; _ci < n_params; _ci++) {
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

    /* bare-fat-result-monomorphization: the canonical (int) body did not
     * typecheck -- emit no FnDef.  The binding keeps its honest TY_FN (declared
     * return + ^fat arg flags) so call sites resolve it, retains the Form for
     * per-call-site re-elaboration, and is swept after top-level elaboration so
     * a never-specialized one still surfaces its deferred diagnostic. */
    if (lazy_defer) {
        b->bare_fat_lazy = true;
        b->defn_form = call;
        elab_track_bare_fat_lazy(e, b);
        return e_nil(e, call->span);
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

    /* CT0/CT1: contract-typed parameters of this lambda, collected during
     * parameter parsing and injected as entry checks once the body exists. */
    const Form *ct_param_preds[MAX_FN_ARITY];
    const char *ct_param_varnames[MAX_FN_ARITY];
    uint32_t    ct_param_param_idx[MAX_FN_ARITY];
    uint32_t    n_ct_param_preds = 0;

    /* Edge 1: snapshot and clear the active letrec self-exclude group at entry.
     * This `fn` IS the letrec init's top-level lambda iff elab_letrec set the
     * group right before calling us; a direct self/mutual call in our own body
     * must therefore be excluded from capture (handled by recursion machinery).
     * Clearing it immediately means any NESTED closure inside this body sees an
     * empty group and so captures the letrec-bound value through its env. */
    Binding **letrec_self_group   = e->letrec_self_group;
    uint32_t  n_letrec_self_group = e->letrec_self_group_n;
    e->letrec_self_group   = NULL;
    e->letrec_self_group_n = 0;

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
            const Symbol *psym = tp->as.sym;
            /* L6 follow-up B: `^&name` marks a row-kinded ([*]) type
             * variable on a `fn` form, mirroring the defn change. */
            if (psym->len > 2 && psym->name[0] == '^' && psym->name[1] == '&') {
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 2, psym->len - 2));
                fn_type_params[i] = bare;
                fn_type_param_kinds[i] = KIND_TYPEROW;
            } else if (psym->len > 1 && psym->name[0] == '^') {
                /* MB2 (constrained-hkt-forall-mode-b-plan): a `^f` type parameter
                 * is higher-kinded (`* -> *`), the defn analog of the
                 * `(defclass Functor [^f] ...)` marker -- so the body may use it
                 * applied as `(f a)`.  Strip the caret; annotations reference the
                 * bare name.  (Higher arities via `^^f` are not needed yet.) */
                const Symbol *bare = symtab_intern(e->st,
                    strslice(psym->name + 1, psym->len - 1));
                fn_type_params[i] = bare;
                fn_type_param_kinds[i] = KIND_ARROW;
            } else {
                fn_type_params[i] = psym;
                fn_type_param_kinds[i] = KIND_STAR;
            }
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

    /* Parse params.  Per-param scratch is arena-sized to the param-vector length
     * (a safe upper bound), not a fixed [MAX_FN_ARITY] stack array, so a lambda
     * may take any number of positional parameters. */
    uint32_t pcap = params_f->as.list.len ? params_f->as.list.len : 1;
    Binding **params = NULL;
    uint32_t n_params = 0;
    TypeKind *param_kinds = (TypeKind *)arena_alloc(e->arena, pcap * sizeof(TypeKind));
    Type **param_full_types = (Type **)arena_alloc(e->arena, pcap * sizeof(Type *));
    for (uint32_t _i = 0; _i < pcap; _i++) param_full_types[_i] = NULL;
    /* AR5: variadic rest parameter state for fn */
    bool fn_is_variadic = false;
    TypeKind fn_rest_kind = TY_INT;
    Type *fn_rest_full_type = NULL;  /* typed-variadic: full Type for user-defined rest */
    /* LT0 (call-cc-completion CC4.4): ^linear marks the next lambda param linear
     * (exactly-once).  Consumption is tracked by the shared var-use path; the
     * LT1 drop check below mirrors elab_defn.  This lets (call/cc (fn [^linear k]
     * ...)) opt into one-shot-by-contract continuations (OQ3). */
    bool next_param_linear = false;
    /* LB1: ^borrow marks the next lambda param as a non-consuming borrow. */
    bool next_param_borrow = false;
    /* A#1 (bare-fat-lambda-param-plan): ^fat marks the next lambda param as a
     * fat-closure consumer, mirroring the defn-param path.  A bare ^fat binder
     * (no fn-type annotation) defaults to :ptr<void> + is_fat so (g x) inside
     * the body dispatches through the fat protocol instead of erroring "'g' is
     * not a function".  An explicit `:(fn [...] :T)` annotation overrides the
     * :ptr<void> default below. */
    bool next_param_fat = false;

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
                /* Accept fused `& rest :int` and spaced `& rest : int`. */
                Form *type_eff = type_p;
                if (type_p->tag == F_TYPE_ANN && type_p->as.list.len == 1 &&
                    (type_p->as.list.items[0]->tag == F_SYM ||
                     type_p->as.list.items[0]->tag == F_KEYWORD)) {
                    type_eff = type_p->as.list.items[0];
                }
                if (type_eff->tag == F_KEYWORD || type_eff->tag == F_SYM) {
                    if (!resolve_variadic_rest_type(e, type_eff,
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
                params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
            }
            param_kinds[n_params] = TY_INT;
            Binding *rest_b = binding_new(e, rest_p->as.sym, TYPE_INT, false, false, rest_p->span);
            rest_b->is_param = true;
            params[n_params++] = rest_b;
            break;
        }
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN || p->tag == F_LIST ||
            p->tag == F_VEC || p->tag == F_CONTRACT_TYPE) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "fn: type annotation without preceding parameter");
                return NULL;
            }
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            Type *ann = fn_type_from_form(e, type_form,
                                          fn_type_params, fn_type_param_kinds, n_fn_type_params);
            if (!ann) return NULL;
            /* CT0: a contract-typed lambda parameter takes its BASE type, and
             * its predicate becomes an entry check -- exactly as for a `defn`.
             * Without the peel the parameter's type stayed TY_CONTRACT and
             * every call site failed with `expected { _ : ? | ... }, got int`,
             * so `#refine` on a lambda parameter did not compile at all. */
            if (ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
                if (ann->as.contract_.predicate && n_ct_param_preds < MAX_FN_ARITY) {
                    ct_param_preds[n_ct_param_preds]     = ann->as.contract_.predicate;
                    ct_param_varnames[n_ct_param_preds]  = ann->as.contract_.var_name;
                    ct_param_param_idx[n_ct_param_preds] = n_params - 1;
                    n_ct_param_preds++;
                }
                ann = ann->as.contract_.base_type;
            }
            param_kinds[n_params - 1] = ann->kind;
            params[n_params - 1]->type = *ann;
            /* Record the full type whenever the annotation carries information
             * its bare TypeKind cannot reconstruct.  TY_APP / TY_TYVAR / TY_FN
             * and any named-tyvar-bearing type obviously qualify, but so do
             * nominal TY_STRUCT / TY_ADT (and opaque newtypes, which lower to
             * TY_STRUCT): type_from_kind(TY_STRUCT) yields a def-less placeholder,
             * losing the nominal identity.  Without the full type on the
             * closure's arg_full_types, a generic callee binding its type
             * parameter through this closure's argument position (e.g. T in
             * `(fn [T] void)`) falls back to that def-less placeholder, so a
             * later bare `item : T` value parameter fails to unify against the
             * real opaque/struct type.  Recording it keeps the nominal identity
             * intact across the binding. */
            if (ann->kind == TY_APP || ann->kind == TY_TYVAR || ann->kind == TY_FN ||
                ann->kind == TY_STRUCT || ann->kind == TY_ADT ||
                fn_type_has_named_tyvar(ann)) {
                param_full_types[n_params - 1] = ann;
            }
            /* hkt-cata-function-arg (producer side): this lambda is being
             * elaborated as the value of an expected function type whose
             * argument at this position is ITSELF a function -- i.e. the lambda
             * is a function-typed carrier B = (fn [(fn ...) ...] ...), the value
             * an algebra arm folds to.  Such a carrier value is only ever
             * fat-dispatched (through the generic catamorphism / match-arm
             * carrier binding), and a capturing closure flows into this
             * function-typed slot, so the argument must cross the boundary as a
             * uniform fat box and be fat-dispatched here.  Mark the param
             * is_fat so (k ...) reads slot 0 instead of calling the fat box's
             * address as a thin function pointer -> SIGSEGV.  Symmetric with
             * the call-site arg_fat marking in elab_call.c.  Gated on the
             * expected arg being a (non-cfnptr) function type so an ordinary
             * fn-typed param of a non-carrier lambda is untouched. */
            if (ann->kind == TY_FN && !ann->as.fn.cfnptr &&
                e->expected_type && e->expected_type->kind == TY_FN &&
                n_params >= 1 && (n_params - 1) < e->expected_type->as.fn.arity &&
                e->expected_type->as.fn.arg_full_types) {
                Type *exp_arg =
                    e->expected_type->as.fn.arg_full_types[n_params - 1];
                if (exp_arg && exp_arg->kind == TY_FN && !exp_arg->as.fn.cfnptr) {
                    params[n_params - 1]->is_fat = true;
                }
            }
            continue;
        }
        /* LT0 (CC4.4): ^linear marks the next param linear (exactly-once). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_linear) {
            next_param_linear = true;
            continue;
        }
        /* LB1: ^borrow marks the next lambda param as a non-consuming borrow. */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_borrow) {
            next_param_borrow = true;
            continue;
        }
        /* A#1 (bare-fat-lambda-param-plan): ^fat marks the next param as a
         * fat-closure consumer (see next_param_fat declaration above). */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_fat) {
            next_param_fat = true;
            continue;
        }
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: parameter name must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* No hard parameter-count ceiling (see the defn path); arbitrary-fn-arity
         * Phase 6 lint nudge past the historical 16 cap. */
        if (n_params == HIGH_ARITY_SOFT_LIMIT) {
            diag_emit_with_code(DIAG_WARNING, p->span, TUR_W0041_HIGH_ARITY,
                      "fn has more than %d positional parameters; prefer a defstruct "
                      "options value or a '& rest :type' variadic (see the Function "
                      "Arity Style Guide)", HIGH_ARITY_SOFT_LIMIT);
        }
        /* Untyped fn params preserve the existing int default. */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        b->is_param = true;
        /* Bidirectional inference (constrained-generic-as-value-bakes-
         * representative.md): when this lambda is elaborated against an expected
         * function type (pushed by the call site for a fn-typed parameter), type
         * an UN-annotated param from the expected arg type instead of defaulting
         * to the int carrier.  This lets `(fn [b] (count-it b))` passed where
         * `(fn [Box] int)` is expected see `b : Box`, so the inner direct
         * typeclass-method dispatch pins its real instance rather than baking the
         * carrier representative.  Gated to NON-primitive expected types
         * (struct/adt/app): for primitives the int carrier is already correct, so
         * leaving them untouched avoids churning existing codegen. */
        if (e->expected_type && e->expected_type->kind == TY_FN &&
            n_params < e->expected_type->as.fn.arity &&
            e->expected_type->as.fn.arg_full_types) {
            Type *exp_full = e->expected_type->as.fn.arg_full_types[n_params];
            if (exp_full && (exp_full->kind == TY_STRUCT ||
                             exp_full->kind == TY_ADT ||
                             exp_full->kind == TY_APP)) {
                param_kinds[n_params] = exp_full->kind;
                b->type = *exp_full;
                param_full_types[n_params] = exp_full;
            }
        }
        if (next_param_linear) {
            b->is_linear = true;
            next_param_linear = false;
        }
        /* LB1: a ^borrow lambda param reads its linear/affine argument without
         * consuming it (see call-site handling). */
        if (next_param_borrow) {
            b->is_borrow = true;
            next_param_borrow = false;
        }
        /* A#1 (bare-fat-lambda-param-plan): a bare ^fat lambda param holds a
         * fat-closure box.  Default it to :ptr<void> + is_fat so (g x) routes
         * through the fat-dispatch path; a later `:(fn [...] :T)` annotation
         * overrides the type (is_fat stays set, exactly as on defn params). */
        if (next_param_fat) {
            b->is_fat = true;
            b->type = TYPE_PTR_VOID;
            param_kinds[n_params] = TY_PTR_VOID;
            next_param_fat = false;
        }
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
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
            declared_effect_row_fn = effect_row_unresolved(e->arena, syms, n_valid);
            body_start = params_idx + 2;
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_fn, e->sym_effect_unsafe);

    /* Check for : return-type annotation */
    if (call->as.list.len >= (body_start + 1)) {
        Form *ret_f = call->as.list.items[body_start];
        /* Spaced `: T` where T is a single symbol or keyword: treat as if
         * fused so the full F_KEYWORD lookup ladder (alias / ADT / struct /
         * sized prim / type-param / opaque-fallback) runs.  Compound
         * `: (-> a b)` etc. still falls through to the F_TYPE_ANN branch. */
        if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
            (ret_f->as.list.items[0]->tag == F_KEYWORD ||
             ret_f->as.list.items[0]->tag == F_SYM)) {
            Form *inner = ret_f->as.list.items[0];
            ret_f = inner;
            if (ret_f->tag == F_SYM) {
                Form *kf = (Form *)arena_alloc(e->arena, sizeof(Form));
                *kf = *ret_f;
                kf->tag = F_KEYWORD;
                ret_f = kf;
            }
        }
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            uint8_t type_param_idx = 0;
            Type *ptr_ret = ptr_type_from_keyword_name(e, kw->name, kw->len,
                                                       ret_f->span, NULL,
                                                       fn_type_params,
                                                       fn_type_param_kinds,
                                                       n_fn_type_params);
            /* rc-angle-bracket-annotation-becomes-tyvar: a typed reference-family
             * `: rc<T>` / `: weak<T>` / `: ref<T>` / `: lref<T>` return type. */
            Type *rc_ret = rc_family_type_from_keyword_name(e, kw->name, kw->len,
                                                            ret_f->span, NULL,
                                                            fn_type_params,
                                                            fn_type_param_kinds,
                                                            n_fn_type_params);
            if (ptr_ret) {
                return_kind = TY_PTR_VOID;
                return_full_type = ptr_ret;
            } else if (rc_ret) {
                return_kind = rc_ret->kind;
                return_full_type = rc_ret;
            } else if (fn_type_param_index(fn_type_params, n_fn_type_params, kw, &type_param_idx)) {
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
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else {
                /* struct-return-through-closure-loses-type (CURRY-V0): a bare
                 * single-symbol return type that names a struct or ADT must
                 * resolve to that nominal type and carry it on the lambda's
                 * fn_type (result_full_type), exactly as elab_defn does.
                 * Without this, `(fn [..] : Person ..)` left the return type as
                 * a TY_TYVAR named "Person", so the lifted thunk returned the
                 * int64 carrier and the value's struct type was lost at the
                 * call site -- a following (.field ...) could not resolve. */
                bool resolved_nominal = false;
                /* defalias table (mirror elab_defn's TA1/TA2 ladder) */
                {
                    const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
                    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
                        if (e->type_alias_names[ai] == ksym) {
                            return_kind      = e->type_alias_kinds[ai];
                            return_full_type = e->type_alias_types[ai];
                            resolved_nominal = true;
                            break;
                        }
                    }
                }
                /* ADT name */
                if (!resolved_nominal) {
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            return_kind = TY_ADT;
                            return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *return_full_type = type_adt(e->adt_defs[ai]);
                            resolved_nominal = true;
                            break;
                        }
                    }
                }
                /* structdef-retirement DS-D: the struct-name loop (scan
                 * e->struct_defs -> TY_STRUCT full-type) is dead -- the
                 * struct_defs registry is always empty; a struct return name
                 * resolves as a record ADT above or falls through to a tyvar. */
                if (!resolved_nominal) {
                    return_kind = TY_TYVAR;
                    return_full_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *return_full_type = type_tyvar_named(kw->name);
                }
            }
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = fn_type_from_form(e, ret_f->as.list.items[0],
                                              fn_type_params, fn_type_param_kinds, n_fn_type_params);
                /* CT0: a contract return type on a lambda contributes its base
                 * type; the predicate is a `defn`-level construct (there is no
                 * postcondition injection point for an anonymous fn), so it is
                 * peeled and dropped rather than left to break the result kind. */
                if (ann && ann->kind == TY_CONTRACT && ann->as.contract_.base_type)
                    ann = ann->as.contract_.base_type;
                if (ann) {
                    return_kind = ann->kind;
                    if (ann->kind == TY_TYVAR || fn_type_has_named_tyvar(ann)) {
                        return_full_type = ann;
                    }
                    if (ann->kind == TY_FN) {
                        return_fn_type = ann;
                    }
                    /* fat-closure-dispatch-aggregate-return: a lambda whose
                     * declared return is an aggregate/applied type (TY_APP like
                     * (Pair A B), a concrete TY_STRUCT, or a TY_ADT) must carry
                     * the full type on its fn_type.  Otherwise emit lowers the
                     * lifted thunk's C return type to int64_t while the body
                     * returns the struct by value -- a hard C compile error. */
                    if (!return_full_type &&
                        (ann->kind == TY_APP || ann->kind == TY_ADT)) {
                        return_full_type = ann;
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
    for (uint32_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    /* KB-025: accumulate this closure's signature type variables on top of the
     * enclosing function's set (see elab_defn for the rationale). */
    uint8_t saved_n_sig_tyvars = e->n_sig_tyvars;
    for (uint32_t i = 0; i < n_params; i++) {
        fn_collect_sig_tyvars(e, &params[i]->type);
    }
    fn_collect_sig_tyvars(e, return_full_type);
    fn_collect_sig_tyvars(e, return_fn_type);

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    /* closure-capture-escapes-linearity: snapshot the substructural state of
     * every enclosing linear/unique binding BEFORE the body runs.
     *
     * Elaborating the body marks an OUTER binding consumed at the point the
     * closure is built, not where it is called, so `(f) (f)` on a closure that
     * frees a captured `^linear` was two consumptions the checker never saw --
     * a real double-free.  Comparing this snapshot against the state after the
     * body identifies exactly which captures the body CONSUMES (as opposed to
     * merely reads), which is the distinction the fix turns on: a read-only
     * capture is safe at any arity and must stay accepted.
     *
     * Arena-allocated rather than malloc'd on purpose -- elab_fn has many early
     * returns between here and the use site below, and the compiler path is
     * leak-checked. */
    uint32_t n_lin_snap = 0;
    for (const Scope *cur = e->scope; cur; cur = cur->parent)
        for (uint32_t i = 0; i < cur->n; i++)
            if (cur->bindings[i]->is_linear || cur->bindings[i]->is_unique)
                n_lin_snap++;
    FnCaptureLinSnap *lin_snap =
        (FnCaptureLinSnap *)arena_alloc(e->arena,
                                        (n_lin_snap ? n_lin_snap : 1)
                                            * sizeof(FnCaptureLinSnap));
    {
        uint32_t k = 0;
        for (const Scope *cur = e->scope; cur; cur = cur->parent)
            for (uint32_t i = 0; i < cur->n; i++) {
                Binding *b = cur->bindings[i];
                if (!b->is_linear && !b->is_unique) continue;
                lin_snap[k].binding       = b;
                lin_snap[k].was_consumed  = b->is_linear_consumed;
                lin_snap[k].was_moved     = b->is_moved;
                k++;
            }
    }

    e->fn_body_depth++;
    if (fn_declared_unsafe) e->unsafe_depth++;
    /* Propagate the lambda's declared return type onto the expected-type
     * channel during body elaboration.  Mirrors what elab_defn already does
     * for top-level defns, so bare parametric-ADT constructor calls
     * (`(PFail)` in `(fn [xs] : (PRes A) ...)`) infer their type parameter
     * from the declared return -- otherwise a lambda body's constructors
     * lack the enclosing expected type that elab_defn provides. */
    Type *saved_fn_body_expected = e->expected_type;
    if (return_full_type) {
        e->expected_type = return_full_type;
    } else if (return_fn_type) {
        e->expected_type = return_fn_type;
    }
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
    e->expected_type = saved_fn_body_expected;
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    e->n_sig_tyvars = saved_n_sig_tyvars;
    e->fn_entry_outer_scope = saved_fn_entry_outer_scope;

    /* LT1 (call-cc-completion CC4.4): at lambda scope exit, verify every ^linear
     * param was consumed.  A ^linear continuation handed to (call/cc (fn [^linear
     * k] ...)) that is never invoked is a dropped linear value (TUR-E0100). */
    if (body) {
        for (uint32_t _li = 0; _li < n_params; _li++) {
            /* LB1: ^borrow params are non-consuming; they carry no consumption
             * obligation (see the defn-scope LT1 check above). */
            if (params[_li]->is_borrow) continue;
            if (params[_li]->is_linear && !params[_li]->is_linear_consumed
                    && !params[_li]->is_moved) {
                diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                    TUR_E0100_LINEAR_DROPPED,
                                    "linear parameter '%s' dropped without being consumed",
                                    params[_li]->name->name);
            }
        }
    }

    /* CT1: inject this lambda's parameter contract checks.  Done BEFORE capture
     * analysis so the final body is what gets scanned, and while the inner
     * scope is still current so the predicate can be elaborated in it. */
    if (n_ct_param_preds > 0 && body && rt_contracts_emitted()) {
        Binding *ct_check_fn = scope_lookup(&e->global, e->sym_tur_contract_check);
        body = rt_inject_param_checks(e, body, ct_check_fn, params, n_params,
                                      ct_param_preds, ct_param_varnames,
                                      ct_param_param_idx, n_ct_param_preds,
                                      call->span);
    }

    /* Phase 3: Capture analysis - collect free variables in the body */
    /* We need to do this before popping the scope */
    uint32_t n_captures = 0;
    Binding **captures = collect_free_vars(body, params, n_params,
                                           letrec_self_group, n_letrec_self_group,
                                           &n_captures);

    /* closure-capture-escapes-linearity: a closure that CONSUMES a captured
     * linear/unique value is itself linear/unique.
     *
     * Consuming the capture once per call means the closure carries exactly the
     * obligation the captured value had, so inheriting its copy_kind makes every
     * bad shape fall out of checks that already exist, with no new diagnostic:
     * calling it twice is TUR-E0101 use-after-consume, `(rc/of f)` is TUR-E0103,
     * and dropping it unused is TUR-E0100 (correct -- the captured resource would
     * never be released).  The unique case lands on TUR-E0201/E0202 the same way.
     *
     * CONSUMES, not merely captures.  A closure that only READS a captured linear
     * value is safe at any arity; a blanket capture-based rule (the shape
     * TUR-E0500 uses for ^multishot handlers, where N invocations are
     * definitional) would reject a large amount of working code -- 73 fixtures
     * carry captures, and the httpd middleware closures that dominate that list
     * are read-only. */
    bool capture_consumes_linear = false;
    bool capture_consumes_unique = false;
    for (uint32_t ci = 0; ci < n_captures && !(capture_consumes_linear
                                               && capture_consumes_unique); ci++) {
        Binding *cb = captures[ci];
        if (!cb->is_linear && !cb->is_unique) continue;
        for (uint32_t si = 0; si < n_lin_snap; si++) {
            if (lin_snap[si].binding != cb) continue;
            /* Transitioned during THIS body: the body is what consumed it. */
            if (cb->is_linear && cb->is_linear_consumed && !lin_snap[si].was_consumed)
                capture_consumes_linear = true;
            if (cb->is_unique && cb->is_moved && !lin_snap[si].was_moved)
                capture_consumes_unique = true;
            break;
        }
    }

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
        } else if (!return_full_type &&
                   (body->type.kind == TY_APP || body->type.kind == TY_ADT) &&
                   !fn_type_has_named_tyvar(&body->type)) {
            /* closure-result-monomorphization Phase 2 grounding: an unannotated
             * lambda whose body is a carrier-ABI aggregate (e.g. a monadic
             * continuation `(fn [x] (some (+ x 1)))` returning `(Option int)`)
             * previously recorded only result_kind=TY_APP and dropped the full
             * type, leaving the fn type's result as `(type-app ? ?)`.  That
             * erasure is exactly why `m7_collect_tyvar_bindings` could not ground
             * `b` in `bind`/`ap`'s `(m b)` result (the continuation's concrete
             * `(Option int)` was unrecoverable), forcing the carrier fallback.
             * Preserve the body's full aggregate type so the element grounds.
             *
             * Gate on a FULLY-GROUND body: a residual free named tyvar (e.g. the
             * fixed arm `B` of a partial ok-biased `(Result int B)` instance)
             * would make the lifted wrapper `emit_abi_fn_is_generic_unsafe` and
             * get skipped as dead generic code while its address is still taken
             * (an undeclared `__poly_N`).  Only concrete aggregates are safe to
             * record here. */
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = body->type;
            return_full_type = rft;
        }
    }

    /* bare-fat-param-non-int-result (Phase A): same retype as elab_defn, run on
     * each lambda's own body so a `fn` whose declared result is non-int-class
     * and whose tail is a bare-^fat call gets the correct call result type.
     * Per-lambda scope -> no cross-lambda leakage. */
    if (kind_is_non_int_register_class(return_kind)) {
        if (retype_bare_fat_tail_calls(body, return_kind) &&
            body->type.kind == TY_INT) {
            body->type = type_from_kind(return_kind);
        }
    }

    /* Create function type */
    TypeKind *arg_kinds = (TypeKind *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(TypeKind));
    for (uint32_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);
    /* AR6: mark variadic functions in their type */
    fn_type.as.fn.is_variadic = fn_is_variadic;
    fn_type.as.fn.rest_kind   = fn_rest_kind;
    fn_type.as.fn.rest_full_type = fn_rest_full_type;
    {
        bool any_full = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (param_full_types[i]) { any_full = true; break; }
        }
        if (any_full) {
            Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint32_t i = 0; i < n_params; i++) aFT[i] = param_full_types[i];
            fn_type.as.fn.arg_full_types = aFT;
        }
    }
    /* bare-fat-sink-poly-box-slot0-int64-mismatch.md: same bare-^fat signature
     * synthesis as elab_defn, for a lambda sink.  A retyped bare `^fat` param
     * gets a `(fn [args] : R)` recorded on arg_full_types so a caller boxing a
     * tur_poly_fn_t selects the typed slot-0 shim. */
    if (kind_is_non_int_register_class(return_kind)) {
        for (uint32_t i = 0; i < n_params; i++) {
            if (!(params[i]->is_fat && params[i]->type.kind == TY_PTR_VOID))
                continue;
            TypeKind *akinds = NULL;
            int n = bare_fat_tail_call_arg_kinds(e->arena, body, params[i], &akinds);
            if (n < 0) continue;
            if (!fn_type.as.fn.arg_full_types) {
                Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
                for (uint32_t k = 0; k < n_params; k++) aFT[k] = NULL;
                fn_type.as.fn.arg_full_types = aFT;
            }
            Type *ft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *ft = type_fn(akinds, (uint8_t)n, return_kind);
            fn_type.as.fn.arg_full_types[i] = ft;
        }
    }
    if (return_full_type) {
        fn_type.as.fn.result_full_type = return_full_type;
    }
    if (return_fn_type) {
        fn_type.as.fn.result_full_type = return_fn_type;
    }
    /* A#1 (bare-fat-lambda-param-plan): propagate ^fat param flags into the
     * lambda's fn_type so call sites auto-shim bare fn arguments into fat
     * closures, mirroring the defn path. */
    {
        bool any_fat = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_fat) { any_fat = true; break; }
        }
        if (any_fat) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_FAT, params[i]->is_fat);
            }
        }
    }
    /* Phase CCL (fn-first-class-application): propagate `:fn` poly-closure
     * markers into the lambda's fn_type, mirroring the defn path (bare `:fn`
     * carrier with poly_type == NULL, or the F5 typed carrier with a concrete
     * TY_FN poly_type). */
    for (uint32_t i = 0; i < n_params; i++) {
        if (params[i]->is_poly_fn &&
            (params[i]->poly_type == NULL ||
             params[i]->poly_type->kind == TY_FN)) {
            FN_ARG_SET(fn_type.as.fn, i, FA_POLY_FN, true);
        }
    }
    /* LB1: propagate ^borrow param flags into the lambda's fn_type. */
    {
        bool any_borrow = false;
        for (uint32_t i = 0; i < n_params; i++) {
            if (params[i]->is_borrow) { any_borrow = true; break; }
        }
        if (any_borrow) {
            for (uint32_t i = 0; i < n_params; i++) {
                FN_ARG_SET(fn_type.as.fn, i, FA_BORROW, params[i]->is_borrow);
            }
        }
    }

    /* closure-capture-escapes-linearity: stamp the inherited obligation onto the
     * closure's own type.  A `let` binding whose initializer type carries
     * CK_LINEAR is already marked linear (elab_forms.c), so this is all it takes
     * for the existing use-after-consume / rc-wrap / dropped checks to start
     * seeing the closure.  Linear wins over unique when a body consumes both --
     * it is the stricter obligation. */
    if (capture_consumes_linear)      fn_type.copy_kind = CK_LINEAR;
    else if (capture_consumes_unique) fn_type.copy_kind = CK_UNIQUE;

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
    /* pr-386 regression fix: mark this as a lifted-lambda helper so the
     * let-bound source_binding alias rule (elab_forms.c) does not chain to it.
     * A captureless closure-returning lambda must be called through the
     * closure-dispatch protocol on the let binding, not by a direct call to
     * __fn_N (whose C signature returns the int64 carrier, not a fn pointer). */
    b->is_lifted_lambda = true;
    b->returns_closure_fn_binding = expr_closure_fn_binding(body);

    /* RT1: publish this lambda's contract parameters on its lifted thunk
     * binding.  A `let` bound to the lambda copies them across (elab_forms.c),
     * which is what lets `(let [f (fn [x : Pos] ...)] (f 0))` be checked at the
     * call the way a call to a named function is. */
    if (n_ct_param_preds > 0 && n_params > 0) {
        const Form **rp = (const Form **)arena_alloc(e->arena, n_params * sizeof(Form *));
        const char **rv = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        const char **rn = (const char **)arena_alloc(e->arena, n_params * sizeof(char *));
        for (uint32_t _pi = 0; _pi < n_params; _pi++) {
            rp[_pi] = NULL;
            rv[_pi] = NULL;
            rn[_pi] = (params[_pi] && params[_pi]->name) ? params[_pi]->name->name : NULL;
        }
        for (uint32_t _ci = 0; _ci < n_ct_param_preds; _ci++) {
            uint32_t _pi = ct_param_param_idx[_ci];
            if (_pi >= n_params) continue;
            rp[_pi] = ct_param_preds[_ci];
            rv[_pi] = ct_param_varnames[_ci];
        }
        b->refine_param_preds = rp;
        b->refine_param_vars  = rv;
        b->refine_param_names = rn;
        b->n_refine_params    = n_params;
    }
    b->closure_return_dispatches = expr_closure_return_dispatches(body);
    b->closure_return_dispatches_untyped = expr_closure_return_dispatches_untyped(body);
    /* let-bound-sf-loses-outer-arg-type: see the defn path -- record whether the
     * lambda's return *value* is a fat closure box vs a thin fn pointer. */
    b->returns_boxed_closure = (body && body->type.kind == TY_FN &&
                                body->type.as.fn.boxed);

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(fd, 0, sizeof(FnDef));
    fd->binding = b;
    if (b) b->source_fn_def = fd;  /* MB1: Binding -> FnDef link */
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = fn_is_variadic;  /* AR5: propagate variadic flag */
    if (fn_is_variadic) {
        extern bool g_has_variadics;
        g_has_variadics = true;  /* AR8: tell emit_module to include __tur_cons_of */
    }
    /* Mirror emit_fns.c:377's predicate so call sites referencing this
     * anonymous fn binding can see the same flag a defn binding would. */
    b->body_is_inline_c = (body && body->kind == EX_INLINE_C);
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_fn) {
        b->type.as.fn.effect_row = declared_effect_row_fn;
    }
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint32_t i = 0; i < n_params; i++) {
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
        
        /* Modify the FnDef to include env parameter as first parameter.  No hard
         * ceiling: the env-prepended arg-kind scratch below is arena-sized to
         * new_n_params (arbitrary-fn-arity). */
        uint32_t new_n_params = n_params + 1;
        
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
        for (uint32_t i = 0; i < n_params; i++) {
            new_params[i + 1] = params[i];
            new_param_types[i + 1] = params[i]->type;
        }
        
        /* Update FnDef with new params */
        fd->params = new_params;
        fd->n_params = new_n_params;
        fd->param_types = new_param_types;
        
        /* Update function type to include env parameter (arena-sized scratch,
         * no fixed cap). */
        TypeKind *new_arg_kinds = (TypeKind *)arena_alloc(e->arena, new_n_params * sizeof(TypeKind));
        new_arg_kinds[0] = TY_PTR_VOID;  /* env parameter */
        for (uint32_t i = 0; i < n_params; i++) {
            new_arg_kinds[i + 1] = param_kinds[i];
        }
        Type new_fn_type = type_fn(new_arg_kinds, new_n_params, return_kind);
        if (b->type.as.fn.arg_full_types) {
            Type **shifted = (Type **)arena_alloc(e->arena, new_n_params * sizeof(Type *));
            shifted[0] = NULL;
            for (uint32_t i = 0; i < n_params; i++) shifted[i + 1] = b->type.as.fn.arg_full_types[i];
            new_fn_type.as.fn.arg_full_types = shifted;
        }
        if (b->type.as.fn.result_full_type) {
            new_fn_type.as.fn.result_full_type = b->type.as.fn.result_full_type;
        }
        if (return_fn_type) {
            new_fn_type.as.fn.result_full_type = return_fn_type;
        }
        /* two-level-sf-closure / vec-get-typed-fat-closure-readback: shift the
         * ^fat param markers (and the ^fat result marker) into the env-prepended
         * thunk type.  type_fn() zero-inits arg_fat, so without this the thunk
         * type a *capturing* closure dispatches through loses its ^fat flags --
         * and a call site that passes a captureless lambda into one of the
         * closure's ^fat fn params reads arg_fat[env+i]=false, skips the
         * EX_FN_TO_FAT shim, and feeds a bare fn pointer to a fat-dispatch
         * consumer (slot-0 read of code bytes -> SEGV).  arg_full_types above is
         * shifted for exactly the same reason; arg_fat must travel with it. */
        for (uint32_t i = 0; i < n_params; i++) {
            FN_ARG_SET(new_fn_type.as.fn, i + 1, FA_FAT, FN_ARG_FLAG(b->type.as.fn, i, FA_FAT));
        }
        new_fn_type.as.fn.result_fat = b->type.as.fn.result_fat;
        /* closure-capture-escapes-linearity: carry the inherited obligation onto
         * the env-prepended thunk type.  type_fn() zero-inits copy_kind, and this
         * is the CAPTURING path -- the only one that can have inherited one -- so
         * without this the mark set above is dropped before any binding sees it. */
        new_fn_type.copy_kind = b->type.copy_kind;
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
        closure->is_shift_receiver = false;   /* arena mem is not zeroed */
        closure->is_effect_payload = false;
        closure->capture_drop_insts = NULL;   /* Model R #1b (arena is non-zeroing) */
        closure->capture_clone_insts = NULL;

        /* closure-drop-glue (Model R): make each OWNING capture participate in the
         * env's lifecycle so it is released when the env dies instead of leaking.
         * Gated on the experiment; the base language is unchanged.  Two kinds:
         *
         *  (a) type-honesty -- an OWNED `^fat` closure handle: MOVE it into the env
         *      (mark the source consumed), so the env is its sole owner and a second
         *      capture is a compile-time use-after-move, not a double-free.  The
         *      is_fat drop-glue walk releases it via TUR_CLOSURE_DROP.
         *
         *  (b) Drop typeclass -- a capture whose type implements Drop (e.g. an owned
         *      refcounted String): record its Drop instance (and Clone, if any).  A
         *      Drop+Clone type is RETAINed at capture / released at env death
         *      (refcount balances, aliasing-safe -- like the rc walk); a Drop-only
         *      (move-only) type is consumed at capture and released once.
         *
         * The letrec self-capture (env storing its own pointer) is not a transfer
         * and is skipped. */
        {
            const Symbol *drop_name = intern_cstr(e->st, "Drop");
            TypeClass *drop_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, drop_name);
            struct TypeClassInstance **drops = NULL;
            for (uint32_t ci = 0; ci < n_captures; ci++) {
                Binding *cap = arena_captures[ci];
                if (!cap || cap->is_global) continue;
                bool self_cap = cap->closure_fn_binding &&
                                cap->closure_fn_binding == fd->binding;
                if (self_cap) continue;
                /* (a) ^fat owned closure handle -> move (is_fat walk releases it). */
                if (cap->is_fat) {
                    (void)binding_mark_moved(cap, call->span);
                    continue;
                }
                /* rc/weak/ref are already released by the type-kind drop-glue arms;
                 * do not also route them through Drop (would double-release). */
                if (cap->type.kind == TY_RC || cap->type.kind == TY_WEAK ||
                    cap->type.kind == TY_REF || cap->type.kind == TY_LREF)
                    continue;
                /* (b) Drop-implementing capture (e.g. an owned String).  MOVE it:
                 * an opaque Drop type has NO scope-exit auto-drop (release is
                 * manual), so retaining a second owner would leak the source (which
                 * nothing releases).  Moving makes the closure the SOLE owner -- the
                 * drop-glue releases it exactly once via the Drop instance, and a
                 * second capture is a compile-time use-after-move. */
                struct TypeClassInstance *di = drop_tc
                    ? typeclass_env_lookup_instance(&e->typeclass_env, drop_tc, &cap->type, 1)
                    : NULL;
                if (!di) continue;
                if (!drops) {
                    drops = (struct TypeClassInstance **)arena_alloc(
                        e->arena, n_captures * sizeof(struct TypeClassInstance *));
                    for (uint32_t k = 0; k < n_captures; k++) drops[k] = NULL;
                }
                drops[ci] = di;
                (void)binding_mark_moved(cap, call->span);
            }
            closure->capture_drop_insts  = drops;
            closure->capture_clone_insts = NULL;   /* unused: Drop captures MOVE */
        }

        /* Store closure reference in FnDef for codegen */
        fd->closure = closure;
        
        /* Create EX_CLOSURE expression */
        /* CRU Phase 3 / Option B (B-1): a capturing closure is a first-class
         * closure *value* -- a fat box { thunk, env... } -- typed as a boxed
         * TY_FN carrying the lambda's user-facing signature (param_kinds /
         * return_kind, i.e. WITHOUT the prepended env param).  A boxed TY_FN
         * shares TY_PTR_VOID's C carrier (a void* holding the box) and is
         * type_eq-interchangeable with it, so it flows through every legacy
         * :ptr<void> closure sink unchanged; the only new behavior is that a
         * direct call on a value statically typed boxed TY_FN dispatches
         * through the fat protocol for all arities (emit_expr.c).  See
         * docs/upcoming/closure-first-class-type-plan.md. */
        TypeKind *clo_arg_kinds = (TypeKind *)arena_alloc(e->arena, (n_params ? n_params : 1) * sizeof(TypeKind));
        for (uint32_t i = 0; i < n_params; i++) clo_arg_kinds[i] = param_kinds[i];
        Type clo_ty = type_fn(clo_arg_kinds, n_params, return_kind);
        clo_ty.as.fn.boxed = true;
        /* curried-fn-typed-param: preserve the closure's full result type so a
         * closure that *returns a function* keeps the inner (fn ...) type on
         * its first-class value.  Without this, a let-bound closure value
         * applied as ((f 1) 2) recovers a bare result kind for (f 1) and the
         * chained application is rejected as not callable. */
        if (return_full_type) {
            clo_ty.as.fn.result_full_type = return_full_type;
        }
        if (return_fn_type) {
            clo_ty.as.fn.result_full_type = return_fn_type;
        }
        /* closure-capture-escapes-linearity: the closure VALUE is what a `let`
         * binds and what every downstream substructural check inspects, so the
         * inherited obligation has to reach this type -- the thunk types above
         * are internal.  This is the last of the three fn types elab_fn builds
         * on the capturing path; all three are freshly type_fn()'d, so the mark
         * has to be restated at each. */
        if (capture_consumes_linear)      clo_ty.copy_kind = CK_LINEAR;
        else if (capture_consumes_unique) clo_ty.copy_kind = CK_UNIQUE;
        Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, clo_ty, call->span);
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
    /* Per-param scratch arena-sized to the param-vector length (a safe upper
     * bound), not a fixed [MAX_FN_ARITY] stack array -- an extern-c shim may
     * mirror a wide C signature with any number of parameters. */
    uint32_t pcap = params_f->as.list.len ? params_f->as.list.len : 1;
    Binding **params = NULL;
    uint32_t n_params = 0;
    TypeKind *param_kinds = (TypeKind *)arena_alloc(e->arena, pcap * sizeof(TypeKind));
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
        /* No hard parameter-count ceiling: an extern-c shim may mirror a wide
         * C signature with arbitrarily many parameters (arbitrary-fn-arity). */

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
                    params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
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
                params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
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
            params = (Binding **)arena_alloc(e->arena, pcap * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation; optionally skip #{Effect...} advisory row */
    uint32_t ret_idx = 3;
    if (ret_idx < call->as.list.len && call->as.list.items[ret_idx]->tag == F_MAP) {
        /* #{...} effect-row annotation: skip silently (advisory in v1) */
        warn_legacy_fx_row(call->as.list.items[ret_idx]);
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
    for (uint32_t i = 0; i < n_params; i++) {
        if (params[i]->is_fat) FN_ARG_SET(fn_type.as.fn, i, FA_FAT, true);
    }

    /* Create a binding for the extern-c function so it can be looked up and called */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, call->span);
    b->is_extern_c = true;  /* ER6: mark as extern-c for effect inference */
    /* The extern-c C name is produced by the LEGACY fold (raw_name_for_binding /
     * elab_mangle_binding_name special-case is_extern_c), matching the prototype
     * emitted via mangle_field_name. Not c_export_name and never the injective
     * scheme: extern-c names are real C symbols (`tur_hamt_new`, `tvar/new` ->
     * `tvar_new`) that must agree across prototype, call sites, and inline-C. */
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
    for (uint32_t i = 0; i < n_params; i++) {
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

    Form *init_f = NULL;
    Form *type_f = NULL;

    if (name_idx + 3 == call->as.list.len) {
        Form *maybe_ann = call->as.list.items[name_idx + 1];
        if (maybe_ann->tag == F_KEYWORD || maybe_ann->tag == F_TYPE_ANN) {
            type_f = maybe_ann;
            init_f = call->as.list.items[name_idx + 2];
        }
    }

    if (!init_f) {
        if (name_idx + 2 == call->as.list.len) {
            init_f = call->as.list.items[name_idx + 1];
        } else {
            diag_emit(DIAG_ERROR, call->span,
                      "def takes (def [^persistent] [^deprecated [\"msg\"]] name [: type] init)");
            return NULL;
        }
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

    Type *declared_type = NULL;
    if (type_f) {
        declared_type = type_expr_from_form(e, type_f, NULL, NULL, NULL, 0);
        if (!declared_type) return NULL;
    }

    Type *saved_expected = e->expected_type;
    if (declared_type) e->expected_type = declared_type;
    Expr *init = elab_form(e, init_f);
    e->expected_type = saved_expected;
    if (!init) return NULL;

    if (declared_type) {
        /* Align with elab_ascribe */
        if (declared_type->kind == TY_APP && init->kind == EX_CALL &&
            init->as.call_.ctor &&
            (init->type.kind == TY_ADT || init->type.kind == TY_APP)) {
            AdtDef *asc_def = type_adt_app_def(declared_type);
            AdtDef *inner_def = (init->type.kind == TY_ADT)
                ? init->type.as.adt_.def : type_adt_app_def(&init->type);
            if (asc_def && asc_def == inner_def)
                init->type = *declared_type;
        }

        TypeKind src_kind = init->type.kind;
        TypeKind dst_kind = declared_type->kind;
        if (src_kind != dst_kind) {
            int src_size = type_size_bytes(src_kind);
            int dst_size = type_size_bytes(dst_kind);
            if (src_size > 0 && dst_size > 0 && src_size == dst_size) {
                Expr *r = expr_new(e->arena, EX_REINTERPRET, *declared_type, call->span);
                r->as.reinterpret_.expr = init;
                r->as.reinterpret_.source_kind = src_kind;
                r->as.reinterpret_.target_kind = dst_kind;
                init = r;
            }
        }

        Expr *asc = expr_new(e->arena, EX_ASCRIBE, *declared_type, call->span);
        asc->as.ascribe_.inner = init;

        if (declared_type->kind == TY_FN &&
            (src_kind == TY_INT || src_kind == TY_PTR_VOID)) {
            asc->type.as.fn.boxed = true;
        }
        init = asc;
    }

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
    return out;
}
