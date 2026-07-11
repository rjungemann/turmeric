/* emit_cps.c -- CPS-substrate codegen wiring (cps-transform-plan).
 *
 * Re-points the base delimited-control lowerings (reset / shift / shift0) off
 * the bespoke inlined runtime and onto the heap-reified CPS substrate's
 * multi-prompt delimited-control machine (`DK`, src/runtime/cps_prompt.{h,c}),
 * emitted directly into the generated program. See emit_cps.h for the model.
 *
 * Turmeric's shift is abortive: `(shift f v)` applies the receiver `f` to the
 * body value `v` and delivers the result to the nearest enclosing `reset`,
 * discarding the captured context (the sub-continuation is never handed back
 * to user code). The DK machine expresses exactly this: a `reset` is a prompt,
 * a `shift` is a DK shift node whose body returns the abort value, and dk_run
 * delivers it through the prompt. Because the abort value `f(v)` can be
 * precomputed in-scope, the shift node needs no per-frame environment capture
 * codegen -- the precomputed value rides in the node's `body_env`.
 *
 * Anything outside the supported subset returns NULL from emit_cps_reset, so
 * the caller falls back to the legacy lowering and that code stays
 * byte-identical.
 */

#include "emit_cps.h"

#include <stdlib.h>
#include <string.h>

#include "emit_internal.h"
#include "builtins.h"
#include "typeclass.h"
#include "types.h"

/* ---- program scan: does it use base delimited control? ---------------- */

static bool uses_base_delimited(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
            return true;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_base_delimited(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_base_delimited(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_base_delimited(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_base_delimited(e->as.closure_.closure->fn->body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_base_delimited(e->as.let_.bindings[i].init)) return true;
            return uses_base_delimited(e->as.let_.body);
        case EX_IF:
            return uses_base_delimited(e->as.if_.cond) ||
                   uses_base_delimited(e->as.if_.then_) ||
                   uses_base_delimited(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_base_delimited(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_base_delimited(e->as.while_.cond) ||
                   uses_base_delimited(e->as.while_.body);
        case EX_SET:    return uses_base_delimited(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_base_delimited(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_base_delimited(e->as.return_.value);
        case EX_DEFER:  return uses_base_delimited(e->as.defer_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_base_delimited(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_base_delimited(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

bool emit_cps_program_uses_delimited(const Expr *program) {
    return uses_base_delimited(program);
}

/* ---- program scan: does it use (call/cc f) / (escape f)? --------------- */

static bool uses_callcc(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALLCC:
            return true;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_callcc(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_callcc(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_callcc(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_callcc(e->as.closure_.closure->fn->body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_callcc(e->as.let_.bindings[i].init)) return true;
            return uses_callcc(e->as.let_.body);
        case EX_IF:
            return uses_callcc(e->as.if_.cond) ||
                   uses_callcc(e->as.if_.then_) ||
                   uses_callcc(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_callcc(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_callcc(e->as.while_.cond) || uses_callcc(e->as.while_.body);
        case EX_SET:    return uses_callcc(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_callcc(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_callcc(e->as.return_.value);
        case EX_DEFER:  return uses_callcc(e->as.defer_.body);
        case EX_RESET:  return uses_callcc(e->as.reset_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_callcc(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_callcc(e->as.call_.args[i])) return true;
            return uses_callcc(e->as.call_.fn_expr);
        /* Control forms and value wrappers that can nest a (call/cc f)/(escape f)
         * -- omitting any of these left the escape-continuation prelude ungated
         * (an "unknown type name 'tur_escape_cont'" build error) when an escape
         * hid inside a shift body, handler case, match arm, etc.  Mirrors the
         * complete expr walk (expr_collect_effects). */
        case EX_MATCH:
            if (uses_callcc(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                if (uses_callcc(e->as.match_.arms[i].guard) ||
                    uses_callcc(e->as.match_.arms[i].body)) return true;
            return false;
        case EX_SHIFT:  return uses_callcc(e->as.shift_.k_fn)  || uses_callcc(e->as.shift_.body);
        case EX_SHIFT0: return uses_callcc(e->as.shift0_.k_fn) || uses_callcc(e->as.shift0_.body);
        case EX_CLONEABLE_RESET: return uses_callcc(e->as.cloneable_reset_.body);
        case EX_CLONEABLE_SHIFT: return uses_callcc(e->as.cloneable_shift_.k_fn) ||
                                        uses_callcc(e->as.cloneable_shift_.body);
        case EX_SERIAL_RESET:    return uses_callcc(e->as.serial_reset_.body);
        case EX_SERIAL_SHIFT:    return uses_callcc(e->as.serial_shift_.k_fn) ||
                                        uses_callcc(e->as.serial_shift_.body);
        case EX_PERFORM:
            if (e->as.perform_.perform)
                for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                    if (uses_callcc(e->as.perform_.perform->args[i])) return true;
            return false;
        case EX_HANDLE:
        case EX_HANDLER_LIT:
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                if (uses_callcc(h->body)) return true;
                for (uint8_t i = 0; i < h->n_cases; i++)
                    if (uses_callcc(h->cases[i].body)) return true;
            }
            return false;
        case EX_RESUME:
            return e->as.resume_.resume &&
                   (uses_callcc(e->as.resume_.resume->k) || uses_callcc(e->as.resume_.resume->value));
        case EX_DISCONTINUE:
            return e->as.discontinue_.discontinue &&
                   (uses_callcc(e->as.discontinue_.discontinue->k) ||
                    uses_callcc(e->as.discontinue_.discontinue->exception));
        case EX_WITH_HANDLER:  return uses_callcc(e->as.with_handler_.handler) ||
                                      uses_callcc(e->as.with_handler_.body);
        case EX_ASYNC:  return uses_callcc(e->as.async_.fn_expr);
        case EX_AWAIT:  return uses_callcc(e->as.await_.fut_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (uses_callcc(e->as.make_struct_.field_values[i])) return true;
            return false;
        case EX_GET_FIELD: return uses_callcc(e->as.get_field_.struct_expr);
        case EX_SET_FIELD: return uses_callcc(e->as.set_field_.receiver) ||
                                  uses_callcc(e->as.set_field_.value);
        case EX_REF:          return uses_callcc(e->as.ref_.expr);
        case EX_DEREF:        return uses_callcc(e->as.deref_.expr);
        case EX_BORROW_IMMUT: return uses_callcc(e->as.borrow_immut_.expr);
        case EX_BORROW_MUT:   return uses_callcc(e->as.borrow_mut_.expr);
        case EX_ASCRIBE:      return uses_callcc(e->as.ascribe_.inner);
        case EX_REINTERPRET:  return uses_callcc(e->as.reinterpret_.expr);
        case EX_CAST:         return uses_callcc(e->as.cast_.expr);
        case EX_FN_TO_FAT:    return uses_callcc(e->as.fn_to_fat_.inner);
        case EX_POLY_TO_FAT:  return uses_callcc(e->as.poly_to_fat_.inner);
        case EX_POLY_WRAP:    return uses_callcc(e->as.poly_wrap_.inner);
        default:
            return false;
    }
}

bool emit_cps_program_uses_callcc(const Expr *program) {
    return uses_callcc(program);
}

/* ---- the abort value: f(v) must round-trip through intptr_t -------------
 * The DK node carries the precomputed abort value in its intptr_t body_env, so
 * the value must survive an intptr_t round trip. Integers, booleans, nil and
 * pointers (cstr) do; floats do not (a (intptr_t) cast truncates). */
static bool ty_intptr_safe(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_NIL: case TY_CSTR:
            return true;
        default:
            return false;
    }
}

/* A let binding emitted ahead of the shift becomes a plain C local; restrict to
 * scalar types whose C representation is unambiguous (no carrier-ABI/struct/RC
 * subtleties). */
static bool ty_simple_local(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_NIL: case TY_CSTR: case TY_FLOAT:
            return true;
        default:
            return false;
    }
}





/* The DK runtime prelude emitters (emit_cps_callcc_prelude,
 * emit_cps_cloneable_bridge_prelude, emit_cps_serial_runtime_prelude,
 * emit_cps_runtime_prelude) were relocated to emit_dk_runtime.c
 * (cps-backend-unification U7, step 1). This file now holds only the
 * direct-style lowering functions and their private analysis helpers. */

/* =========================================================================
 * CPS9 (cps-transform-plan): cloneable continuations on the DK machine.
 *
 * The headline "next increment" called out in CPS8: re-point cloneable
 * continuations (cloneable-reset / cloneable-shift, the substrate behind
 * call/cc*) onto the multi-prompt DK machine so a captured continuation
 * actually reifies the *delimited context* (the frames between the shift and
 * its reset) and replays it -- multi-shot -- via dk_invoke.
 *
 * Before this, the emitted cloneable continuation was trivial: the captured
 * "rest of the computation" was a no-op `return __value`, so a context like
 * (cloneable-reset (+ 10 (cloneable-shift k ...))) lost the `+ 10` on resume
 * (and, with the shift nested inside an operand, did not even compile). Now
 * the context is lambda-lifted into DK frames; the cloneable continuation
 * stores the reified sub-continuation chain as its env, with a DK-invoking
 * cont_fn -- reusing the existing tur_cloneable_cont machinery wholesale
 * (resume/clone/drop are unchanged; multi-shot is dk_invoke's internal copy).
 *
 * Supported context subset (anything else falls back to the legacy lowering,
 * keeping its emitted C byte-identical): a non-empty chain of single-hole
 * integer binary ops (+, -, *) wrapping exactly one cloneable-shift whose
 * receiver has no live captures, with an intptr-safe (int) result. The
 * non-hole operands are pure int expressions, evaluated once at capture time
 * and reused on every resume (the standard delimited-control semantics).
 * ========================================================================= */

#define CL_MAX_CTX_FRAMES 32

typedef struct {
    const char    *c_op;     /* builtin frame: "+","-","*","/"; NULL for a call frame */
    const Binding *call_fn;  /* call frame: target top-level fn binding; else NULL */
    bool           hole_left;/* true iff the hole is the left operand/arg */
    /* ignore_value: a continuation frame that does not consume the resume value
     * (a do-sequence tail). A 0-arg such frame calls target(); a 1-arg one calls
     * target(env) with the captured argument. Hole-carrying frames are false. */
    bool           ignore_value;
    const Expr    *other;    /* the non-hole operand (captured into the frame env) */
    TypeKind       env_kind; /* type kind of `other` -- for typed serial marshaling */
    /* cps-transform-plan (a): when the env is a nominal type with a Serializable
     * instance (not the inline int/cstr kinds), the instance's method C names;
     * NULL for inline-marshaled envs. */
    const char    *env_ser;
    const char    *env_deser;
} ClFrame;

/* cps-transform-plan (grammar extension): a pure `let` binding the context walk
 * descended through. Its init does not reach the shift, so it is emitted once at
 * the reset site as a plain C local ahead of the captured-operand evaluation;
 * the context frames' non-hole operands may then reference it by name. */
#define CL_MAX_CTX_LETS 16
typedef struct {
    const Binding *binding;  /* the let-bound variable */
    const Expr    *init;     /* its pure initializer */
} CtxLet;

/* The nominal name of a record / opaque TY_ADT type, or NULL otherwise.  Every
 * nominal carrier (a lowered `defstruct`, a `defdata`, and an opaque `defopaque`
 * newtype) is a TY_ADT after the structdef-retirement migration, so serial env
 * marshaling only has to recognise TY_ADT.  (DS-C: the former TY_STRUCT arm is
 * dead -- no TY_STRUCT value is ever constructed.) */
static const char *sk_nominal_type_name(Type t) {
    if (t.kind == TY_ADT && t.as.adt_.def) return t.as.adt_.def->name;
    return NULL;
}

/* cps-transform-plan (a): find a Serializable instance for `t` and return its
 * serialize/deserialize method C names. Restricted to nominal (TY_STRUCT or
 * TY_ADT, incl. opaque) types -- primitive int/cstr envs use the inline codec.
 * Scans the program's instance definitions (codegen has no typeclass env). */
static bool sk_find_serializable(const Expr *program, Type t,
                                 const char **ser_out, const char **deser_out) {
    if (!program || program->kind != EX_PROGRAM) return false;
    const char *tname = sk_nominal_type_name(t);
    if (!tname) return false;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_INSTANCE_DEF) continue;
        const TypeClassInstance *inst = it->as.instance_def_.instance;
        if (!inst || !inst->typeclass || !inst->typeclass->name) continue;
        if (strcmp(inst->typeclass->name->name, "Serializable") != 0) continue;
        if (inst->n_type_args < 1) continue;
        const char *atname = sk_nominal_type_name(inst->type_args[0]);
        if (!atname) continue;
        if (strcmp(atname, tname) != 0) continue;
        /* Found the matching instance. When the caller only wants existence
         * (NULL out-params, e.g. a feasibility check), don't allocate names. */
        if (!ser_out || !deser_out) return true;
        const char *ser = NULL, *deser = NULL;
        const TypeClass *tc = inst->typeclass;
        for (uint8_t j = 0; j < tc->n_methods && j < inst->n_method_impls; j++) {
            if (!inst->method_impls[j] || !inst->method_impls[j]->binding) continue;
            const char *mn = tc->methods[j].name ? tc->methods[j].name->name : "";
            char *cn = raw_name_for_binding(inst->method_impls[j]->binding);
            if (strcmp(mn, "serialize") == 0) ser = cn;
            else if (strcmp(mn, "deserialize") == 0) deser = cn;
            else free(cn);
        }
        if (ser && deser) { *ser_out = ser; *deser_out = deser; return true; }
        free((void *)ser); free((void *)deser);
        return false;
    }
    return false;
}


/* Boundary-aware: can `e` dynamically reach a shift of kind `target` bound to
 * the enclosing reset? Stops at nested resets/shifts of any other flavor and at
 * fn boundaries (those self-delimit or are not our shift). Shared by the
 * cloneable (CPS9) and serial (CPS10) context walks. */
static bool reaches_shift_kind(const Expr *e, ExprKind target) {
    if (!e) return false;
    if (e->kind == target) return true;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_SHIFT:
        case EX_CLONEABLE_RESET:
        case EX_RESET:
        case EX_SERIAL_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_CALLCC:
        case EX_FN:
        case EX_FN_DEF:
        case EX_CLOSURE:
            return false;   /* target already matched above; these self-delimit */
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (reaches_shift_kind(e->as.builtin.args[i], target)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (reaches_shift_kind(e->as.call_.args[i], target)) return true;
            return false;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (reaches_shift_kind(e->as.let_.bindings[i].init, target)) return true;
            return reaches_shift_kind(e->as.let_.body, target);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (reaches_shift_kind(e->as.do_.items[i], target)) return true;
            return false;
        case EX_IF:
            return reaches_shift_kind(e->as.if_.cond, target) ||
                   reaches_shift_kind(e->as.if_.then_, target) ||
                   reaches_shift_kind(e->as.if_.else_or_null, target);
        case EX_RETURN:
            return reaches_shift_kind(e->as.return_.value, target);
        case EX_SET:
            return reaches_shift_kind(e->as.set_.value, target);
        default:
            return false;
    }
}

static bool cl_op_supported(const char *op) {
    return op && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                  strcmp(op, "*") == 0 || strcmp(op, "/") == 0);
}


/* Walk the reset body down to its single shift of kind `target`, recording each
 * enclosing single-hole int binop as a context frame (frames[0] = outermost).
 * Returns the shift expr and *n_out frames (>= 0), or NULL if the body is not a
 * supported context chain. Shared by the cloneable (CPS9) and serial (CPS10)
 * lowerings. */
/* Env/result kinds a context frame can carry: int and cstr (both fit an
 * intptr_t; the serial marshaler encodes each by kind -- int inline, cstr
 * length-prefixed). With value-typed cont<T> the surface can now express a
 * cstr-valued continuation, so cstr contexts (e.g. (cat "hi " [])) type-check. */
static bool env_kind_ok(TypeKind k) { return k == TY_INT || k == TY_CSTR; }

static const Expr *collect_ctx(const Expr *rb, ExprKind target,
                               ClFrame *frames, uint32_t *n_out,
                               const Expr *program, bool want_names,
                               CtxLet *lets, uint32_t *n_lets_out) {
    uint32_t n = 0;
    uint32_t nl = 0;
    const Expr *cur = rb;
    for (;;) {
        if (n >= CL_MAX_CTX_FRAMES) return NULL;
        if (cur && cur->kind == EX_LET) {
            /* A pure `let` in the context: each init must not reach the shift
             * (it is prelude, evaluated once at capture time) and must be a
             * simple scalar local; the body carries the hole. The bindings are
             * recorded so the emit site can lay them down as C locals ahead of
             * the captured-operand evaluation. */
            if (!lets || !n_lets_out) return NULL;
            const Expr *body = cur->as.let_.body;
            if (!reaches_shift_kind(body, target)) return NULL;
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                const Expr *init = cur->as.let_.bindings[i].init;
                const Binding *b = cur->as.let_.bindings[i].binding;
                if (reaches_shift_kind(init, target)) return NULL;
                if (!b || !ty_simple_local(b->type.kind)) return NULL;
                if (expr_contains_return_or_throw(init)) return NULL;
                if (nl >= CL_MAX_CTX_LETS) return NULL;
                lets[nl].binding = b;
                lets[nl].init = init;
                nl++;
            }
            cur = body;
            continue;
        }
        if (cur && cur->kind == EX_DO) {
            /* do-sequence with a statement-position shift:
             *   (do PRELUDE... (shift k v) TAIL)
             * The items before the shift are side-effecting prelude, evaluated
             * once at capture time (recorded as binding-less prelude entries).
             * The single optional tail item is the captured continuation: an
             * ignore-value frame that, on resume, runs regardless of the resume
             * value. This is the "skip expensive-init, resume into the loop"
             * shape the application-image-dumps plan targets. */
            if (!lets || !n_lets_out) return NULL;
            uint32_t N = cur->as.do_.n;
            int32_t m = -1;
            for (uint32_t i = 0; i < N; i++) {
                if (reaches_shift_kind(cur->as.do_.items[i], target)) {
                    if (m >= 0) return NULL;   /* at most one hole */
                    m = (int32_t)i;
                }
            }
            if (m < 0) return NULL;
            /* The shift must sit in statement position (be the do item itself).
             * The tail items (m, N) are the captured continuation. */
            if (cur->as.do_.items[m]->kind != target) return NULL;
            /* Prelude items [0, m): emitted for side effect at the reset site. */
            for (int32_t i = 0; i < m; i++) {
                if (nl >= CL_MAX_CTX_LETS) return NULL;
                if (expr_contains_return_or_throw(cur->as.do_.items[i])) return NULL;
                lets[nl].binding = NULL;                 /* side-effect prelude */
                lets[nl].init    = cur->as.do_.items[i];
                nl++;
            }
            /* Tail items (m, N): each a 0-arg top-level call -> an ignore-value
             * frame. They run in source order on resume, so record them in
             * reverse: the first tail item is innermost (adjacent to the shift,
             * runs first), the last is outermost (runs last, its value is the
             * reset's value). */
            for (int32_t i = (int32_t)N - 1; i > m; i--) {
                const Expr *tail = cur->as.do_.items[i];
                if (tail->kind != EX_CALL ||
                    !tail->as.call_.fn_binding || tail->as.call_.fn_expr) return NULL;
                const Binding *fb = tail->as.call_.fn_binding;
                if (fb->type.kind != TY_FN) return NULL;
                if (fb->closure_fn_binding) return NULL;
                /* The outermost tail item yields the reset's value (scalar);
                 * inner ones are run for effect and their value discarded. */
                if (i == (int32_t)N - 1 && !env_kind_ok(tail->type.kind)) return NULL;
                if (n >= CL_MAX_CTX_FRAMES) return NULL;
                if (tail->as.call_.n_args == 0 && fb->type.as.fn.arity == 0) {
                    /* 0-arg tail call -> ignore-value frame, no env. */
                    frames[n].c_op = NULL;
                    frames[n].call_fn = fb;
                    frames[n].hole_left = true;   /* unused for a 0-arg frame */
                    frames[n].ignore_value = true;
                    frames[n].other = NULL;       /* no env */
                    frames[n].env_kind = TY_INT;  /* inline zero env */
                    frames[n].env_ser = NULL;
                    frames[n].env_deser = NULL;
                    n++;
                } else if (tail->as.call_.n_args == 1 && fb->type.as.fn.arity == 1) {
                    /* 1-arg tail call (f env) -> ignore-value frame whose single
                     * argument is a pure captured value (no hole). The arg is
                     * marshaled by kind, so the loop can take scalar config. */
                    const Expr *arg = tail->as.call_.args[0];
                    if (reaches_shift_kind(arg, target)) return NULL;  /* must be pure env */
                    if (expr_contains_return_or_throw(arg)) return NULL;
                    const char *eser = NULL, *edeser = NULL;
                    if (env_kind_ok(arg->type.kind)) {
                        /* int/cstr env -- inline codec */
                    } else if (program &&
                               sk_find_serializable(program, arg->type,
                                                    want_names ? &eser : NULL,
                                                    want_names ? &edeser : NULL)) {
                        /* nominal env with a Serializable instance */
                    } else {
                        return NULL;
                    }
                    frames[n].c_op = NULL;
                    frames[n].call_fn = fb;
                    frames[n].hole_left = true;   /* env occupies the sole arg slot */
                    frames[n].ignore_value = true;
                    frames[n].other = arg;
                    frames[n].env_kind = arg->type.kind;
                    frames[n].env_ser = eser;
                    frames[n].env_deser = edeser;
                    n++;
                } else {
                    return NULL;   /* richer tail calls need continuation lifting */
                }
            }
            cur = cur->as.do_.items[m];   /* the shift; loop exits below */
            continue;
        }
        if (cur && cur->kind == EX_BUILTIN) {
            /* Arithmetic frame: a single-hole int binop (+, -, *, /). */
            const BuiltinSpec *spec = cur->as.builtin.spec;
            if (!spec || cur->as.builtin.n != 2) return NULL;
            if (!cl_op_supported(spec->c_op)) return NULL;
            if (cur->type.kind != TY_INT) return NULL;
            const Expr *a0 = cur->as.builtin.args[0];
            const Expr *a1 = cur->as.builtin.args[1];
            bool h0 = reaches_shift_kind(a0, target);
            bool h1 = reaches_shift_kind(a1, target);
            if (h0 == h1) return NULL;             /* need exactly one hole */
            const Expr *other = h0 ? a1 : a0;
            if (other->type.kind != TY_INT) return NULL;
            if (reaches_shift_kind(other, target)) return NULL;
            if (expr_contains_return_or_throw(other)) return NULL;
            frames[n].c_op = spec->c_op;
            frames[n].call_fn = NULL;
            frames[n].hole_left = h0;
            frames[n].ignore_value = false;
            frames[n].other = other;
            frames[n].env_kind = TY_INT;
            frames[n].env_ser = NULL;
            frames[n].env_deser = NULL;
            n++;
            cur = h0 ? a0 : a1;
        } else if (cur && cur->kind == EX_CALL && cur->as.call_.n_args == 2 &&
                   cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            /* Call frame: a 2-arg call to a resolved top-level function with one
             * hole arg and one pure env arg. The env may be int/cstr (inline) or
             * -- when `program` is supplied (serial path) -- a nominal type with
             * a Serializable instance (marshaled via that instance, CPS step a). */
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 2) return NULL;
            if (fb->closure_fn_binding) return NULL;   /* needs hidden env arg */
            if (!env_kind_ok(cur->type.kind)) return NULL;   /* result: scalar */
            const Expr *a0 = cur->as.call_.args[0];
            const Expr *a1 = cur->as.call_.args[1];
            bool h0 = reaches_shift_kind(a0, target);
            bool h1 = reaches_shift_kind(a1, target);
            if (h0 == h1) return NULL;
            /* the hole slot's param must be a carrier scalar (the resume value) */
            TypeKind hole_param = h0 ? fb->type.as.fn.arg_kinds[0]
                                     : fb->type.as.fn.arg_kinds[1];
            if (!env_kind_ok(hole_param)) return NULL;
            const Expr *other = h0 ? a1 : a0;   /* the env operand */
            if (reaches_shift_kind(other, target)) return NULL;
            if (expr_contains_return_or_throw(other)) return NULL;
            const char *eser = NULL, *edeser = NULL;
            if (env_kind_ok(other->type.kind)) {
                /* int/cstr env -- inline codec */
            } else if (program &&
                       sk_find_serializable(program, other->type,
                                            want_names ? &eser : NULL,
                                            want_names ? &edeser : NULL)) {
                /* nominal env with a Serializable instance -- instance codec */
            } else {
                return NULL;
            }
            frames[n].c_op = NULL;
            frames[n].call_fn = fb;
            frames[n].hole_left = h0;
            frames[n].ignore_value = false;
            frames[n].other = other;
            frames[n].env_kind = other->type.kind;
            frames[n].env_ser = eser;
            frames[n].env_deser = edeser;
            n++;
            cur = h0 ? a0 : a1;
        } else if (cur && cur->kind == EX_CALL && cur->as.call_.n_args == 1 &&
                   cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            /* 1-arg call frame: (f HOLE) -- the sole argument is the hole, so
             * there is no captured env. The frame applies f to the resume
             * value; it marshals as the target name with a zero (unused) env. */
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 1) return NULL;
            if (fb->closure_fn_binding) return NULL;   /* needs hidden env arg */
            if (!env_kind_ok(cur->type.kind)) return NULL;   /* result: scalar */
            const Expr *a0 = cur->as.call_.args[0];
            if (!reaches_shift_kind(a0, target)) return NULL;
            if (!env_kind_ok(fb->type.as.fn.arg_kinds[0])) return NULL;
            frames[n].c_op = NULL;
            frames[n].call_fn = fb;
            frames[n].hole_left = true;   /* sole arg carries the hole */
            frames[n].ignore_value = false;
            frames[n].other = NULL;       /* no env */
            frames[n].env_kind = TY_INT;  /* marshaled as an inline zero env */
            frames[n].env_ser = NULL;
            frames[n].env_deser = NULL;
            n++;
            cur = a0;
        } else {
            break;
        }
    }
    if (cur && cur->kind == target) {
        *n_out = n;
        if (n_lets_out) *n_lets_out = nl;
        return cur;
    }
    return NULL;
}

/* cps-transform-plan (grammar extension): the one runtime branch point the
 * resumable lowering supports.  If `body` is an `(if cond THEN ELSE)` whose
 * condition is pure (does not reach the shift) and whose arms split cleanly
 * into exactly one shift-bearing arm and one pure arm, return the shift-bearing
 * arm (the context body to walk) and set *cond_out / *else_out (the pure arm) /
 * *when_out (true iff the shift arm is the `then` arm, so the emitted C test is
 * `if (cond)` vs `if (!(cond))`).  Otherwise return NULL.  The condition is
 * evaluated once at the reset site; the shift path runs the DK chain built from
 * the shift-bearing arm, the other path yields the pure arm's value directly --
 * the frame-chain model cannot express a shift reached only on one runtime
 * branch any other way. */
static const Expr *ctx_if_branch(const Expr *body, ExprKind shift_kind,
                                 const Expr **cond_out, const Expr **else_out,
                                 bool *when_out) {
    if (!body || body->kind != EX_IF) return NULL;
    const Expr *cond = body->as.if_.cond;
    const Expr *thn  = body->as.if_.then_;
    const Expr *els  = body->as.if_.else_or_null;
    if (!cond || !thn || !els) return NULL;            /* need both arms */
    if (reaches_shift_kind(cond, shift_kind)) return NULL;
    if (expr_contains_return_or_throw(cond)) return NULL;
    bool ht = reaches_shift_kind(thn, shift_kind);
    bool he = reaches_shift_kind(els, shift_kind);
    if (ht == he) return NULL;                         /* exactly one shift arm */
    const Expr *shift_arm = ht ? thn : els;
    const Expr *pure_arm  = ht ? els : thn;
    if (reaches_shift_kind(pure_arm, shift_kind)) return NULL;   /* defensive */
    if (expr_contains_return_or_throw(pure_arm)) return NULL;
    *cond_out = cond;
    *else_out = pure_arm;
    *when_out = ht;
    return shift_arm;
}

/* cps-transform-plan (grammar extension, last shape): an `if` branch point
 * reached *through* outer context frames, e.g. (reset (+ 5 (if c THEN[shift]
 * ELSE))). ctx_if_branch only matches an `if` that is the whole reset body; the
 * helpers below locate an `if` sitting *under* a chain of context frames and let
 * the lowering split it into a shift-bearing context body and a pure-arm context
 * body, each still a flat frame chain the existing emitters handle. */

/* The unique index of the arg in `args[0..n)` that reaches a shift of kind
 * `target`, or -1 if zero or more than one arg reaches it (so the descent is
 * unambiguous; collect_ctx enforces the "exactly one hole" rule later). */
static int shift_child_index(Expr *const *args, uint32_t n, ExprKind target) {
    int found = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (reaches_shift_kind(args[i], target)) {
            if (found >= 0) return -1;   /* ambiguous */
            found = (int)i;
        }
    }
    return found;
}

/* Walk down the context spine (single-hole binops, 2-arg calls, pure `let`
 * bodies) from `cur` following the unique shift-reaching child. Return the first
 * enclosing `if` reached this way, or NULL if the spine reaches the shift itself
 * (a flat context, no branch point) or hits an unsupported node. The frame
 * validity of the spine (op support, operand purity/type) is *not* checked here;
 * the caller defers that to collect_ctx run over the substituted shift body. */
static const Expr *find_ctx_if(const Expr *cur, ExprKind target) {
    for (int guard = 0; cur && guard < 4096; guard++) {
        if (cur->kind == EX_IF)   return cur;     /* the branch point */
        if (cur->kind == target)  return NULL;    /* flat: shift, no `if` */
        const Expr *next = NULL;
        if (cur->kind == EX_BUILTIN && cur->as.builtin.n == 2) {
            int h = shift_child_index(cur->as.builtin.args, 2, target);
            if (h < 0) return NULL;
            next = cur->as.builtin.args[h];
        } else if (cur->kind == EX_CALL && cur->as.call_.n_args == 2 &&
                   cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            int h = shift_child_index(cur->as.call_.args, 2, target);
            if (h < 0) return NULL;
            next = cur->as.call_.args[h];
        } else if (cur->kind == EX_LET || cur->kind == EX_LETREC) {
            if (!reaches_shift_kind(cur->as.let_.body, target)) return NULL;
            next = cur->as.let_.body;
        } else {
            return NULL;
        }
        cur = next;
    }
    return NULL;
}

/* A small pool of compile-time-only allocations (spine clones + their cloned
 * arg arrays) freed in bulk once the substituted bodies have been emitted. */
typedef struct { void *items[128]; uint32_t n; } ClonePool;
static void *pool_take(ClonePool *p, size_t sz) {
    void *m = malloc(sz);
    if (p->n < 128) p->items[p->n++] = m;   /* spine is tiny; cap defensively */
    return m;
}
static void pool_free(ClonePool *p) {
    for (uint32_t i = 0; i < p->n; i++) free(p->items[i]);
    p->n = 0;
}

/* Return a copy of `orig` in which the spine node `if_node` (found by
 * find_ctx_if) is replaced by `replacement`, following the same unique
 * shift-reaching child path. Only the spine nodes (and the binop/call arg arrays
 * carrying the hole) are freshly allocated -- every off-spine sub-expression is
 * shared with `orig`. The clones live until pool_free. */
static const Expr *clone_spine(const Expr *orig, const Expr *if_node,
                               const Expr *replacement, ExprKind target,
                               ClonePool *pool) {
    if (orig == if_node) return replacement;
    Expr *c = pool_take(pool, sizeof(Expr));
    *c = *orig;
    if (orig->kind == EX_BUILTIN && orig->as.builtin.n == 2) {
        uint32_t n = orig->as.builtin.n;
        int h = shift_child_index(orig->as.builtin.args, n, target);
        Expr **na = pool_take(pool, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) na[i] = orig->as.builtin.args[i];
        na[h] = (Expr *)clone_spine(orig->as.builtin.args[h], if_node,
                                    replacement, target, pool);
        c->as.builtin.args = na;
    } else if (orig->kind == EX_CALL && orig->as.call_.n_args == 2) {
        uint32_t n = orig->as.call_.n_args;
        int h = shift_child_index(orig->as.call_.args, n, target);
        Expr **na = pool_take(pool, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) na[i] = orig->as.call_.args[i];
        na[h] = (Expr *)clone_spine(orig->as.call_.args[h], if_node,
                                    replacement, target, pool);
        c->as.call_.args = na;
    } else if (orig->kind == EX_LET || orig->kind == EX_LETREC) {
        c->as.let_.body = (Expr *)clone_spine(orig->as.let_.body, if_node,
                                              replacement, target, pool);
    }
    return c;
}

/* Pure feasibility check (NO emission): is this cloneable-reset lowerable onto
 * the DK machine? Requires a non-empty supported context (so existing
 * empty-context fixtures stay on the legacy path, byte-identical) -- except for
 * the if-shaped body, which never matched the legacy path, so an empty context
 * in the shift arm is admitted there. The `if` may be the whole body or sit
 * beneath outer context frames (the last grammar shape). */
static bool cl_can_lower(const Expr *e) {
    if (!e || e->kind != EX_CLONEABLE_RESET) return false;
    if (!ty_intptr_safe(e->type.kind)) return false;
    const Expr *inner = e->as.cloneable_reset_.body;
    const Expr *if_node = find_ctx_if(inner, EX_CLONEABLE_SHIFT);
    bool has_if = (if_node != NULL);

    /* The context body to walk down to the shift: the whole reset body for a
     * flat context, or -- when there is an `if` branch point (at the root or
     * beneath outer frames) -- the reset body with the `if` replaced by its
     * shift-bearing arm, so the outer frames + the shift arm form one flat
     * chain. */
    const Expr *shift_body = inner;
    ClonePool pool = {0};
    if (has_if) {
        const Expr *cond = NULL, *els = NULL; bool when = true;
        const Expr *branch = ctx_if_branch(if_node, EX_CLONEABLE_SHIFT,
                                           &cond, &els, &when);
        if (!branch) return false;
        shift_body = clone_spine(inner, if_node, branch, EX_CLONEABLE_SHIFT, &pool);
    }

    ClFrame frames[CL_MAX_CTX_FRAMES];
    CtxLet lets[CL_MAX_CTX_LETS];
    uint32_t nf = 0, nl = 0;
    const Expr *shift = collect_ctx(shift_body, EX_CLONEABLE_SHIFT, frames, &nf,
                                    NULL, false, lets, &nl);
    bool ok = shift &&
              (has_if || nf != 0) &&     /* empty flat context -> legacy */
              shift->as.cloneable_shift_.n_live_captures == 0 &&
              shift->as.cloneable_shift_.k_fn != NULL;
    pool_free(&pool);
    return ok;
}





/* Program scan: does any cloneable-reset lower onto the DK machine? Gates the
 * DK runtime prelude + the cloneable<->DK bridge. Mirrors cl_can_lower. */
static bool uses_cloneable_dk(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CLONEABLE_RESET && cl_can_lower(e)) return true;
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_cloneable_dk(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_cloneable_dk(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_cloneable_dk(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_cloneable_dk(e->as.closure_.closure->fn->body);
        case EX_CLONEABLE_RESET:
            return uses_cloneable_dk(e->as.cloneable_reset_.body);
        case EX_RESET:
            return uses_cloneable_dk(e->as.reset_.body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_cloneable_dk(e->as.let_.bindings[i].init)) return true;
            return uses_cloneable_dk(e->as.let_.body);
        case EX_IF:
            return uses_cloneable_dk(e->as.if_.cond) ||
                   uses_cloneable_dk(e->as.if_.then_) ||
                   uses_cloneable_dk(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_cloneable_dk(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_cloneable_dk(e->as.while_.cond) ||
                   uses_cloneable_dk(e->as.while_.body);
        case EX_SET:    return uses_cloneable_dk(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_cloneable_dk(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_cloneable_dk(e->as.return_.value);
        case EX_DEFER:  return uses_cloneable_dk(e->as.defer_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_cloneable_dk(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_cloneable_dk(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

bool emit_cps_program_uses_cloneable_dk(const Expr *program) {
    return uses_cloneable_dk(program);
}

/* =========================================================================
 * CPS10 (cps-transform-plan, CPS5.4): serializable continuations on the DK
 * machine.
 *
 * serial-shift captures the delimited context as a DK chain, exactly like the
 * cloneable path (CPS9) -- but the captured continuation is *marshalable*. A
 * heap-reified sub-continuation is a flat list of (frame, env) nodes, so it can
 * be serialized by writing each frame's stable TAG plus its env, and rebuilt by
 * mapping the tag back to a frame function -- no fiber/stack snapshot to walk
 * (the CPS5.4 insight). This is strictly simpler than snapshotting a native
 * stack and is portable: tags are stable integers, not code addresses.
 *
 * To make the tags stable the context frames are a FIXED set emitted once into
 * the preamble (not per-site lambda-lifted as in CPS9): +, *, and - (with the
 * hole on either side), keyed by SK_TAG_*. The env (the captured non-hole
 * operand) is an int and is marshaled inline; richer env types via the
 * Serializable typeclass are a follow-on. The supported context subset is the
 * same single-hole integer (+, -, *) chain as CPS9; serial-shift may also be
 * the whole reset body (empty context).
 *
 * Surface runtime (emitted builtins, registered in builtins.c):
 *   tur_serial_cont_resume(k, v)      -- run the captured chain on v
 *   tur_serial_cont_serialize(k)      -- marshal to a length-prefixed buffer
 *   tur_serial_cont_deserialize(bytes)-- rebuild a runnable chain from a buffer
 * ========================================================================= */

/* Pure feasibility check (NO emission) for a serial-reset. Unlike the cloneable
 * path, an empty context (serial-shift IS the whole body) is allowed -- serial
 * continuations are an all-new feature with no legacy snapshots to preserve. */
static bool sk_can_lower(const Expr *e, const Expr *program) {
    if (!e || e->kind != EX_SERIAL_RESET) return false;
    if (!ty_intptr_safe(e->type.kind)) return false;
    const Expr *inner = e->as.serial_reset_.body;
    const Expr *if_node = find_ctx_if(inner, EX_SERIAL_SHIFT);

    /* See cl_can_lower: walk the shift-bearing context body, which is the whole
     * reset body for a flat context or -- with an `if` branch point at the root
     * or beneath outer frames -- the body with the `if` replaced by its
     * shift arm. */
    const Expr *shift_body = inner;
    ClonePool pool = {0};
    if (if_node) {
        const Expr *cond = NULL, *els = NULL; bool when = true;
        const Expr *branch = ctx_if_branch(if_node, EX_SERIAL_SHIFT,
                                           &cond, &els, &when);
        if (!branch) return false;
        shift_body = clone_spine(inner, if_node, branch, EX_SERIAL_SHIFT, &pool);
    }

    ClFrame frames[CL_MAX_CTX_FRAMES];
    CtxLet lets[CL_MAX_CTX_LETS];
    uint32_t nf = 0, nl = 0;
    const Expr *shift = collect_ctx(shift_body, EX_SERIAL_SHIFT,
                                    frames, &nf, program, /*want_names=*/false,
                                    lets, &nl);
    bool ok = shift && shift->as.serial_shift_.k_fn != NULL;
    pool_free(&pool);
    return ok;
}




/* Program scan: does any serial-reset lower onto the DK machine? Gates the DK
 * runtime prelude + the serial marshaling runtime. Mirrors sk_can_lower. */
static const Expr *g_sk_scan_root = NULL;  /* whole program, for the instance scan */
/* When true, the walk reports mere *presence* of serial syntax (any
 * serial-shift), not just lowerable resets. The serial runtime prelude
 * (tur_serial_cont_*) must be emitted whenever stdlib save-cont!/resume-cont!
 * could reference it -- i.e. whenever a serial-shift exists -- so an
 * unsupported context degrades cleanly instead of emitting an implicit
 * declaration that miscompiles. See
 * docs/reported/serial-shift-unsupported-context-miscompile.md. */
static bool g_sk_any_serial = false;

static bool uses_serial_dk(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_SERIAL_SHIFT && g_sk_any_serial) return true;
    if (e->kind == EX_SERIAL_RESET &&
        (g_sk_any_serial || sk_can_lower(e, g_sk_scan_root))) return true;
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_serial_dk(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_serial_dk(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_serial_dk(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_serial_dk(e->as.closure_.closure->fn->body);
        case EX_SERIAL_RESET:
            return uses_serial_dk(e->as.serial_reset_.body);
        case EX_CLONEABLE_RESET:
            return uses_serial_dk(e->as.cloneable_reset_.body);
        case EX_RESET:
            return uses_serial_dk(e->as.reset_.body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_serial_dk(e->as.let_.bindings[i].init)) return true;
            return uses_serial_dk(e->as.let_.body);
        case EX_IF:
            return uses_serial_dk(e->as.if_.cond) ||
                   uses_serial_dk(e->as.if_.then_) ||
                   uses_serial_dk(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_serial_dk(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_serial_dk(e->as.while_.cond) ||
                   uses_serial_dk(e->as.while_.body);
        case EX_SET:    return uses_serial_dk(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_serial_dk(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_serial_dk(e->as.return_.value);
        case EX_DEFER:  return uses_serial_dk(e->as.defer_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_serial_dk(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_serial_dk(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

/* True if the program contains any serial-shift/serial-reset, lowerable or not.
 * Gates emission of the DK machine + serial marshaling runtime so the stdlib
 * save-cont!/resume-cont! references never dangle. */
bool emit_cps_program_contains_serial(const Expr *program) {
    g_sk_scan_root = program;
    g_sk_any_serial = true;
    bool r = uses_serial_dk(program);
    g_sk_any_serial = false;
    g_sk_scan_root = NULL;
    return r;
}
