#include "emit_cps_ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cps_ir.h"
#include "cps.h"
#include "builtins.h"
#include "globals.h"
#include "arena.h"
#include "expr.h"

/* The elaborator's complete free-variable walker (backs the closure-capture
 * pass).  Reused here to find every enclosing var a delegated *composite*
 * (match / while / for / do / let / set! / ...) references, so such a form can be
 * lifted into a continuation env.  Declared in elab_internal.h. */
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
                            Binding **self_exclude, uint32_t n_self_exclude,
                            uint32_t *n_out);

/* True for a NULL or `{}` (ERK_EMPTY) effect row.  Used to distinguish a
 * provably effect-free `TY_FN` param (safe to delegate an indirect call
 * through) from an effectful callback (whose call would need DK threading).
 * Defined in src/passes/effect.c. */
bool effect_row_is_empty(struct EffectRow *row);

/* =========================================================================
 * CPS-IR-to-C backend -- Phase C1.  See emit_cps_ir.h for the ABI and scope.
 *
 * Two responsibilities:
 *   1. Whole-program classification (ensure_S): which colored functions the
 *      backend emits.  A function is emittable iff its CTerm is in the C1 core
 *      subset (scalar types, no reset/shift/effects, no unsupported form) AND
 *      no join point in it must be reified onto the heap chain -- i.e. no
 *      non-tail call to another emittable function.  That last clause is a
 *      monotone fixpoint (removing a function from the set only turns its
 *      callers' cps->cps edges into ordinary synchronous calls, which never
 *      forces a heap join), so it converges.
 *   2. Emission (emit_cps_ir_try_fn): walk the CTerm and write the DK-ABI body
 *      plus a direct-entry wrapper.
 *
 * Join points (CT_LETCONT) lower to a C local + a forward label/goto (plan:
 * "local labels/gotos"): delivering a value to join j assigns the join
 * parameter and `goto L<j>`.  This is sufficient for every join whose
 * continuation stays on the C stack (if-merges and the result of a synchronous
 * call).  A join that would have to survive a cps->cps call needs a real
 * dk_frame; that is deliberately out of the C1 subset -- such functions fall
 * back -- and lands in a later phase.
 * ========================================================================= */

/* ---- the emittable value set (non-scalar Tier A) --------------------- *
 * A type is "slot-representable at Tier A" when its value fits the one-word DK
 * slot by a plain cast: any <=64-bit integer/bool, or a cstr/pointer.  See
 * docs/upcoming/v1/cps-backend-non-scalar-values-plan.md.  Tier B (float, bit-
 * reinterpret) and Tier C (wide by-value aggregates, boxed) are not yet here.
 *
 * Deliberately absent: the OWNING pointers TY_REF / TY_RC / TY_WEAK / TY_LREF.
 * Do NOT add them here -- they are never bare slot values.  A bare owning
 * pointer cannot even be a function result / continuation payload in the source
 * language (`(defn f [] : rc<int> ...)` is a type error); owning values cross a
 * continuation only as FIELDS of a struct / ADT, so their slot discipline is the
 * enclosing aggregate's drop glue (N3), not a per-pointer cast.  The non-owning
 * borrows TY_REF_IMMUT / TY_REF_MUT are also omitted: a borrow threaded through a
 * continuation would outlive its referent, which the borrow checker rejects.
 * See docs/upcoming/v1/cps-backend-owning-pointers-plan.md. */
static bool slot_ty(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT64: case TY_BOOL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_CSTR:
        case TY_PTR_VOID:   /* raw pointer: Copy, non-owning -- casts through the slot */
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:  /* Tier B: bit-reinterpret */
            return true;
        default:
            return false;
    }
}

static const char *binder_ctype(EmitCtx *ctx, TypeKind ty);
static const char *binder_ctype_full(EmitCtx *ctx, TypeKind ty, const Type *t);

/* G3a: when set (while a colored-generic MONOMORPH is being CPS-emitted as a
 * self-contained island, and during the --dump-cps-mono probe), the slot-tier and
 * signature gates resolve a type through the active monomorph specialization
 * before reading it -- so the generic body's tyvar types (`A`, `(Option A)`) are
 * classified / boxed / bit-reinterpreted at their CONCRETE monomorph types.  NULL
 * in every ordinary emit path, so all of this is a strict no-op there. */
static EmitCtx *g_cps_mono_resolver = NULL;

/* Resolve `*t` through the active monomorph spec when the hook is on; else return
 * `*t` unchanged.  `out` provides storage for the resolved Type.  Returns the
 * type to use (either `out` or `t`). */
static const Type *cps_resolve_ty(const Type *t, Type *out) {
    if (g_cps_mono_resolver && t) { *out = emit_resolve_type(g_cps_mono_resolver, *(Type *)t); return out; }
    return t;
}

/* Forward decls used by ensure_S's G3b mono-template classification (defined
 * below). */
static bool mono_sig_ok(const FnDef *fd, const EmitAbiSpecialization *spec);
static bool is_cps_island(const CTerm *t, const Symbol **handled, int nh);

/* The AdtDef at the head of a struct / ADT / ADT-app type, or NULL. */
static const AdtDef *slot_agg_def(const Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_ADT) return t->as.adt_.def;
    if (t->kind == TY_APP) return type_adt_app_def((Type *)t);
    return NULL;
}

/* Tier C: a by-value aggregate that must be BOXED (heap-copied) to cross the
 * one-word DK slot -- as opposed to a carrier-ABI aggregate whose bit pattern
 * already IS the int64 carrier (would ride the slot by a plain cast) or a scalar
 * (Tier A/B).
 *
 * Restricted to owning-free flat products (`needs_drop_glue == false`): the box
 * is then a pure bitwise value copy with no refcount / drop concern, so it is
 * safe to heap-copy on store and deref on load even under a multi-shot
 * continuation (each crossing gets its own independent copy).  Aggregates with
 * owning fields (rc / ref / weak) still fall back -- their crossing needs
 * retain-on-copy plus drop glue (N3-O2).  Carrier heap-ADT / parametric-app
 * handles also stay out (bit-copying an owning int64 handle would duplicate its
 * refcount).  The def must be present: a bare surface annotation (e.g. a NULL
 * `return_type` def) is not enough -- callers pass the body/value Type, which
 * carries the real monomorphized def. */
static bool slot_box_ty(const Type *t) {
    if (!t) return false;
    Type _r; t = cps_resolve_ty(t, &_r);
    /* A heap-passed ADT / struct (`(Vec int)` -> `tur_adt_Vec__int *`, ...) is
     * already a pointer (the int64 carrier), NOT a by-value product to heap-copy.
     * Some heap ADT defs still have product *shape* (`{ len; cap; data }`), so
     * type_is_byvalue_adt_product can report true for them -- but boxing one would
     * double-indirect the handle (store a T** where a T* is expected) and later
     * free a non-box.  Exclude them here so they are neither admitted through the
     * Tier C by-value gate nor routed through slot_store/slot_load's box/unbox. */
    if (type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t)) return false;
    if (!type_is_byvalue_adt_product(*(Type *)t)) return false;
    const AdtDef *d = slot_agg_def(t);
    return d && !d->needs_drop_glue;
}

/* A heap-passed ADT / struct handle (`(Vec int)` -> `tur_adt_Vec__int *`, a heap
 * ADT node, ...).  Its C representation IS a pointer -- the int64 carrier -- so
 * it crosses the DK slot by a plain cast (like TY_PTR_VOID), never a box.  It is
 * an *owning* handle, so it is admitted through the slot / signature gate but NOT
 * through the capture gate (cap_ty_ok): a handle live across a control op is
 * captured into a continuation env, where sharing the owning pointer is only
 * sound in a single-shot continuation -- so cap_add gates a capture of it on
 * g_cap_single_shot (a handler CASE body, which runs per-perform, still bails).
 * As a slot value / atom / call-arg it just threads the pointer by move, matching
 * the direct emitter (whose owning ops are delegated via CT_LETRAW either way). */
static bool carrier_handle_ok(const Type *t) {
    if (!t) return false;
    return (type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t));
}

/* A value can cross the DK slot at the current tier: a Tier A/B scalar, a Tier C
 * owning-free by-value aggregate (boxed), or a heap-ADT/struct handle (plain-cast
 * int64 carrier).  This is the whole-Type gate used by classification wherever
 * the full Type is available (the bare-TypeKind slot_ty stays for scalar-only
 * sites).  It is deliberately WIDER than cap_ty_ok: a value may cross a slot by a
 * single move even when it may not be duplicated into a capture env. */
static bool slot_ok_t(const Type *t, TypeKind k) {
    Type rt; const Type *r = cps_resolve_ty(t, &rt);
    if (r != t) k = r->kind;
    return slot_ty(k) || slot_box_ty(r) || carrier_handle_ok(r);
}

/* Signature-position slot gate (a colored function's PARAMS and RETURN).  It is
 * stricter than slot_ok_t: only a SCALAR (Tier A/B) type is admitted.  A
 * non-scalar signature -- a heap-ADT/struct HANDLE (`(Set cstr)` ->
 * `tur_adt_Set__cstr *`) OR a by-value ADT aggregate (`slot_box_ty`, e.g. a
 * `tur_adt_H` record) -- is rejected.  The CPS backend emits a colored function
 * as `f__cps(<concrete params>, DK*)` plus a direct-entry wrapper
 * `f(<concrete params>)`; when the same base name is ALSO reached through the
 * uniform carrier/dict ABI (a typeclass method dict is `_Bool(int64_t,int64_t)`,
 * and the base name is specialized both concretely and as an int64 carrier), the
 * CPS wrapper + `__cps` re-emit the concrete signature and collide with the
 * carrier specialization -- a `conflicting types` / duplicate-definition C error.
 * A scalar signature has a single int64/bool/double ABI that never diverges
 * between the carrier and concrete paths, so it is safe; a non-scalar-signature
 * colored function evicts to the direct emitter, which owns the carrier bridge.
 * (Non-scalars still cross DK slots fine as INTERNAL values via slot_ok_t -- this
 * gate is signature-only.) */
static bool sig_slot_ok(const Type *t, TypeKind k) {
    Type rt; const Type *r = cps_resolve_ty(t, &rt);
    if (r != t) k = r->kind;
    return slot_ty(k);
}

/* Store a value into the one-word slot.  Returns a malloc'd C expression.
 *   Tier A: plain cast.
 *   Tier B: bit-reinterpret ((intptr_t)3.5 would truncate the fraction).
 *   Tier C: heap-copy the aggregate and store the pointer (unboxed on load). */
static char *slot_store(EmitCtx *ctx, TypeKind k, const Type *t, const char *e) {
    Buf b; buf_init(&b);
    Type _r; const Type *rt = cps_resolve_ty(t, &_r); if (rt != t) k = rt->kind; t = rt;
    if (slot_box_ty(t)) {
        const char *cty = emit_type_c_name(ctx, *(Type *)t);
        buf_printf(&b,
            "(intptr_t)({ %s *__bx = (%s *)malloc(sizeof(%s)); *__bx = (%s); __bx; })",
            cty, cty, cty, e);
    } else if (k == TY_FLOAT || k == TY_FLOAT64)
        buf_printf(&b, "(intptr_t)((union { double d; int64_t i; }){ .d = (%s) }).i", e);
    else if (k == TY_FLOAT32)
        buf_printf(&b, "(intptr_t)(uint32_t)((union { float f; uint32_t u; }){ .f = (%s) }).u", e);
    else
        buf_printf(&b, "(intptr_t)(%s)", e);
    buf_putc(&b, '\0');
    char *s = strdup(b.data); buf_free(&b); return s;
}

/* Load a value out of the one-word slot.  Returns a malloc'd C expression.
 *   Tier C: deref the boxed pointer to a value copy.  `consume` additionally
 *   frees the box -- only sound at a single-shot boundary (the root entry
 *   unwrap).  The continuation-internal boundaries pass consume=false and leak
 *   the box (matching the intentional DK-node leak): a multi-shot continuation
 *   could load the same slot more than once, so freeing there would double-free. */
static char *slot_load(EmitCtx *ctx, TypeKind k, const Type *t, const char *s, bool consume) {
    Buf b; buf_init(&b);
    Type _r; const Type *rt = cps_resolve_ty(t, &_r); if (rt != t) k = rt->kind; t = rt;
    if (slot_box_ty(t)) {
        const char *cty = emit_type_c_name(ctx, *(Type *)t);
        if (consume)
            buf_printf(&b,
                "({ %s *__bx = (%s *)(%s); %s __v = *__bx; free(__bx); __v; })",
                cty, cty, s, cty);
        else
            buf_printf(&b, "(*(%s *)(%s))", cty, s);
    } else if (k == TY_FLOAT || k == TY_FLOAT64)
        buf_printf(&b, "((union { double d; int64_t i; }){ .i = (int64_t)(%s) }).d", s);
    else if (k == TY_FLOAT32)
        buf_printf(&b, "((union { float f; uint32_t u; }){ .u = (uint32_t)(%s) }).f", s);
    else
        /* Plain cast to the value's real C type -- binder_ctype_full so a heap-ADT
         * handle (TY_ADT/APP/STRUCT) casts back to its pointer type, not int64. */
        buf_printf(&b, "(%s)(%s)", binder_ctype_full(ctx, k, t), s);
    buf_putc(&b, '\0');
    char *r = strdup(b.data); buf_free(&b); return r;
}

/* The Type a colored function's value has when delivered to KK_RET.  fd->return_type
 * can carry a NULL ADT def (the surface annotation is not def-resolved); the body's
 * type carries the real monomorphized def and the body's value is exactly what is
 * delivered, so prefer it for an aggregate return. */
static const Type *fn_ret_type(const FnDef *fd) {
    if (fd->body) {
        TypeKind bk = fd->body->type.kind;
        if ((bk == TY_ADT || bk == TY_APP || bk == TY_STRUCT)
            && slot_agg_def(&fd->body->type))
            return &fd->body->type;
    }
    return &fd->return_type;
}

/* The C type used to declare a let-binder of type kind `ty` (TY_NIL binders are
 * unit placeholders stored as int64_t). */
static const char *binder_ctype(EmitCtx *ctx, TypeKind ty) {
    if (ty == TY_NIL || ty == TY_UNKNOWN) return "int64_t";
    return emit_type_c_name(ctx, emit_type_from_kind(ty));
}

/* Like binder_ctype, but uses the full Type when it is a struct / ADT so the
 * local is declared with its real (monomorphized) C type -- e.g.
 * tur_adt_Option__int -- rather than a bare, wrong fallback.  Such a value is a
 * local that never crosses the DK slot (crossing is gated by atom_ok /
 * fn_sig_ok, which reject non-slot types); only scalar fields it yields cross. */
static const char *binder_ctype_full(EmitCtx *ctx, TypeKind ty, const Type *t) {
    /* TY_TYVAR: a monomorph body's generic binder (`A`) -- keep the named Type so
     * emit_type_c_name resolves it through the active spec to its concrete C type
     * (a scalar tyvar reaches this path; binder_ctype would drop the name and
     * mis-spell it).  Only occurs while a monomorph is emitted (spec active). */
    if (t && (ty == TY_STRUCT || ty == TY_ADT || ty == TY_APP || ty == TY_TYVAR))
        return emit_type_c_name(ctx, *t);
    return binder_ctype(ctx, ty);
}

/* Atom types we can materialize as a slot-representable value. */
static bool atom_ok(const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return slot_ty(a->ty) || a->ty == TY_UNKNOWN;  /* any <=64-bit int width */
        case CA_BOOL: return true;
        case CA_UNIT: return true;   /* nil placeholder (0) */
        case CA_STR:  return true;   /* cstr literal (Tier A pointer) */
        case CA_FLOAT: return true;  /* float/double literal (Tier B) */
        case CA_VAR:
            /* A poly-fat closure (`tur_poly_fn_t`) is a multi-word aggregate that
             * cannot ride the one-word slot / a `void*` temp: spilling it (`t2 =
             * f;`) or crossing it into a slot miscompiles.  Calling such a param
             * goes through a CT_LETRAW delegation (collect_free_vars, not atom_ok),
             * so rejecting it here only blocks the unsound slot-crossing use and
             * evicts a function that threads a poly-fn value as a plain value. */
            if (a->var && a->var->is_poly_fn) return false;
            return slot_ok_t(a->type, a->ty) || a->ty == TY_NIL;
        case CA_CVAR: return slot_ok_t(a->type, a->ty) || a->ty == TY_NIL;
        default:      return false;  /* CA_OTHER */
    }
}

static bool is_println_shape(BuiltinShape s) {
    return s == BS_PRINTLN_INT || s == BS_PRINTLN_BOOL || s == BS_PRINTLN_UINT
        || s == BS_PRINTLN_CSTR || s == BS_PRINTLN_FLOAT || s == BS_PRINTLN_FLOAT32;
}

static bool shape_supported(const BuiltinSpec *sp) {
    if (!sp) return false;
    switch (sp->shape) {
        case BS_BIN_INFIX:
        case BS_VARIADIC_FOLD:
        case BS_DIV_CHECK:
        case BS_PREFIX_UNARY:
        case BS_AND_SC:
        case BS_OR_SC:
        case BS_FUNC_CALL:   /* a plain C runtime call c_op(args); prim_expr emits it */
        case BS_PRINTLN_INT:
        case BS_PRINTLN_BOOL:
        case BS_PRINTLN_UINT:
        case BS_PRINTLN_CSTR:
        case BS_PRINTLN_FLOAT:
        case BS_PRINTLN_FLOAT32:
            return true;
        default:
            return false;
    }
}

/* Measurement helper (TUR_TRACE_EVICT readiness gate): find the first
 * CT_UNSUPPORTED node in a translated function, so the eviction trace can name
 * the residual form.  Not part of the admission logic (term_core_ok owns that);
 * this only walks the tree to surface a `why` for the trace. */
static const CTerm *first_unsupported(const CTerm *t) {
    if (!t) return NULL;
    const CTerm *r = NULL;
    switch (t->kind) {
        case CT_UNSUPPORTED: return t;
        case CT_LETVAL:   return first_unsupported(t->as.letval.body);
        case CT_LETPRIM:  return first_unsupported(t->as.letprim.body);
        case CT_LETCALL:  return first_unsupported(t->as.letcall.body);
        case CT_LETRAW:   return first_unsupported(t->as.letraw.body);
        case CT_LETCONT:
            r = first_unsupported(t->as.letcont.jbody);
            return r ? r : first_unsupported(t->as.letcont.body);
        case CT_IF:
            r = first_unsupported(t->as.if_.then_);
            return r ? r : first_unsupported(t->as.if_.else_);
        case CT_RESET:
            r = first_unsupported(t->as.reset.delim);
            return r ? r : first_unsupported(t->as.reset.body);
        case CT_SHIFT:    return first_unsupported(t->as.shift.body);
        case CT_HANDLE:
            r = first_unsupported(t->as.handle.delim);
            if (r) return r;
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++) {
                r = first_unsupported(t->as.handle.cases[i].case_body);
                if (r) return r;
            }
            return first_unsupported(t->as.handle.body);
        case CT_PERFORM:  return first_unsupported(t->as.perform.body);
        case CT_RESUME:   return first_unsupported(t->as.resume.body);
        case CT_CLONEABLE: return first_unsupported(t->as.cloneable.body);
        case CT_CALLCC:   return first_unsupported(t->as.callcc.body);
        default: return NULL;   /* CT_APPCONT / CT_TAILCALL: leaves */
    }
}

static bool term_core_ok(const CTerm *t);
static bool delim_ok(const CTerm *t);   /* reset-delim admission (permits KK_PROMPT delivery) */
static bool letraw_ok(const CTerm *t);  /* CT_LETRAW soundness (owning-drop guard) */
static bool binding_in_s(const Binding *b);  /* callee CPS-emitted? (cps->cps vs cps->direct) */

/* A poly-fat closure (`tur_poly_fn_t`) is a multi-word aggregate.  When such a
 * value is THREADED as a call argument the CT-IR spills it to an intermediate
 * one-word slot / `void*` temp first (`t2 = f;`), which cannot hold the fat
 * struct -- a hard C error (`incompatible types ... from 'tur_poly_fn_t'`).  A
 * function that only CALLS its poly-fn param (a delegated indirect call, never
 * re-threading it as an arg) is fine, so this is an arg-position check, not a
 * signature reject: fn_sig_ok still admits a poly-fn PARAM. */
static bool atom_is_fat_fn(const CAtom *a) {
    return a->kind == CA_VAR && a->var && a->var->is_poly_fn;
}

/* Whether an atom may cross into a call ARGUMENT slot.
 *
 *  - A poly-fat closure never can (spilling it to a temp slot miscompiles),
 *    regardless of whether the call is cps->cps or cps->direct.
 *  - A Tier C by-value-aggregate atom (slot_box_ty, e.g. `tur_adt_Option__int`)
 *    can cross a cps->cps call (the callee's `__cps` params match the CPS ABI)
 *    but NOT a cps->DIRECT call: there the delegated call emits the arg RAW to
 *    the callee's direct-emitter C signature, which takes a by-value ADT through
 *    the CARRIER ABI (int64_t) -- so a bare-struct arg where an int64 carrier is
 *    expected is a hard C error.  The direct emitter boxes such an arg
 *    (emit_byvalue_carrier_abi); the CPS delegation does not, so the function
 *    must EVICT.  Scalars and heap-handle carriers already ARE the int64 carrier
 *    and cross either way. */
static bool call_arg_ok(const CAtom *a, bool cps_to_direct) {
    if (!atom_ok(a)) return false;
    if (atom_is_fat_fn(a)) return false;
    if (cps_to_direct && (a->kind == CA_VAR || a->kind == CA_CVAR)
        && slot_box_ty(a->type))
        return false;
    return true;
}

/* ---- C3 delimited-control subset predicates -------------------------- *
 * The C3 backend lowers a reset/shift only in a restricted, zero-capture,
 * identity-receiver subset; anything else keeps its direct-style emission.
 * These predicates gate that subset. */

#define CC_MAX_BOUND 128

/* Source bindings that are NOT captures inside the lifted body being checked
 * (an effect handler case's params and continuation `k`, bound by the DKHandler
 * signature).  Set by has_capture() before each walk. */
static const Binding *g_excl_b[8];
static int            g_excl_n;

/* True while collecting captures for a SINGLE-SHOT continuation (a reset / handle
 * / perform continuation, or a shift body).  In the admitted subset a
 * continuation `k` is resumed AT MOST ONCE (a second resume is a hard error,
 * TUR-E0201; multi-shot needs cloneable-shift, which falls back), so such a
 * continuation runs at most once.  That makes it safe to capture an OWNING value
 * (an rc handle, a drop-glue aggregate, a heap handle) by a shallow value copy
 * with no clone: the value's drop -- which the drop-insertion pass sinks into the
 * continuation -- then runs exactly once, balanced, no double-free.  A handler
 * CASE body is NOT single-shot (it runs once per perform, potentially many times
 * in a loop), so this stays false for collect_caps_case and owning captures there
 * still bail. */
static bool g_cap_single_shot;

static bool binding_excluded(const Binding *b) {
    for (int i = 0; i < g_excl_n; i++) if (g_excl_b[i] == b) return true;
    return false;
}

/* True if `b` is one of a CT_CLONEABLE node's own `let` prelude bindings -- a C
 * local emitted at the reset site, hence not a capture of any enclosing frame. */
static bool cloneable_is_local(const CTerm *t, const Binding *b) {
    for (uint32_t i = 0; i < t->as.cloneable.n_lets; i++)
        if (t->as.cloneable.lets[i].binding == b) return true;
    return false;
}

/* The free variables of a CT_CLONEABLE node's direct-emitted context sub-exprs
 * (each `let` init, and -- if present -- the `if` condition and pure arm), with
 * the node's own `let` bindings excluded (they are locals at the reset site).
 * Returns a malloc'd array the caller frees; *n_out is the count (0 => NULL).
 * The same complete free-var analysis CT_LETRAW uses, so a capturing context
 * sub-expr surfaces its enclosing locals for the lifted-helper env. */
static Binding **cloneable_ctx_free_vars(const CTerm *t, uint32_t *n_out) {
    *n_out = 0;
    Binding *locals[16];
    uint8_t n_locals = 0;
    for (uint32_t i = 0; i < t->as.cloneable.n_lets && n_locals < 16; i++)
        if (t->as.cloneable.lets[i].binding)
            locals[n_locals++] = (Binding *)t->as.cloneable.lets[i].binding;

    Binding **acc = NULL;
    uint32_t n_acc = 0, cap = 0;
    #define CLONE_ADD_FV(ex) do { const Expr *_e = (ex); \
        if (_e) { uint32_t _nf = 0; \
            Binding **_fv = collect_free_vars(_e, locals, n_locals, NULL, 0, &_nf); \
            for (uint32_t _i = 0; _i < _nf; _i++) { \
                if (n_acc == cap) { cap = cap ? cap * 2 : 8; \
                    acc = realloc(acc, cap * sizeof(Binding *)); } \
                acc[n_acc++] = _fv[_i]; } \
            free(_fv); } } while (0)
    for (uint32_t i = 0; i < t->as.cloneable.n_lets; i++)
        CLONE_ADD_FV(t->as.cloneable.lets[i].init);
    CLONE_ADD_FV(t->as.cloneable.if_cond);
    CLONE_ADD_FV(t->as.cloneable.if_pure);
    #undef CLONE_ADD_FV
    *n_out = n_acc;
    return acc;
}

/* True if `t` references a free variable that a lifted continuation / shift /
 * handler-case helper would have to capture: a non-global, non-excluded source
 * var, or a CPS var not bound within `t` and not the excluded incoming value
 * `exclude` (UINT32_MAX = none).  The zero-capture cut falls back when it holds. */
static bool has_capture_rec(const CTerm *t, uint32_t exclude,
                            uint32_t *bound, int nb) {
    if (!t || nb >= CC_MAX_BOUND) return t ? true : false;
    #define CC_ATOM(a) do { const CAtom *_a = (a); \
        if (_a->kind == CA_VAR) { \
            if (_a->var && !_a->var->is_global && !binding_excluded(_a->var)) { \
                /* a source binding's cvar id is its binding id; the excluded */ \
                /* incoming value and locally-bound names are not captures.   */ \
                uint32_t _id = _a->var->id; bool _f = (_id != exclude); \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; } \
                if (_f) return true; } } \
        else if (_a->kind == CA_CVAR) { \
            if (_a->cvar_id != exclude) { bool _f = true; \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _a->cvar_id) { _f = false; break; } \
                if (_f) return true; } } } while (0)
    switch (t->kind) {
        case CT_APPCONT: CC_ATOM(&t->as.appcont.v); return false;
        case CT_LETVAL:
            CC_ATOM(&t->as.letval.v);
            bound[nb] = t->as.letval.x.id;
            return has_capture_rec(t->as.letval.body, exclude, bound, nb + 1);
        case CT_LETPRIM:
            for (uint32_t i = 0; i < t->as.letprim.n; i++) CC_ATOM(&t->as.letprim.args[i]);
            bound[nb] = t->as.letprim.x.id;
            return has_capture_rec(t->as.letprim.body, exclude, bound, nb + 1);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++) CC_ATOM(&t->as.letcall.args[i]);
            bound[nb] = t->as.letcall.x.id;
            return has_capture_rec(t->as.letcall.body, exclude, bound, nb + 1);
        case CT_TAILCALL:
            for (uint32_t i = 0; i < t->as.tailcall.n; i++) CC_ATOM(&t->as.tailcall.args[i]);
            return false;
        case CT_IF:
            CC_ATOM(&t->as.if_.cond);
            return has_capture_rec(t->as.if_.then_, exclude, bound, nb)
                || has_capture_rec(t->as.if_.else_, exclude, bound, nb);
        case CT_LETCONT:
            bound[nb] = t->as.letcont.param.id;
            return has_capture_rec(t->as.letcont.jbody, exclude, bound, nb + 1)
                || has_capture_rec(t->as.letcont.body, exclude, bound, nb + 1);
        case CT_RESUME:
            CC_ATOM(&t->as.resume.k);   /* k is the handler's excluded continuation */
            CC_ATOM(&t->as.resume.v);
            bound[nb] = t->as.resume.x.id;
            return has_capture_rec(t->as.resume.body, exclude, bound, nb + 1);
        case CT_CLONEABLE: {
            /* receiver is a global fn -- no capture; Shape 2 frame operands may
             * be captured vars (they ride the frame env), except those naming the
             * node's own `let` prelude locals. */
            for (uint32_t _i = 0; _i < t->as.cloneable.n_frames; _i++) {
                const CAtom *_op = &t->as.cloneable.frames[_i].operand;
                if (_op->kind == CA_VAR && cloneable_is_local(t, _op->var)) continue;
                CC_ATOM(_op);
            }
            /* Direct-emitted context sub-exprs (let inits, if cond/pure arm): a
             * non-local free var they reference is a capture too. */
            uint32_t _nfv = 0;
            Binding **_fv = cloneable_ctx_free_vars(t, &_nfv);
            for (uint32_t _i = 0; _i < _nfv; _i++) {
                const Binding *_bb = _fv[_i];
                if (!_bb || _bb->is_global || binding_excluded(_bb)) continue;
                uint32_t _id = _bb->id; bool _f = (_id != exclude);
                for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                if (_f) { free(_fv); return true; }
            }
            free(_fv);
            /* U7: a closure receiver's free vars are captures too. */
            if (t->as.cloneable.receiver_expr) {
                uint32_t _rn = 0;
                Binding **_rfv = collect_free_vars(t->as.cloneable.receiver_expr, NULL, 0, NULL, 0, &_rn);
                for (uint32_t _i = 0; _i < _rn; _i++) {
                    const Binding *_bb = _rfv[_i];
                    if (_bb && !_bb->is_global && !binding_excluded(_bb)) {
                        uint32_t _id = _bb->id; bool _f = (_id != exclude);
                        for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                        if (_f) { free(_rfv); return true; }
                    }
                }
                free(_rfv);
            }
            bound[nb] = t->as.cloneable.x.id;
            return has_capture_rec(t->as.cloneable.body, exclude, bound, nb + 1);
        }
        case CT_LETRAW: {
            /* The complete set of enclosing names the delegated Expr references is
             * exactly its free-variable set -- the same complete walker
             * collect_caps_rec uses.  A non-atomic operand (O1-a: e.g.
             * `(rc/of (+ a b))`) references locals nested BELOW a top-level var, so
             * walk the whole operand rather than only direct-var operands;
             * otherwise a real capture would be missed and the term wrongly
             * admitted into a lifted zero-capture body (an undeclared C name).  A
             * composite the walker cannot analyze surfaces no free vars, matching
             * the collect_caps_rec / cap_add fallback path. */
            const Expr *le = t->as.letraw.e;
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(le, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv; i++) {
                const Binding *b = fv[i];
                if (!b || b->is_global || binding_excluded(b)) continue;
                uint32_t _id = b->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) { free(fv); return true; }
            }
            free(fv);
            bound[nb] = t->as.letraw.x.id;
            return has_capture_rec(t->as.letraw.body, exclude, bound, nb + 1);
        }
        case CT_CALLCC: {
            /* A capturing receiver makes the escape landing capture its free vars;
             * surface them (collect_free_vars descends into the EX_CALLCC receiver)
             * and report a capture so the zero-capture join cut sees it.  Then bind
             * x and recurse the body. */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(t->as.callcc.e, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv; i++) {
                const Binding *cb = fv[i];
                if (cb && !cb->is_global && !binding_excluded(cb)) {
                    uint32_t _id = cb->id; bool _f = (_id != exclude);
                    for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                    if (_f) { free(fv); return true; }
                }
            }
            free(fv);
            bound[nb] = t->as.callcc.x.id;
            return has_capture_rec(t->as.callcc.body, exclude, bound, nb + 1);
        }
        default: return true;   /* nested reset/shift/handle/perform in a lifted body: bail */
    }
    #undef CC_ATOM
}

static bool has_capture(const CTerm *t, uint32_t exclude) {
    uint32_t bound[CC_MAX_BOUND];
    g_excl_n = 0;
    return has_capture_rec(t, exclude, bound, 0);
}

/* ---- N6.3: capture collection for a lifted perform continuation ---------- *
 * A perform continuation that references enclosing locals is lifted with a real
 * env (a heap struct of the captured values), not the zero-capture shortcut.
 * This collector gathers the distinct captured SOURCE bindings a body needs,
 * and is exhaustive-or-bails: it collects a scalar (Copy, multi-shot-safe)
 * CA_VAR capture, and sets `ok=false` for anything it cannot represent in an env
 * (a captured CPS var, a non-scalar capture, a delegated composite with a
 * capture, or a nested control op).  A false `ok` means the caller keeps the
 * zero-capture fallback.  Mirrors has_capture_rec's traversal exactly so no
 * capture is missed (a missed capture would emit an undeclared reference). */
#define CC_MAX_CAPS 16
/* A capture is either a source binding (`b`, named via name_for_binding) or a
 * CPS-introduced result var (`cvname`, a fresh "tN" name that is both its
 * declaration and its reference).  Exactly one of b / cvname is set. */
typedef struct {
    const Binding *b[CC_MAX_CAPS];
    const char    *cvname[CC_MAX_CAPS];
    uint32_t       cvid[CC_MAX_CAPS];
    TypeKind       ty[CC_MAX_CAPS];
    const Type    *type[CC_MAX_CAPS];   /* full type (Tier C by-value aggregate); may be NULL */
    bool           polyfn[CC_MAX_CAPS]; /* a rank-2 poly fn value -- env field is tur_poly_fn_t */
    int n; bool ok;
} CapSet;

/* A capture rides the env by value: a scalar (Tier A/B) or an owning-free
 * by-value aggregate (Tier C, slot_box_ty).  Both are Copy, so a leaked env
 * storing them is multi-shot-safe with no retain/drop.  Owning handles / carrier
 * ADTs are rejected (their copy would duplicate a refcount). */
static bool cap_ty_ok(TypeKind ty, const Type *type) {
    return slot_ty(ty) || slot_box_ty(type);
}

static void cap_add(CapSet *cs, const Binding *b, TypeKind ty, const Type *type) {
    /* A rank-2 poly fn value is a fat closure (tur_poly_fn_t, wider than a slot).
     * It is Copy -- a borrowed function value the callee never drops (its owner
     * outlives the call) -- so it rides the env by value like a by-value
     * aggregate, with `tur_poly_fn_t` as the env-field type.  Everything else
     * goes through the scalar / by-value-aggregate gate. */
    bool is_poly = (b && b->is_poly_fn);
    /* A `^borrow` parameter (is_borrow) is a type-system-guaranteed non-consuming
     * value: the callee reads it but never drops or moves it (the caller owns it
     * and outlives the call, including any deferred continuation, which runs
     * inside this function's dynamic extent).  So an owning `^borrow` value (an
     * rc handle, a drop-glue aggregate) that fails the Copy gate above still rides
     * the env by a shallow value copy with NO retain/drop: the shared handle is
     * never released by this function, so no double-free, and the refcount is
     * unchanged from the direct path (multi-shot reads stay read-only). */
    bool borrowed = (b && b->is_borrow);
    /* In a single-shot continuation any value -- including an owning one -- may
     * ride the env by a shallow value copy: the continuation runs at most once,
     * so a drop it performs on the captured value runs at most once (balanced).
     * (A fn value still routes through the poly path for its tur_poly_fn_t env
     * type; a by-value aggregate through slot_box for its unbox.) */
    bool owning_ok = g_cap_single_shot;
    if (!is_poly && !borrowed && !owning_ok && !cap_ty_ok(ty, type)) { cs->ok = false; return; }
    for (int i = 0; i < cs->n; i++) if (cs->b[i] == b) return;
    if (cs->n >= CC_MAX_CAPS) { cs->ok = false; return; }
    cs->b[cs->n] = b; cs->cvname[cs->n] = NULL; cs->ty[cs->n] = ty;
    cs->type[cs->n] = type; cs->polyfn[cs->n] = is_poly; cs->n++;
}

/* The C type of capture slot i: a fat closure is `tur_poly_fn_t`, everything
 * else its scalar / by-value-aggregate binder type. */
static const char *cap_ctype(EmitCtx *ctx, const CapSet *caps, int i) {
    if (caps->polyfn[i]) return "tur_poly_fn_t";
    return binder_ctype_full(ctx, caps->ty[i], caps->type[i]);
}

static void cap_add_cvar(CapSet *cs, uint32_t id, const char *name, TypeKind ty, const Type *type) {
    if (!cap_ty_ok(ty, type) || !name) { cs->ok = false; return; }
    for (int i = 0; i < cs->n; i++) if (cs->cvname[i] && cs->cvid[i] == id) return;
    if (cs->n >= CC_MAX_CAPS) { cs->ok = false; return; }
    cs->b[cs->n] = NULL; cs->cvname[cs->n] = name; cs->cvid[cs->n] = id;
    cs->ty[cs->n] = ty; cs->type[cs->n] = type; cs->polyfn[cs->n] = false; cs->n++;
}

static void collect_caps_rec(const CTerm *t, uint32_t exclude,
                             uint32_t *bound, int nb, CapSet *cs) {
    if (!cs->ok) return;
    if (!t || nb >= CC_MAX_BOUND) { if (!t) return; cs->ok = false; return; }
    #define COL_ATOM(a) do { const CAtom *_a = (a); \
        if (_a->kind == CA_VAR) { \
            if (_a->var && !_a->var->is_global && !binding_excluded(_a->var)) { \
                uint32_t _id = _a->var->id; bool _f = (_id != exclude); \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; } \
                if (_f) cap_add(cs, _a->var, _a->ty, _a->type); } } \
        else if (_a->kind == CA_CVAR) { \
            if (_a->cvar_id != exclude) { bool _f = true; \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _a->cvar_id) { _f = false; break; } \
                if (_f) cap_add_cvar(cs, _a->cvar_id, _a->cvar_name, _a->ty, _a->type); } } } while (0)
    /* A free source var referenced inside a delegated (direct-emitted) Expr: the
     * same test COL_ATOM applies to a CA_VAR, but reaching through an EX_VAR node
     * to its binding.  Its Type carries the monomorphized def (for slot_box_ty). */
    #define COL_VAREXPR(ve) do { const Expr *_e = (ve); \
        while (_e && _e->kind == EX_ASCRIBE) _e = _e->as.ascribe_.inner; \
        if (_e && _e->kind == EX_VAR && _e->as.var.binding \
            && !_e->as.var.binding->is_global \
            && !binding_excluded(_e->as.var.binding)) { \
            const Binding *_b = _e->as.var.binding; \
            uint32_t _id = _b->id; bool _f = (_id != exclude); \
            for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; } \
            if (_f) cap_add(cs, _b, _b->type.kind, &_b->type); } } while (0)
    switch (t->kind) {
        case CT_APPCONT: COL_ATOM(&t->as.appcont.v); return;
        case CT_LETVAL:
            COL_ATOM(&t->as.letval.v);
            bound[nb] = t->as.letval.x.id;
            collect_caps_rec(t->as.letval.body, exclude, bound, nb + 1, cs); return;
        case CT_LETPRIM:
            for (uint32_t i = 0; i < t->as.letprim.n; i++) COL_ATOM(&t->as.letprim.args[i]);
            bound[nb] = t->as.letprim.x.id;
            collect_caps_rec(t->as.letprim.body, exclude, bound, nb + 1, cs); return;
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++) COL_ATOM(&t->as.letcall.args[i]);
            bound[nb] = t->as.letcall.x.id;
            collect_caps_rec(t->as.letcall.body, exclude, bound, nb + 1, cs); return;
        case CT_TAILCALL:
            for (uint32_t i = 0; i < t->as.tailcall.n; i++) COL_ATOM(&t->as.tailcall.args[i]);
            return;
        case CT_IF:
            COL_ATOM(&t->as.if_.cond);
            collect_caps_rec(t->as.if_.then_, exclude, bound, nb, cs);
            collect_caps_rec(t->as.if_.else_, exclude, bound, nb, cs); return;
        case CT_LETCONT:
            bound[nb] = t->as.letcont.param.id;
            collect_caps_rec(t->as.letcont.jbody, exclude, bound, nb + 1, cs);
            collect_caps_rec(t->as.letcont.body, exclude, bound, nb + 1, cs); return;
        case CT_RESUME:
            /* resume k v: k and v are captured/threaded like any operand; the
             * result binder x is bound for the continuation body.  A capturing
             * handler case resumes, so this must be collectable. */
            COL_ATOM(&t->as.resume.k);
            COL_ATOM(&t->as.resume.v);
            bound[nb] = t->as.resume.x.id;
            collect_caps_rec(t->as.resume.body, exclude, bound, nb + 1, cs); return;
        case CT_CLONEABLE: {
            /* The receiver is a named global fn (no capture); Shape 2 frame
             * operands may be captured vars (they ride the frame env), except
             * those naming the node's own `let` prelude locals.  The direct-
             * emitted context sub-exprs (let inits, if cond/pure) contribute their
             * non-local free vars as captures too.  Bind the result binder for the
             * continuation body and collect its captures. */
            for (uint32_t _i = 0; _i < t->as.cloneable.n_frames; _i++) {
                const CAtom *_op = &t->as.cloneable.frames[_i].operand;
                if (_op->kind == CA_VAR && cloneable_is_local(t, _op->var)) continue;
                COL_ATOM(_op);
            }
            uint32_t _nfv = 0;
            Binding **_fv = cloneable_ctx_free_vars(t, &_nfv);
            for (uint32_t _i = 0; _i < _nfv && cs->ok; _i++) {
                const Binding *_bb = _fv[_i];
                if (!_bb || _bb->is_global || binding_excluded(_bb)) continue;
                uint32_t _id = _bb->id; bool _f = (_id != exclude);
                for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                if (_f) cap_add(cs, _bb, _bb->type.kind, &_bb->type);
            }
            free(_fv);
            /* U7: a closure receiver (receiver_expr) contributes its own captures
             * -- collect_free_vars surfaces the closure's free vars so each scalar
             * capture rides the lifted env (non-Copy bails to fallback). */
            if (t->as.cloneable.receiver_expr) {
                uint32_t _rn = 0;
                Binding **_rfv = collect_free_vars(t->as.cloneable.receiver_expr, NULL, 0, NULL, 0, &_rn);
                for (uint32_t _i = 0; _i < _rn && cs->ok; _i++) {
                    const Binding *_bb = _rfv[_i];
                    if (!_bb || _bb->is_global || binding_excluded(_bb)) continue;
                    uint32_t _id = _bb->id; bool _f = (_id != exclude);
                    for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                    if (_f) cap_add(cs, _bb, _bb->type.kind, &_bb->type);
                }
                free(_rfv);
            }
            bound[nb] = t->as.cloneable.x.id;
            collect_caps_rec(t->as.cloneable.body, exclude, bound, nb + 1, cs); return;
        }
        case CT_LETRAW: {
            /* A delegated (direct-emitted) owning-value op.  Its operand vars are
             * the only free names it references (operands are atomic).  Collect
             * each -- the same enumeration has_capture_rec walks -- so a lifted
             * body containing a delegated op that captures a Copy value (scalar or
             * owning-free by-value aggregate, e.g. `(.field p)`) is admitted with
             * the operand riding the env.  Composites this scan does not enumerate
             * bail to fallback (cs->ok = false), matching has_capture_rec's
             * conservative default. */
            /* A delegated (direct-emitted) op.  Whatever its shape -- an rc op, a
             * field read, a struct build, a direct/indirect call, or a delegated
             * composite (match / while / for / do / let / ...) -- the complete set
             * of enclosing names it references is exactly its free-variable set.
             * Use the elaborator's free-var walker (the same complete analysis the
             * closure-capture pass uses) so a complex operand like `(+ x 5)` inside
             * a delegated call captures `x`, not just top-level direct-var args.
             * Each free var is filtered against the CPS-bound set / exclude /
             * globals and handed to cap_add, which admits Copy captures (scalar or
             * owning-free by-value aggregate) and bails to fallback on a non-Copy
             * capture.  If the walker ever missed a free var, that var would be an
             * undeclared C name in the lifted helper -- a compile error, not a
             * silent miscompile. */
            const Expr *le = t->as.letraw.e;
            /* U2: a delegated (call/cc f)/(escape f) references its enclosing
             * captures through the receiver `f`; collect_free_vars now descends
             * into an EX_CALLCC receiver (elab_core.c), so a capturing receiver's
             * enclosing locals are surfaced here and ride the lifted continuation
             * env (a scalar capture is admitted by cap_add; a non-Copy capture
             * bails to fallback). */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(le, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv && cs->ok; i++) {
                const Binding *b = fv[i];
                if (!b || b->is_global || binding_excluded(b)) continue;
                uint32_t _id = b->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) cap_add(cs, b, b->type.kind, &b->type);
            }
            free(fv);
            bound[nb] = t->as.letraw.x.id;
            collect_caps_rec(t->as.letraw.body, exclude, bound, nb + 1, cs); return;
        }
        case CT_RESET:
            /* A nested reset sitting in a lifted continuation (two sibling nested
             * resets: the second lands in the first's continuation, e.g.
             * `(reset (+ (reset (shift ...)) (reset (shift ...))))`).  Its
             * delimited body runs under its own (separately lifted) prompt and its
             * continuation continues in this same helper, so collect the free
             * captures of both -- binding the reset value for the continuation --
             * rather than bailing.  emit_reset recurses to emit the nested prompt;
             * this just enumerates the captures its lifted helpers ride. */
            collect_caps_rec(t->as.reset.delim, exclude, bound, nb, cs);
            bound[nb] = t->as.reset.x.id;
            collect_caps_rec(t->as.reset.body, exclude, bound, nb + 1, cs); return;
        case CT_SHIFT:
            /* The delimited body under a nested reset ends in an (abortive) shift;
             * its body's captures ride the shift-body helper's env.  Walk it so a
             * capture threaded through the enclosing continuation is not missed. */
            collect_caps_rec(t->as.shift.body, exclude, bound, nb, cs); return;
        case CT_CALLCC: {
            /* The receiver f may capture enclosing locals (build_callcc admits a
             * capturing closure).  Surface its free vars -- collect_free_vars
             * descends into the EX_CALLCC receiver -- and ride each scalar capture
             * in the lifted env (a non-Copy capture bails to fallback via cap_add),
             * mirroring the CT_LETRAW-delegated callcc.  Then bind x and walk body. */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(t->as.callcc.e, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv && cs->ok; i++) {
                const Binding *cb = fv[i];
                if (!cb || cb->is_global || binding_excluded(cb)) continue;
                uint32_t _id = cb->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) cap_add(cs, cb, cb->type.kind, &cb->type);
            }
            free(fv);
            bound[nb] = t->as.callcc.x.id;
            collect_caps_rec(t->as.callcc.body, exclude, bound, nb + 1, cs); return;
        }
        default:
            /* Nested control ops in a lifted body: out of this collector's scope. */
            cs->ok = false; return;
    }
    #undef COL_ATOM
    #undef COL_VAREXPR
}

/* Collect the scalar source captures of `body` (excluding the value param
 * `exclude`).  Returns true and fills `cs` when every capture is collectable;
 * false means the zero-capture fallback must be kept. */
static bool collect_caps(const CTerm *body, uint32_t exclude, CapSet *cs) {
    uint32_t bound[CC_MAX_BOUND];
    cs->n = 0; cs->ok = true;
    g_cap_single_shot = true;   /* a reset/handle/perform continuation or shift body */
    collect_caps_rec(body, exclude, bound, 0, cs);
    g_cap_single_shot = false;
    return cs->ok;
}

/* Collect a handler case body's scalar captures, excluding the case's own params
 * and continuation k (bound by the DKHandler signature).  Same collect-or-bail
 * contract as collect_caps. */
static bool collect_caps_case(const CTerm *body, const CHandleCase *hc, CapSet *cs) {
    g_excl_n = 0;
    for (uint32_t i = 0; i < hc->n_params && g_excl_n < 8; i++)
        g_excl_b[g_excl_n++] = hc->params[i];
    if (hc->k && g_excl_n < 8) g_excl_b[g_excl_n++] = hc->k;
    uint32_t bound[CC_MAX_BOUND];
    cs->n = 0; cs->ok = true;
    g_cap_single_shot = false;   /* a handler case body runs once per perform, not single-shot */
    collect_caps_rec(body, UINT32_MAX, bound, 0, cs);
    g_excl_n = 0;
    return cs->ok;
}

/* A reset continuation must be self-contained: every KK_VAR it delivers to must
 * name a join defined within it (its exits otherwise are KK_RET, which the
 * lifted helper maps to its own k).  Rejects a body that escapes to an
 * enclosing join. */
static bool joins_closed_rec(const CTerm *t, uint32_t *def, int nd) {
    if (!t || nd >= CC_MAX_BOUND) return t == NULL;
    switch (t->kind) {
        case CT_APPCONT:
            if (t->as.appcont.kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.appcont.kont.id) return true;
                return false;
            }
            return true;
        case CT_LETVAL:  return joins_closed_rec(t->as.letval.body, def, nd);
        case CT_LETPRIM: return joins_closed_rec(t->as.letprim.body, def, nd);
        case CT_LETCALL: return joins_closed_rec(t->as.letcall.body, def, nd);
        case CT_LETRAW:  return joins_closed_rec(t->as.letraw.body, def, nd);
        case CT_TAILCALL:
            if (t->as.tailcall.kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.tailcall.kont.id) return true;
                return false;
            }
            return true;
        case CT_IF:
            return joins_closed_rec(t->as.if_.then_, def, nd)
                && joins_closed_rec(t->as.if_.else_, def, nd);
        case CT_LETCONT:
            def[nd] = t->as.letcont.j.id;
            return joins_closed_rec(t->as.letcont.jbody, def, nd + 1)
                && joins_closed_rec(t->as.letcont.body, def, nd + 1);
        case CT_RESUME: return joins_closed_rec(t->as.resume.body, def, nd);
        case CT_CLONEABLE: return joins_closed_rec(t->as.cloneable.body, def, nd);
        case CT_CALLCC: return joins_closed_rec(t->as.callcc.body, def, nd);
        default: return false;
    }
}

static bool reset_body_ok(const CTerm *t) {
    if (!term_core_ok(t)) return false;
    uint32_t def[CC_MAX_BOUND];
    return joins_closed_rec(t, def, 0);
}

/* A shift body in the C3 subset: straight-line (letval/letprim/letcall) ending
 * in delivery of a scalar atom to the prompt.  No branches, tail calls, joins,
 * or nested delimiters. */
static bool shift_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind == KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && shift_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return shift_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return shift_body_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && shift_body_ok(t->as.letraw.body);
        case CT_CALLCC:
            /* A call/cc / escape hoisted into the shift body's straight-line
             * abort-value computation (e.g. `(shift (fn [v] v) (+ 1 (escape f)))`):
             * emit_lifted emits the body through emit_term, which lowers CT_CALLCC
             * via the native setjmp landing (emit_callcc), so it stays in the CT-IR
             * path rather than delegating to emit_cps.c. */
            return (slot_ok_t(t->as.callcc.x.type, t->as.callcc.x.ty) || t->as.callcc.x.ty == TY_NIL)
                && shift_body_ok(t->as.callcc.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && shift_body_ok(t->as.if_.then_) && shift_body_ok(t->as.if_.else_);
        default: return false;
    }
}

/* A perform continuation (C4): straight-line, delivered to KK_RET as a scalar
 * (the lifted DKFrame returns it).  No branches / tail calls / joins / nested
 * control. */
static bool perform_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* Delivered to the function's return continuation (KK_RET) or, when
             * the perform is the tail of a delimited body, to the enclosing
             * prompt (KK_PROMPT); the lifted frame returns the value either way. */
            return (t->as.appcont.kont.kind == KK_RET || t->as.appcont.kont.kind == KK_PROMPT)
                && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && perform_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return perform_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return perform_body_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && perform_body_ok(t->as.letraw.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && perform_body_ok(t->as.if_.then_) && perform_body_ok(t->as.if_.else_);
        default: return false;
    }
}

/* A handler case body (C4): straight-line with `resume` (dk_invoke), delivered
 * to the prompt (KK_PROMPT) as a scalar.  resume.k is the continuation binding
 * (not scalar-checked); resume.v and other atoms are scalar. */
static bool handle_case_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind == KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && handle_case_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return handle_case_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return handle_case_ok(t->as.letcall.body);
        case CT_RESUME:
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && handle_case_ok(t->as.resume.body);
        case CT_LETRAW:
            return letraw_ok(t) && handle_case_ok(t->as.letraw.body);
        case CT_CALLCC:
            /* A call/cc / escape hoisted into a handler case body (e.g. `(resume k
             * (+ 1 (escape f)))`): emit_lifted emits the case through emit_term,
             * which lowers CT_CALLCC via the native setjmp landing (emit_callcc),
             * so it stays in the CT-IR path rather than delegating to emit_cps.c. */
            return (slot_ok_t(t->as.callcc.x.type, t->as.callcc.x.ty) || t->as.callcc.x.ty == TY_NIL)
                && handle_case_ok(t->as.callcc.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && handle_case_ok(t->as.if_.then_) && handle_case_ok(t->as.if_.else_);
        default: return false;
    }
}

/* An owning-value op that allocates or increments a refcount, i.e. creates a
 * drop obligation (rc/of, rc/clone).  rc/strong-count / rc->ptr only read. */
static bool owning_alloc_expr(const Expr *e) {
    return e && (e->kind == EX_RC_OF || e->kind == EX_RC_CLONE);
}

/* True if `e` is `(rc/drop v)` where v is the binding with id `bid`. */
static bool is_rc_drop_of(const Expr *e, uint32_t bid) {
    if (!e || e->kind != EX_RC_DROP) return false;
    const Expr *arg = e->as.rc_drop_.expr;
    while (arg && arg->kind == EX_ASCRIBE) arg = arg->as.ascribe_.inner;
    return arg && arg->kind == EX_VAR && arg->as.var.binding
        && arg->as.var.binding->id == bid;
}

/* Is owning binding `bid` dropped on a straight-line path before any control op,
 * branch, delivery, or the end of the term?  This is the soundness condition for
 * emitting an rc alloc/clone on the CPS path: an abortive `shift` discards its
 * continuation, so a drop that is not reached before the next control op would be
 * dropped from the program (leak / miscompile).  Conservative: any branch
 * (CT_IF / CT_LETCONT) or delivery before the drop returns false (fall back). */
static bool owning_dropped_before_control(const CTerm *t, uint32_t bid) {
    while (t) {
        switch (t->kind) {
            case CT_LETRAW:
                if (is_rc_drop_of(t->as.letraw.e, bid)) return true;
                t = t->as.letraw.body; break;
            case CT_LETVAL:  t = t->as.letval.body;  break;
            case CT_LETPRIM: t = t->as.letprim.body; break;
            case CT_LETCALL: t = t->as.letcall.body; break;
            case CT_LETCONT:
                /* Execution proceeds in the body (which may jump to the join);
                 * the join's jbody is reached via an appcont, i.e. after this
                 * point, so a drop must appear in the body before that jump. */
                t = t->as.letcont.body; break;
            case CT_IF:
                /* Dropped before control iff dropped on BOTH branches (whichever
                 * runs, the value is dead before the branch's own control op). */
                return owning_dropped_before_control(t->as.if_.then_, bid)
                    && owning_dropped_before_control(t->as.if_.else_, bid);
            case CT_RESET:
            case CT_HANDLE:
            case CT_PERFORM:
                /* The owning value crosses into a SINGLE-SHOT continuation (the
                 * reset/handle/perform continuation runs at most once -- a second
                 * resume is a hard error, TUR-E0201).  It is now captured into
                 * that continuation's env and dropped there exactly once (or moved
                 * out), which is sound -- so the drop no longer has to precede the
                 * control op.  The capture-admission gates enforce this: an owning
                 * capture is admitted for a single-shot continuation (collect_caps)
                 * but bails for a handler CASE body (collect_caps_case, which runs
                 * per-perform), so a value consumed in a multi-run case still falls
                 * back.  A missed capture surfaces as an undeclared C name (compile
                 * error), never a silent leak/double-free. */
                return true;
            default: return false;   /* shift (abortive) / unexpected: conservative */
        }
    }
    return false;
}

/* Soundness of a CT_LETRAW node in isolation: an allocating owning op (rc/of,
 * rc/clone) must have its drop reachable before any control op (see
 * owning_dropped_before_control).  Struct/ADT ops (make-struct, get-field,
 * default-of) have no drop obligation and always pass. */
static bool letraw_ok(const CTerm *t) {
    if (owning_alloc_expr(t->as.letraw.e) && t->as.letraw.x.bind
        && !owning_dropped_before_control(t->as.letraw.body, t->as.letraw.x.bind->id))
        return false;
    return true;
}

/* Recursively check that every node lies in the C1 core subset.  Does not
 * consult the emittable set -- the cps->cps join clause is handled separately
 * by the fixpoint (needs_heap_join). */
static bool term_core_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind != KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return (slot_ok_t(t->as.letval.x.type, t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && term_core_ok(t->as.letval.body);
        case CT_LETPRIM: {
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return term_core_ok(t->as.letprim.body);
        }
        case CT_LETCALL: {
            /* The result binder may be a non-slot local (e.g. a struct/ADT read
             * only via get-field); any crossing use is gated by atom_ok below /
             * fn_sig_ok, so no slot check on the binder itself.  A CT_LETCALL is
             * always a cps->direct call to an uncolored callee, so a by-value ADT
             * arg must ride the callee's carrier ABI -- reject it (call_arg_ok
             * cps_to_direct=true) so the function evicts rather than emit a
             * raw-struct-for-int64 arg. */
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!call_arg_ok(&t->as.letcall.args[i], true)) return false;
            return term_core_ok(t->as.letcall.body);
        }
        case CT_LETRAW:
            /* An owning-value op delegated to the direct emitter.  Its operand is
             * atomic (guaranteed at translation) and its result is a local that
             * never crosses a slot, so no slot_ty check applies to the binder.
             * BUT an allocating/incrementing op (rc/of, rc/clone) creates a drop
             * obligation.  An abortive `shift` discards its continuation -- so if
             * the balancing drop is not on a straight-line path before the next
             * control op, it would be discarded (leak) or the value would abort
             * past the code that uses it (miscompile).  Require the drop up front;
             * otherwise fall back. */
            return letraw_ok(t) && term_core_ok(t->as.letraw.body);
        case CT_TAILCALL: {
            /* KK_RET / KK_VAR / KK_PROMPT are all valid here: a KK_PROMPT tail
             * call threads the enclosing prompt chain (a reset's delimited
             * body); a KK_VAR is only reifiable when the callee is not emittable
             * (handled by needs_heap_join).  A cps->direct tail call (callee not
             * CPS-emitted) passes its args RAW to the callee's direct-emitter C
             * signature, so a by-value ADT arg must ride the carrier ABI -- reject
             * it (evict).  A cps->cps tail call threads through `f__cps`, whose
             * params the CPS backend emits with the matching ABI, so the wider
             * atom_ok admits the by-value aggregate there. */
            bool cps_to_direct = !binding_in_s(t->as.tailcall.fn);
            for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                if (!call_arg_ok(&t->as.tailcall.args[i], cps_to_direct))
                    return false;
            return true;
        }
        case CT_LETCONT:
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && term_core_ok(t->as.letcont.jbody)
                && term_core_ok(t->as.letcont.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && term_core_ok(t->as.if_.then_)
                && term_core_ok(t->as.if_.else_);
        case CT_RESET: {
            /* C3/N6.3: delimited body + a self-contained continuation whose
             * captures (if any) are all scalar source vars (carried in the env). */
            CapSet cs;
            return (slot_ok_t(t->as.reset.x.type, t->as.reset.x.ty) || t->as.reset.x.ty == TY_NIL)
                && delim_ok(t->as.reset.delim)
                && reset_body_ok(t->as.reset.body)
                && collect_caps(t->as.reset.body, t->as.reset.x.id, &cs);
        }
        case CT_SHIFT: {
            /* C3/N6.3: straight-line shift body (which already applies the
             * receiver -- cps_shift_body).  The captured continuation is
             * discarded here (abortive); resume lands in C4.  The body's scalar
             * captures (if any) ride the shift-body helper's env. */
            CapSet scs;
            return shift_body_ok(t->as.shift.body)
                && collect_caps(t->as.shift.body, t->as.shift.k.id, &scs);
        }
        case CT_HANDLE: {
            /* C4/N6.2/N6.3: N-case handler; each case <=1 effect param + zero-
             * capture straight-line body; the continuation may carry scalar
             * captures in its env. */
            CapSet hcs;
            if (!(slot_ok_t(t->as.handle.x.type, t->as.handle.x.ty) || t->as.handle.x.ty == TY_NIL)
                || !term_core_ok(t->as.handle.delim)
                || !reset_body_ok(t->as.handle.body)
                || !collect_caps(t->as.handle.body, t->as.handle.x.id, &hcs))
                return false;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++) {
                const CHandleCase *c = &t->as.handle.cases[ci];
                CapSet ccs;
                /* A multi-arg effect delivers its args heap-packed as one word
                 * (emit_perform); the handler case unpacks each param from the
                 * array (emit_lifted).  Every param must be slot-representable so
                 * the pack/unpack round-trips.  A single (or zero) param keeps the
                 * original direct-in-`arg` path unchanged. */
                if (c->n_params > 1)
                    for (uint32_t pi = 0; pi < c->n_params; pi++)
                        if (!slot_ok_t(&c->params[pi]->type, c->params[pi]->type.kind))
                            return false;
                if (!handle_case_ok(c->case_body)) return false;
                if (!collect_caps_case(c->case_body, c, &ccs)) return false;
            }
            return true;
        }
        case CT_PERFORM: {
            /* C4/N6.3: straight-line continuation whose captures (if any) are all
             * scalar source vars -- lifted with a real env.  A multi-arg effect
             * (n>1) is heap-packed into one word (emit_perform) and unpacked at the
             * handler case; every arg must be a slot-representable atom.  A single
             * (or zero) arg keeps the original one-word-slot path. */
            if (!perform_body_ok(t->as.perform.body))
                return false;
            for (uint32_t i = 0; i < t->as.perform.n; i++)
                if (!atom_ok(&t->as.perform.args[i])) return false;
            CapSet cs;
            return collect_caps(t->as.perform.body, t->as.perform.x.id, &cs);
        }
        case CT_RESUME:
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && term_core_ok(t->as.resume.body);
        case CT_CLONEABLE:
            /* U3: cloneable with a named uncolored global receiver (checked at
             * translation).  Shape 1 has no context; Shape 2's operand atom
             * (n_frames >= 1) must each be slot-representable (they ride the frame
             * env).  Admission needs a slot-representable result + core body. */
            if (!(slot_ok_t(t->as.cloneable.x.type, t->as.cloneable.x.ty) || t->as.cloneable.x.ty == TY_NIL))
                return false;
            for (uint32_t i = 0; i < t->as.cloneable.n_frames; i++) {
                const CloneFrame *fr = &t->as.cloneable.frames[i];
                /* Arithmetic frames ride their operand in the frame-env slot, so it
                 * must be a slot-atom -- UNLESS it is a non-atomic pure operand on
                 * env_expr (D6a), which is emit_value'd at the reset site like a
                 * call-frame env.  A CALL frame's captured env is likewise validated
                 * and marshaled by the builder/emitter (int/cstr inline, Serializable
                 * via its instance, or a non-atomic env on env_expr), so it does not
                 * go through the operand-slot check. */
                if (!fr->call_fn && !fr->env_expr && !atom_ok(&fr->operand)) return false;
            }
            return term_core_ok(t->as.cloneable.body);
        case CT_CALLCC:
            /* U7: a local setjmp escape landing (emit_callcc).  The receiver is
             * capture-free (build_callcc) and emitted as a value; admission needs a
             * slot-representable call/cc result and a core continuation body. */
            return (slot_ok_t(t->as.callcc.x.type, t->as.callcc.x.ty) || t->as.callcc.x.ty == TY_NIL)
                && term_core_ok(t->as.callcc.body);
        default: /* CT_UNSUPPORTED */
            return false;
    }
}

/* ---- reset-delim admission (U1) ------------------------------------------
 * A reset's *delimited body* is emitted (emit_reset) with the enclosing prompt
 * chain as `cur_k`, so its tail positions deliver the delimited value to the
 * prompt (emit_deliver: `return dk_run(cur_k, v)`), and an interior join
 * (KK_VAR) delivers via goto.  That is exactly the delivery `term_core_ok`
 * forbids in *core* position (its CT_APPCONT case rejects KK_PROMPT), which is
 * why a reset body with control flow around the shift -- `(reset (if c (shift
 * ...) v))`, `(reset (+ e (if c (shift ...) v)))` -- was evicted even though
 * both the shifting and the normal-return branch are individually emittable.
 *
 * delim_ok mirrors term_core_ok but admits KK_PROMPT/KK_VAR delivery of the
 * delimited value at every tail position, recursing through the straight-line
 * (letval/letprim/letcall/letraw), branch (if), and join (letcont) structure.
 * A nested `shift` keeps its existing straight-line admission (shift_body_ok +
 * scalar-capture collect).
 *
 * CT_LETCONT (a join whose jbody may deliver to the prompt) IS admitted: it is
 * the "non-empty delimited continuation that an abortive shift discards" --
 * `(reset (+ 10 (if c (shift ...) 5)))` builds `letcont j(t){+10; prompt}` and
 * the shift branch discards j.  The CPS path lowers this correctly (the shift
 * aborts, ignoring +10); the direct/legacy path now lowers it correctly too
 * (emit_cps_reset's setjmp/longjmp escape path -- see emit_cps.c), so direct ==
 * cps holds and there is no backend divergence.
 *
 * A nested delimiter (reset/handle/perform/resume) still falls through to
 * term_core_ok, so delivering an *outer* prompt through an inner delimiter
 * stays out of scope. */
static bool delim_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* Deliver the delimited value to the enclosing prompt (KK_PROMPT,
             * the tail) or to an interior inline join (KK_VAR).  Both are
             * emitted by emit_deliver; unlike core position, KK_PROMPT here is
             * the correct delimited-body result. */
            return atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return (slot_ok_t(t->as.letval.x.type, t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && delim_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return delim_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return delim_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && delim_ok(t->as.letraw.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && delim_ok(t->as.if_.then_)
                && delim_ok(t->as.if_.else_);
        case CT_LETCONT:
            /* A join in the delim: its jbody may deliver to the prompt (a
             * discarded delimited continuation).  Recurse via delim_ok so that
             * prompt delivery is admitted in both the join body and the term. */
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && delim_ok(t->as.letcont.jbody)
                && delim_ok(t->as.letcont.body);
        case CT_TAILCALL: {
            /* A KK_PROMPT tail call threads the prompt chain (same as term_core_ok). */
            for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                if (!atom_ok(&t->as.tailcall.args[i])) return false;
            return true;
        }
        case CT_SHIFT: {
            /* An abortive shift in the delimited body: same admission as core. */
            CapSet scs;
            return shift_body_ok(t->as.shift.body)
                && collect_caps(t->as.shift.body, t->as.shift.k.id, &scs);
        }
        case CT_RESET: {
            /* U7-reset (nested-reset slice): a `reset` inside the enclosing
             * delimited body.  emit_reset installs a fresh prompt (the DK machine
             * is multi-prompt; a shift binds to the nearest enclosing prompt, so
             * the nesting is correct), threads the inner delimited body under it,
             * and lifts the inner reset's continuation as a RESET_CONT that
             * delivers the nested value onward -- in tail position, to the OUTER
             * prompt (KK_PROMPT).  That outward KK_PROMPT delivery is exactly what
             * the stricter reset_body_ok (via term_core_ok) forbids, which is why
             * a nested reset used to evict to the direct emitter.  Admit the inner
             * delimited body AND its continuation via delim_ok (both may deliver
             * to a prompt), with scalar-only captures riding the lifted env. */
            CapSet cs;
            return (slot_ok_t(t->as.reset.x.type, t->as.reset.x.ty) || t->as.reset.x.ty == TY_NIL)
                && delim_ok(t->as.reset.delim)
                && delim_ok(t->as.reset.body)
                && collect_caps(t->as.reset.body, t->as.reset.x.id, &cs);
        }
        default:
            /* Nested handle/perform/resume/unsupported: keep the stricter core
             * admission -- not relaxed by this slice. */
            return term_core_ok(t);
    }
}

/* A source parameter's raw C name (used unchanged now that fn_params is set
 * during CPS emission) must not collide with a name the CPS backend synthesizes:
 * the continuation `k`, or any `__`-prefixed internal (`__root`, `__r`, `__cap`,
 * `__h<N>`, and the fresh result temporaries `__t<N>`).  A colliding param would
 * shadow or be shadowed by a generated identifier; exclude such a function from
 * CPS candidacy so it falls back to the direct emitter (which owns its own
 * naming).  The `t<N>` branch below is retained defensively: temporaries are now
 * `__t<N>` (caught by the `__` rule), but an un-prefixed `t<N>` param remains
 * cheap to keep off the CPS path. */
static bool param_name_clashes_cps(const Binding *b) {
    if (!b || !b->name || !b->name->name) return false;
    const char *n = b->name->name;
    if (strcmp(n, "k") == 0) return true;
    if (n[0] == '_' && n[1] == '_') return true;
    if (n[0] == 't' && n[1] != '\0') {
        for (const char *p = n + 1; *p; p++) if (*p < '0' || *p > '9') return false;
        return true;   /* t followed by all digits */
    }
    return false;
}

static bool fn_sig_ok(const FnDef *fd) {
    /* Return crosses the slot (Tier A/B scalar or Tier C boxed aggregate).  A
     * nil/void return is admitted too: the body still delivers a unit (0) to the
     * return continuation, and the entry wrapper is emitted `void` (see below).
     * fn_ret_type prefers the body Type, which carries the real def a NULL-def
     * return annotation lacks.  A by-value aggregate param passes by value in C
     * directly (never rides the slot) but is admitted through the same gate -- an
     * owning-field aggregate stays out. */
    const Type *rt = fn_ret_type(fd);
    if (rt->kind != TY_NIL && !sig_slot_ok(rt, rt->kind)) return false;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        const Binding *p = fd->params[i];
        /* A poly fn param crosses as a fat closure (tur_poly_fn_t); a `^borrow`
         * param is a read-only handle the callee never consumes; a plain (non-
         * rank-2) *effect-free* `TY_FN` param crosses as the direct emitter's
         * function-pointer spelling (a `cfnptr` typedef, else the opaque `int64_t`
         * carrier).  All three are passed by value in C and are admitted even when
         * their type fails the scalar / by-value-aggregate slot gate -- emit_params
         * emits the matching C type, and a call *through* such a param is an
         * uncolored delegated indirect call (CT_LETRAW), which the direct emitter
         * emits with the same spelling.
         *
         * An *effectful* fn param (`(fn [..] #{E} ..)`) is ALSO admitted.  The key
         * realization: the fiber effect machine dispatches DYNAMICALLY (a global
         * handler chain), so invoking an effectful callback is a plain fiber call
         * whose effect propagates without any continuation threading.  Delegating
         * it is therefore SOUND -- the effect never touches the DK machine.  What
         * keeps a DK function from splitting is the whole-program taint: the effect
         * is performed by the (fiber) callback body, which taints it, so its
         * handler stays co-fiber, and any DK function that would itself perform
         * that effect on DK is evicted.  The DK higher-order function's own
         * continuation survives the fiber suspend (it lives on the fiber's C
         * stack).  Multi-shot resume through a callback is a hard error on BOTH
         * paths (TUR-E0201), so it is not a new hazard.  Verified across mixing
         * DK+fiber effects, continuation-after-callback, named + nested callbacks,
         * and the full suite. */
        /* A poly-fat (rank-2) closure param (`tur_poly_fn_t`) is a multi-word
         * aggregate.  The CPS backend threads it through one-word slots / mono
         * specialization paths that cannot hold the fat struct (`void* =
         * tur_poly_fn_t`), so a function taking one must EVICT to the direct
         * emitter (which owns the fat-closure ABI).  A plain effect-free `TY_FN`
         * param -- a bare function pointer that fits the int64 carrier -- stays
         * admitted below (the delegated indirect call the comment describes). */
        if (p->is_poly_fn) return false;
        bool fn_param_ok = p->type.kind == TY_FN;
        if (!p->is_borrow && !fn_param_ok
            && !sig_slot_ok(&p->type, p->type.kind)) return false;
        if (param_name_clashes_cps(p)) return false;
    }
    return true;
}

/* ---- whole-program classification cache ------------------------------ */

/* `eff_lo`/`eff_hi` are a 128-bit set of the effect tags this function performs
 * or handles (bit `effect_tag(sym)-2`).  `in_s` is whether the function is
 * CPS-emitted; a non-candidate (main / exported / fell-back) is kept in the
 * table with in_s=false so its effects taint any CPS peer sharing them. */
typedef struct {
    const FnDef *fd; const Binding *bind; CTerm *term; bool in_s;
    uint64_t eff_lo, eff_hi;
    /* Split of eff_* into effects this fn PERFORMS vs HANDLES, and its colored
     * call graph -- backing the call-path taint (below).  `edges` is a bitset
     * over g_ents indices of directly-called colored peers; `edges_all` marks a
     * fn that makes an indirect/over-cap call (conservatively reaches every
     * colored peer). */
    uint64_t perf_lo, perf_hi;
    uint64_t hand_lo, hand_hi;
    uint64_t *edges;   /* NULL until the call graph is built (see ensure_S) */
    bool      edges_all;
    /* G3b: a colored GENERIC template whose monomorphs are all CPS-admissible.
     * It sig-rejects itself (tyvar TY_APP) so in_s stays false and it is never
     * emitted directly, but while mono_template is true it does NOT taint its
     * effects -- its monomorphs stand in for it, and they emit DK.  The taint
     * fixpoint evicts it (mono_template=false, reverting to tainting) if its
     * effect is tainted by a genuine fiber peer. */
    bool mono_template;
} SEnt;

/* The program entry point `main` (never module-prefixed). */
static bool fn_is_main(const FnDef *fd) {
    return fd && fd->binding && fd->binding->name
        && fd->binding->name->len == 4
        && memcmp(fd->binding->name->name, "main", 4) == 0;
}

/* cps-backend-direct-lowering-removal-plan (D2b): is this a `main` that should
 * be CPS-emitted with a fixed-ABI entry wrapper?  A zero-arg `main` whose body
 * directly uses delimited control (reset/shift/shift0, cloneable/serial reset)
 * is otherwise excluded from S purely because of its entry-point ABI; admitting
 * it (subject to the same fn_sig_ok/term_core_ok subset gates) lets its delimited
 * ops emit through the native CT-IR path instead of the direct emitter, and the
 * `int main(...)` wrapper trampolines into `main__cps`.  Effect-only mains (no
 * delimited op) stay excluded -- CPS-emitting them is pure overhead and removes
 * no direct-lowering caller. */
static bool fn_is_d2b_main(const FnDef *fd) {
    return fn_is_main(fd) && fd->n_params == 0
        && fd->body && cps_expr_contains_shift(fd->body);
}

static const Expr *g_prog;      /* program the cache is keyed on */
static EmitCtx    *g_emit_ctx;  /* G3b: ctx of the active emission, for ctx->abi_specializations */
static EmitCtx    *g_ents_ctx;  /* G3b: the g_emit_ctx the cached g_ents were classified under */
static Arena       g_arena;     /* owns the cached CTerms (+ coloring) */
static bool        g_arena_live;
static SEnt       *g_ents;
static size_t      g_ents_n;
static bool        g_fwd_done;  /* __cps forward decls emitted for g_prog */

/* Effect -> prompt-tag registry.  Tags start at 2 (reset/shift use 1); the same
 * interned effect Symbol always maps to the same tag, so a handle and a matching
 * perform agree.  Reset per program. */
#define CPS_MAX_EFFECTS 128
static const Symbol *g_eff_syms[CPS_MAX_EFFECTS];
static int           g_eff_n;

static int effect_tag(const Symbol *eff) {
    for (int i = 0; i < g_eff_n; i++)
        if (g_eff_syms[i] == eff) return i + 2;
    if (g_eff_n < CPS_MAX_EFFECTS) { g_eff_syms[g_eff_n] = eff; return (g_eff_n++) + 2; }
    return 2;
}

static bool binding_in_s(const Binding *b) {
    if (!b) return false;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == b) return g_ents[i].in_s;
    return false;
}

/* True when this CT_LETCONT is a non-tail cps->cps call: its body is directly a
 * tailcall to an emittable colored callee that threads this join as the callee's
 * continuation.  Such a join is reified as a DK frame (emit_heap_join) rather
 * than an inline goto-join. */
static bool letcont_is_heap_join(const CTerm *t) {
    const CTerm *b = t->as.letcont.body;
    return b && b->kind == CT_TAILCALL
        && b->as.tailcall.kont.kind == KK_VAR
        && b->as.tailcall.kont.id == t->as.letcont.j.id
        && binding_in_s(b->as.tailcall.fn);
}

/* A heap-join jbody is lowered into a value-transform frame fn `(env, value)`
 * that RETURNS jbody's value (emit_lifted LH_PERFORM_CONT); the DK machine then
 * delivers that return through the frame's `next` (cur_k).  The frame fn thus
 * has NO continuation in scope -- neither the enclosing `k` (KK_RET) nor a join
 * var (KK_VAR) -- so a jbody that tail-calls another colored function is
 * unemittable: the lowered `g__cps(args, k)` would reference an undeclared `k`.
 * (needs_heap_join's own CT_TAILCALL case catches a KK_VAR cps->cps call, but a
 * KK_RET cps->cps call is legitimate at a function's TOP level -- where `k` is a
 * real param -- and only unemittable once buried in a lifted join body, which is
 * exactly what this scan detects.)  A nested if/let spine is walked to its tail
 * positions; control ops and plain value deliveries carry no bare cps->cps tail
 * call. */
static bool jbody_has_cps_tailcall(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_TAILCALL: return binding_in_s(t->as.tailcall.fn);
        case CT_LETVAL:   return jbody_has_cps_tailcall(t->as.letval.body);
        case CT_LETPRIM:  return jbody_has_cps_tailcall(t->as.letprim.body);
        case CT_LETCALL:  return jbody_has_cps_tailcall(t->as.letcall.body);
        case CT_LETRAW:   return jbody_has_cps_tailcall(t->as.letraw.body);
        case CT_IF:       return jbody_has_cps_tailcall(t->as.if_.then_)
                              || jbody_has_cps_tailcall(t->as.if_.else_);
        case CT_LETCONT:  return jbody_has_cps_tailcall(t->as.letcont.jbody)
                              || jbody_has_cps_tailcall(t->as.letcont.body);
        default:          return false;
    }
}

/* Does `t` contain a heap join the backend CANNOT emit?  A non-tail cps->cps
 * call reifies its join continuation as a DK frame; that is now supported for
 * the direct CT_LETCONT-body form, provided the join body is zero-capture (the
 * frame carries only the enclosing continuation).  This returns true only for
 * the cases still unhandled: a capturing heap join, a jbody that itself contains
 * a cps->cps tail call (the lifted frame fn has no `k` to thread), or a KK_VAR
 * cps->cps tail call not sitting directly under its CT_LETCONT (e.g. inside an
 * if branch). */
static bool needs_heap_join(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT: return false;
        case CT_LETVAL:  return needs_heap_join(t->as.letval.body);
        case CT_LETPRIM: return needs_heap_join(t->as.letprim.body);
        case CT_LETCALL: return needs_heap_join(t->as.letcall.body);
        case CT_LETRAW:  return needs_heap_join(t->as.letraw.body);
        case CT_TAILCALL:
            return t->as.tailcall.kont.kind == KK_VAR
                && binding_in_s(t->as.tailcall.fn);
        case CT_LETCONT:
            if (letcont_is_heap_join(t)) {
                /* The join is lifted into a DK frame; the handled tailcall (body)
                 * is not walked.  The frame carries only the enclosing
                 * continuation, so the join body must not capture other locals,
                 * nor itself contain a cps->cps tail call (the frame fn has no `k`
                 * to thread -- see jbody_has_cps_tailcall). */
                if (has_capture(t->as.letcont.jbody, t->as.letcont.param.id))
                    return true;
                if (jbody_has_cps_tailcall(t->as.letcont.jbody))
                    return true;
                return needs_heap_join(t->as.letcont.jbody);
            }
            return needs_heap_join(t->as.letcont.jbody)
                || needs_heap_join(t->as.letcont.body);
        case CT_IF:
            return needs_heap_join(t->as.if_.then_)
                || needs_heap_join(t->as.if_.else_);
        case CT_RESET:
            return needs_heap_join(t->as.reset.delim)
                || needs_heap_join(t->as.reset.body);
        case CT_SHIFT:
            return needs_heap_join(t->as.shift.body);
        case CT_HANDLE: {
            if (needs_heap_join(t->as.handle.delim) || needs_heap_join(t->as.handle.body))
                return true;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++)
                if (needs_heap_join(t->as.handle.cases[ci].case_body)) return true;
            return false;
        }
        case CT_PERFORM:
            return needs_heap_join(t->as.perform.body);
        case CT_RESUME:
            return needs_heap_join(t->as.resume.body);
        case CT_CLONEABLE:
            return needs_heap_join(t->as.cloneable.body);
        case CT_CALLCC:
            return needs_heap_join(t->as.callcc.body);
        default: return false;
    }
}

/* Mark effect `eff` in a 128-bit set (bit index = effect_tag - 2). */
static void mark_effect(const Symbol *eff, uint64_t *lo, uint64_t *hi) {
    if (!eff) return;
    int idx = effect_tag(eff) - 2;      /* >=0 */
    if (idx < 64)       *lo |= (uint64_t)1 << idx;
    else if (idx < 128) *hi |= (uint64_t)1 << (idx - 64);
}

/* Accumulator for the raw-Expr effect/call walk.  `plo/phi` collect the tags of
 * effects the expression *performs*; `hlo/hhi` collect the tags it *handles*
 * (installs a prompt for).  When `callees` is non-NULL, every direct call's
 * `fn_binding` is appended (deduped) up to `cap_callees`; an indirect call, or
 * an overflow past the cap, sets `*callee_overflow` (the caller then treats the
 * function as reaching every colored peer -- sound over-approximation).  The
 * combined "performs-or-handles" wrapper aliases plo==hlo and phi==hhi. */
typedef struct {
    uint64_t *plo, *phi;      /* performed effects */
    uint64_t *hlo, *hhi;      /* handled effects   */
    const Binding **callees;  /* optional: direct-call bindings (deduped)      */
    int       *n_callees;
    int        cap_callees;
    bool      *callee_overflow;
} EffAcc;

static void eff_acc_add_callee(EffAcc *acc, const Binding *b) {
    if (!acc->callees) return;
    if (!b) { if (acc->callee_overflow) *acc->callee_overflow = true; return; }
    for (int k = 0; k < *acc->n_callees; k++)
        if (acc->callees[k] == b) return;
    if (*acc->n_callees < acc->cap_callees) acc->callees[(*acc->n_callees)++] = b;
    else if (acc->callee_overflow) *acc->callee_overflow = true;
}

/* Walk the *source* expression `e`, recording (a) the effects it performs and
 * handles, and (b) its direct callees -- both over the raw Expr (not the CTerm).
 * The raw walk is what makes co-classification sound: a `perform` / `handle`
 * buried under a form the CPS translator does not lower (e.g. `match`) is
 * invisible in the CTerm (it collapses to CT_UNSUPPORTED) but still emits a
 * *fiber* effect op, so it must taint the effect for any CPS peer.  It also
 * covers UNCOLORED performers (a function whose only control op is hidden from
 * coloring never becomes a CPS candidate, yet still fiber-performs), which the
 * CTerm view cannot see at all.  The callee walk backs the call-path taint: an
 * intermediary that neither performs nor handles E, but conducts a call from a
 * CPS handler of E down to a CPS performer of E, is still a DK-chain link.
 *
 * Effects are dynamically scoped -- a `perform` and its `handle` must run on the
 * same machine (both DK, or both fiber) -- so this set is what the fixpoint
 * compares across every top-level function. */
static void expr_collect_effects_acc(const Expr *e, EffAcc *acc) {
    if (!e) return;
    #define REC(x) expr_collect_effects_acc((x), acc)
    switch (e->kind) {
        /* --- the effect operations themselves --- */
        case EX_PERFORM:
            if (e->as.perform_.perform) {
                mark_effect(e->as.perform_.perform->effect_name, acc->plo, acc->phi);
                for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                    REC(e->as.perform_.perform->args[i]);
            }
            return;
        case EX_HANDLE:
        case EX_HANDLER_LIT:
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                REC(h->body);
                for (uint8_t i = 0; i < h->n_cases; i++) {
                    mark_effect(h->cases[i].effect_name, acc->hlo, acc->hhi);
                    REC(h->cases[i].body);
                }
            }
            return;
        case EX_RESUME:
            if (e->as.resume_.resume) { REC(e->as.resume_.resume->k); REC(e->as.resume_.resume->value); }
            return;
        case EX_DISCONTINUE:
            if (e->as.discontinue_.discontinue) {
                REC(e->as.discontinue_.discontinue->k);
                REC(e->as.discontinue_.discontinue->exception);
            }
            return;
        case EX_WITH_HANDLER:   REC(e->as.with_handler_.handler); REC(e->as.with_handler_.body); return;
        case EX_COMPOSE_HANDLERS: REC(e->as.compose_handlers_.h1); REC(e->as.compose_handlers_.h2); return;
        /* --- structural / binding forms: recurse into every sub-expression --- */
        case EX_LET:
        case EX_LETREC:   /* reuses the let_ union member */
            for (uint32_t i = 0; i < e->as.let_.n; i++) REC(e->as.let_.bindings[i].init);
            REC(e->as.let_.body); return;
        case EX_IF:    REC(e->as.if_.cond); REC(e->as.if_.then_); REC(e->as.if_.else_or_null); return;
        case EX_DO:    for (uint32_t i = 0; i < e->as.do_.n; i++) REC(e->as.do_.items[i]); return;
        case EX_WHILE: REC(e->as.while_.cond); REC(e->as.while_.body); return;
        case EX_SET:   REC(e->as.set_.value); return;
        case EX_DEF:   REC(e->as.def_.init); return;
        case EX_BUILTIN: for (uint32_t i = 0; i < e->as.builtin.n; i++) REC(e->as.builtin.args[i]); return;
        case EX_CALL:
            /* Direct call -> record the callee; indirect call -> overflow (may
             * reach any colored peer).  fn_expr is NULL for a resolved direct
             * call, non-NULL only for the indirect/higher-order case. */
            if (e->as.call_.fn_binding) eff_acc_add_callee(acc, e->as.call_.fn_binding);
            else if (e->as.call_.fn_expr && acc->callees && acc->callee_overflow)
                *acc->callee_overflow = true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) REC(e->as.call_.args[i]);
            REC(e->as.call_.fn_expr); REC(e->as.call_.dict_arg); return;
        case EX_RETURN: REC(e->as.return_.value); return;
        case EX_MATCH:
            REC(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                REC(e->as.match_.arms[i].guard);
                REC(e->as.match_.arms[i].body);
            }
            return;
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) REC(e->as.make_struct_.field_values[i]);
            return;
        case EX_GET_FIELD: REC(e->as.get_field_.struct_expr); return;
        case EX_SET_FIELD: REC(e->as.set_field_.receiver); REC(e->as.set_field_.value); return;
        case EX_SET_DEREF: REC(e->as.set_deref_.ref); REC(e->as.set_deref_.value); return;
        /* delimited-control */
        case EX_RESET:            REC(e->as.reset_.body); return;
        case EX_SHIFT:            REC(e->as.shift_.k_fn); REC(e->as.shift_.body); return;
        case EX_SHIFT0:           REC(e->as.shift0_.k_fn); REC(e->as.shift0_.body); return;
        case EX_CALLCC:           REC(e->as.callcc_.fn); return;
        case EX_CLONEABLE_RESET:  REC(e->as.cloneable_reset_.body); return;
        case EX_CLONEABLE_SHIFT:  REC(e->as.cloneable_shift_.k_fn); REC(e->as.cloneable_shift_.body); return;
        case EX_SERIAL_RESET:     REC(e->as.serial_reset_.body); return;
        case EX_SERIAL_SHIFT:     REC(e->as.serial_shift_.k_fn); REC(e->as.serial_shift_.body); return;
        /* async / STM */
        case EX_ASYNC:      REC(e->as.async_.fn_expr); return;
        case EX_AWAIT:      REC(e->as.await_.fut_expr); return;
        case EX_ATOMICALLY: REC(e->as.atomically_.stm_expr); return;
        case EX_CHECK:      REC(e->as.check_.cond); return;
        case EX_OR_ELSE:    REC(e->as.or_else_.stm1); REC(e->as.or_else_.stm2); return;
        case EX_STM:        for (uint32_t i = 0; i < e->as.stm_.n_body; i++) REC(e->as.stm_.body[i]); return;
        case EX_TVAR_NEW:    REC(e->as.tvar_new_.init); return;
        case EX_TVAR_READ:   REC(e->as.tvar_read_.tvar); return;
        case EX_TVAR_WRITE:  REC(e->as.tvar_write_.tvar); REC(e->as.tvar_write_.value); return;
        case EX_TVAR_MODIFY: REC(e->as.tvar_modify_.tvar); REC(e->as.tvar_modify_.fn); return;
        case EX_TVAR_SWAP:   REC(e->as.tvar_swap_.tvar); REC(e->as.tvar_swap_.new_val); return;
        case EX_TVAR_CAS:    REC(e->as.tvar_cas_.tvar); REC(e->as.tvar_cas_.old_val); REC(e->as.tvar_cas_.new_val); return;
        /* single-operand wrappers */
        case EX_REF:          REC(e->as.ref_.expr); return;
        case EX_DEREF:        REC(e->as.deref_.expr); return;
        case EX_BORROW_IMMUT: REC(e->as.borrow_immut_.expr); return;
        case EX_BORROW_MUT:   REC(e->as.borrow_mut_.expr); return;
        case EX_ASCRIBE:      REC(e->as.ascribe_.inner); return;
        case EX_REINTERPRET:  REC(e->as.reinterpret_.expr); return;
        case EX_CAST:         REC(e->as.cast_.expr); return;
        case EX_FN_TO_FAT:    REC(e->as.fn_to_fat_.inner); return;
        case EX_POLY_TO_FAT:  REC(e->as.poly_to_fat_.inner); return;
        case EX_POLY_WRAP:    REC(e->as.poly_wrap_.inner); return;
        case EX_CONT_PRED:    REC(e->as.cont_pred_.expr); return;
        case EX_RC_OF:        REC(e->as.rc_of_.expr); return;
        case EX_RC_CLONE:     REC(e->as.rc_clone_.expr); return;
        case EX_RC_DROP:      REC(e->as.rc_drop_.expr); return;
        case EX_RC_PTR:       REC(e->as.rc_ptr_.expr); return;
        case EX_RC_COUNT:     REC(e->as.rc_count_.expr); return;
        case EX_RC_FROM_REF:  REC(e->as.rc_from_ref_.expr); return;
        case EX_REF_FROM_RC:  REC(e->as.ref_from_rc_.expr); return;
        case EX_WEAK:         REC(e->as.weak_.expr); return;
        case EX_WEAK_UPGRADE: REC(e->as.weak_upgrade_.expr); return;
        case EX_WEAK_PRED:    REC(e->as.weak_pred_.expr); return;
        case EX_REF_PRED:     REC(e->as.ref_pred_.expr); return;
        case EX_PANIC:        REC(e->as.panic_.payload); return;
        case EX_PANIC_WITH:   REC(e->as.panic_with_.payload); return;
        case EX_CATCH_UNWIND: REC(e->as.catch_unwind_.thunk); return;
        case EX_CATCH_PANIC_OF: REC(e->as.catch_panic_of_.thunk); return;
        case EX_UNION_INJECT: REC(e->as.union_inject_.value); return;
        case EX_ANY_TYPE_OF:  REC(e->as.any_type_of_.value); return;
        case EX_ANY_IS:       REC(e->as.any_is_.value); return;
        case EX_EXISTS_PACK:  REC(e->as.exists_pack_.value); return;
        case EX_EXISTS_OPEN:  REC(e->as.exists_open_.packed); REC(e->as.exists_open_.body); return;
        case EX_EXISTS_DISPATCH:
            for (uint32_t i = 0; i < e->as.exists_dispatch_.n_args; i++) REC(e->as.exists_dispatch_.args[i]);
            return;
        case EX_DEFER: REC(e->as.defer_.body); return;
        /* generators, dynvars, list/set literals, checked cast, cont-app */
        case EX_ANY_CAST:  REC(e->as.any_cast_.value); return;
        case EX_CONS_LIST: for (uint32_t i = 0; i < e->as.cons_list_.n; i++) REC(e->as.cons_list_.items[i]); return;
        case EX_SET_LIT:   for (uint32_t i = 0; i < e->as.set_lit_.n; i++) REC(e->as.set_lit_.items[i]); return;
        case EX_YIELD:     REC(e->as.yield_.value); return;
        case EX_GEN:       if (e->as.gen_.def) REC(e->as.gen_.def->body); return;
        case EX_GEN_NEXT:  REC(e->as.gen_next_.gen_expr); return;
        case EX_GEN_DONE:  REC(e->as.gen_done_.gen_expr); return;
        case EX_DEFDYNAMIC:   REC(e->as.defdynamic_.root_expr); return;
        case EX_DYNVAR_SET:   REC(e->as.dynvar_set_.value); return;
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
                REC(e->as.dynvar_binding_.pairs[i].override_expr);
            REC(e->as.dynvar_binding_.body); return;
        case EX_CPS_CONT_APP: REC(e->as.cps_cont_app_.cont); REC(e->as.cps_cont_app_.value); return;
        case EX_PANIC_PAYLOAD_TYPE:  REC(e->as.panic_payload_type_.payload); return;
        case EX_PANIC_PAYLOAD_VALUE: REC(e->as.panic_payload_value_.payload); return;
        case EX_PANIC_PAYLOAD_FILE:  REC(e->as.panic_payload_file_.payload); return;
        case EX_PANIC_PAYLOAD_LINE:  REC(e->as.panic_payload_line_.payload); return;
        case EX_PANIC_PAYLOAD_DOWNS: REC(e->as.panic_payload_downs_.payload); return;
        /* nested function / closure: descend into its body so a lambda that
         * performs an effect taints the effect for its enclosing function too. */
        case EX_CLOSURE:
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                REC(e->as.closure_.closure->fn->body);
            return;
        case EX_FN_DEF: if (e->as.fn_def_.fn) REC(e->as.fn_def_.fn->body); return;
        case EX_FN:     if (e->as.fn_.fn)     REC(e->as.fn_.fn->body);     return;
        case EX_SELECT:
            for (uint32_t i = 0; i < e->as.select_.n_clauses; i++) {
                REC(e->as.select_.clauses[i].chan);
                REC(e->as.select_.clauses[i].send_val);
                REC(e->as.select_.clauses[i].body);
            }
            REC(e->as.select_.default_body);
            return;
        default:
            /* Leaves (literals, var, dict, extern-c/inline-c, type/def decls) and
             * panic-payload accessors carry no effect operation. */
            return;
    }
    #undef REC
}

/* Combined "performs-or-handles" effect set (no callee collection) -- the shape
 * every existing caller uses.  Perform and handle tags fold into the same set. */
static void expr_collect_effects(const Expr *e, uint64_t *lo, uint64_t *hi) {
    EffAcc acc = { lo, hi, lo, hi, NULL, NULL, 0, NULL };
    expr_collect_effects_acc(e, &acc);
}

/* G3b: is `fd` a colored GENERIC whose EVERY monomorph spec is CPS-admissible
 * (concrete signature ok + body core)?  Enumerated from the complete spec set
 * (ctx->abi_specializations, populated by the pre-emit scan before ensure_S).
 * `term` is the generic body's CTerm; each monomorph reuses it and resolves its
 * tyvar types through that monomorph's spec.  Returns false when the generic has
 * no monomorph specs (a dead generic taints nothing anyway). */
static bool mono_template_all_admissible(EmitCtx *ctx, const FnDef *fd, CTerm *term) {
    if (!ctx || !term) return false;
    bool any = false;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->fn != fd || !spec->fn_expr || !spec->clone_name) continue;
        any = true;
        const EmitAbiSpecialization *saved = ctx->current_abi_specialization;
        ctx->current_abi_specialization = (EmitAbiSpecialization *)spec;
        g_cps_mono_resolver = ctx;
        bool ok = mono_sig_ok(fd, spec) && term_core_ok(term);
        g_cps_mono_resolver = NULL;
        ctx->current_abi_specialization = saved;
        if (!ok) return false;
    }
    return any;
}

/* Bitset helpers over g_ents indices (word = index>>6, bit = index&63). */
static inline bool ent_bit(const uint64_t *bs, size_t i) {
    return (bs[i >> 6] >> (i & 63)) & 1u;
}
static inline void ent_bit_set(uint64_t *bs, size_t i) {
    bs[i >> 6] |= (uint64_t)1 << (i & 63);
}

/* Forward transitive closure over the colored call graph: grow `set` to every
 * entry reachable from a seed via `edges` (or everything, once an `edges_all`
 * conduit is reached).  Monotone; iterates to a fixed point. */
static void reach_forward(uint64_t *set, size_t n, size_t nwords) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!ent_bit(set, i)) continue;
            if (g_ents[i].edges_all) {
                for (size_t j = 0; j < n; j++)
                    if (!ent_bit(set, j)) { ent_bit_set(set, j); changed = true; }
            } else if (g_ents[i].edges) {
                for (size_t w = 0; w < nwords; w++) {
                    uint64_t add = g_ents[i].edges[w] & ~set[w];
                    if (add) { set[w] |= add; changed = true; }
                }
            }
        }
    }
}

/* Backward transitive closure: grow `set` (seeded with the performers) to every
 * entry that can reach one of them -- an entry with a direct edge into `set`, or
 * one that calls indirectly (`edges_all`, so it reaches any performer). */
static void reach_backward(uint64_t *set, size_t n, size_t nwords) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t j = 0; j < n; j++) {
            if (ent_bit(set, j)) continue;
            bool reaches = g_ents[j].edges_all;
            if (!reaches && g_ents[j].edges)
                for (size_t w = 0; w < nwords && !reaches; w++)
                    if (g_ents[j].edges[w] & set[w]) reaches = true;
            if (reaches) { ent_bit_set(set, j); changed = true; }
        }
    }
}

static void ensure_S(const Expr *program) {
    /* Cached -- but G3b's mono-template classification needs g_emit_ctx (the
     * spec set).  If a prior run classified under a different ctx (e.g. a NULL
     * ctx from emit_cps_ir_program_has_emittable running before the first
     * try_fn), recompute so mono-templates are populated. */
    if (g_prog == program && g_ents && g_ents_ctx == g_emit_ctx) return;

    if (g_arena_live) { arena_free(&g_arena); g_arena_live = false; }
    free(g_ents); g_ents = NULL; g_ents_n = 0;
    g_prog = program;
    g_fwd_done = false;
    g_eff_n = 0;
    if (!program || program->kind != EX_PROGRAM) return;

    arena_init(&g_arena, 0);
    g_arena_live = true;

    /* Coloring is idempotent (cps.c resets first); guarantee it ran. */
    cps_color_program(&g_arena, (Expr *)program);

    /* Enter EVERY colored top-level fn into the table.  Candidates (scalar sig,
     * core-only CTerm, not main/exported) start in S; non-candidates are kept
     * with in_s=false so their effect set participates in co-classification --
     * a `perform`/`handle` in a fallback function taints the effect for every
     * CPS peer, since the two effect machines (DK vs fiber) do not interoperate.
     * Uncolored functions never contain a control op, so they touch no effect
     * and are skipped entirely. */
    uint32_t np = program->as.program.n;
    g_ents = (SEnt *)calloc(np ? np : 1, sizeof(SEnt));
    /* base_taint: effects performed/handled by any top-level code that is NEVER
     * CPS-emitted -- an uncolored function (whose sole control op is hidden from
     * coloring but still emits a fiber effect op) or a top-level def initializer.
     * These permanently taint their effects. */
    uint64_t base_lo = 0, base_hi = 0;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn) {
            FnDef *fd = it->as.fn_def_.fn;
            if (fd->cps_colored && fd->binding && fd->body) {
                /* `main` is the program entry point (int main(int, char**)); an
                 * exported binding names a fixed C symbol/linkage.  Neither takes
                 * the plain static int64_t entry-wrapper shape.  Exported bindings
                 * stay excluded; a delimited zero-arg `main` is admitted (D2b) with
                 * a fixed-ABI `int main` wrapper (emitted in emit_cps_ir_try_fn).
                 * Either way a `handle`/`perform` in an excluded fn still taints. */
                bool candidate = true;
                if (fd->binding->c_export_name) candidate = false;
                if (fn_is_main(fd) && !fn_is_d2b_main(fd)) candidate = false;
                if (candidate && !fn_sig_ok(fd)) candidate = false;
                CTerm *t = cps_ir_translate_fn(&g_arena, (Expr *)program, fd);
                if (candidate && !term_core_ok(t)) candidate = false;
                /* G3b: a colored generic sig-rejects (candidate=false) but if all
                 * its monomorphs are CPS-admissible it is a mono-template -- it
                 * stands in for its DK monomorphs and must NOT taint its effects.
                 * Only meaningful when it is not itself a direct candidate. */
                bool mono_tmpl = !candidate
                    && mono_template_all_admissible(g_emit_ctx, fd, t);
                g_ents[g_ents_n].fd = fd;
                g_ents[g_ents_n].bind = fd->binding;
                g_ents[g_ents_n].term = t;
                g_ents[g_ents_n].in_s = candidate;
                g_ents[g_ents_n].mono_template = mono_tmpl;
                SEnt *en = &g_ents[g_ents_n];
                en->perf_lo = en->perf_hi = en->hand_lo = en->hand_hi = 0;
                en->edges = NULL; en->edges_all = false;
                EffAcc acc = { &en->perf_lo, &en->perf_hi,
                               &en->hand_lo, &en->hand_hi, NULL, NULL, 0, NULL };
                expr_collect_effects_acc(fd->body, &acc);
                en->eff_lo = en->perf_lo | en->hand_lo;
                en->eff_hi = en->perf_hi | en->hand_hi;
                g_ents_n++;
                continue;
            }
            /* uncolored (or binding-less) fn: its effects are always fiber. */
            if (fd->body) expr_collect_effects(fd->body, &base_lo, &base_hi);
            continue;
        }
        /* non-fn top-level item (e.g. a def whose init performs). */
        expr_collect_effects(it, &base_lo, &base_hi);
    }

    /* Build the colored call graph backing the call-path taint.  Each entry's
     * `edges` bitset names the directly-called colored peers; `edges_all` marks
     * an entry whose body makes an indirect / over-cap call (conservatively
     * reaching every colored peer). */
    size_t nwords = (g_ents_n + 63) / 64;
    if (nwords == 0) nwords = 1;
    for (size_t i = 0; i < g_ents_n; i++) {
        g_ents[i].edges = (uint64_t *)arena_alloc(&g_arena, nwords * sizeof(uint64_t));
        memset(g_ents[i].edges, 0, nwords * sizeof(uint64_t));
        g_ents[i].edges_all = false;
        if (!g_ents[i].fd || !g_ents[i].fd->body) continue;
        enum { CALLEE_CAP = 256 };
        const Binding *cbuf[CALLEE_CAP];
        int ncb = 0; bool overflow = false;
        uint64_t scratch_lo = 0, scratch_hi = 0;
        EffAcc acc = { &scratch_lo, &scratch_hi, &scratch_lo, &scratch_hi,
                       cbuf, &ncb, CALLEE_CAP, &overflow };
        expr_collect_effects_acc(g_ents[i].fd->body, &acc);
        g_ents[i].edges_all = overflow;
        for (int k = 0; k < ncb; k++)
            for (size_t j = 0; j < g_ents_n; j++)
                if (g_ents[j].bind == cbuf[k]) { ent_bit_set(g_ents[i].edges, j); break; }
    }

    /* Precompute, per effect bit, the CALL-PATH node set: entries that lie on a
     * call path from a colored handler of the effect down to a colored performer
     * of it (forward-reachable from a handler AND backward-reachable to a
     * performer).  Fixed across the fixpoint -- only whether a node is currently a
     * fallback changes.  NULL for bits with no handler/performer pairing. */
    uint64_t *path_nodes[CPS_MAX_EFFECTS];
    for (int b = 0; b < CPS_MAX_EFFECTS; b++) {
        path_nodes[b] = NULL;
        uint64_t bl = (b < 64) ? ((uint64_t)1 << b) : 0;
        uint64_t bh = (b < 64) ? 0 : ((uint64_t)1 << (b - 64));
        bool has_h = false, has_p = false;
        for (size_t i = 0; i < g_ents_n && !(has_h && has_p); i++) {
            if ((g_ents[i].hand_lo & bl) || (g_ents[i].hand_hi & bh)) has_h = true;
            if ((g_ents[i].perf_lo & bl) || (g_ents[i].perf_hi & bh)) has_p = true;
        }
        if (!has_h || !has_p) continue;
        uint64_t *fwd = (uint64_t *)arena_alloc(&g_arena, nwords * sizeof(uint64_t));
        uint64_t *bwd = (uint64_t *)arena_alloc(&g_arena, nwords * sizeof(uint64_t));
        memset(fwd, 0, nwords * sizeof(uint64_t));
        memset(bwd, 0, nwords * sizeof(uint64_t));
        for (size_t i = 0; i < g_ents_n; i++) {
            if ((g_ents[i].hand_lo & bl) || (g_ents[i].hand_hi & bh)) ent_bit_set(fwd, i);
            if ((g_ents[i].perf_lo & bl) || (g_ents[i].perf_hi & bh)) ent_bit_set(bwd, i);
        }
        reach_forward(fwd, g_ents_n, nwords);
        reach_backward(bwd, g_ents_n, nwords);
        for (size_t w = 0; w < nwords; w++) fwd[w] &= bwd[w];  /* path = F & B */
        path_nodes[b] = fwd;
    }

    /* Fixpoint (monotone -- only removals, so it converges):
     *   Rule A: drop any candidate that needs a heap-reified join.
     *   Rule B: co-classify effects.  An effect is "tainted" if it is touched by
     *           base_taint (never-CPS code) or by any function NOT in S; evict
     *           every in-S function that touches a tainted effect, so a
     *           performer and its handler are never split across the DK and
     *           fiber machines (which do not interoperate).
     *   Rule C: call-path taint.  If any call-path node between a colored handler
     *           of E and a colored performer of E is a genuine fallback, the DK
     *           chain is severed at that cps->direct frame -- taint E so the
     *           handler + performer fall back coherently with the conduit. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < g_ents_n; i++) {
            if (g_ents[i].in_s && needs_heap_join(g_ents[i].term)) {
                g_ents[i].in_s = false;
                changed = true;
            }
        }
        /* An effect is tainted by any entry that is NEITHER an S candidate NOR a
         * mono-template: those are the genuinely fiber functions.  A mono-template
         * stands in for its DK monomorphs, so it does not taint while it holds. */
        uint64_t taint_lo = base_lo, taint_hi = base_hi;
        for (size_t i = 0; i < g_ents_n; i++) {
            if (!g_ents[i].in_s && !g_ents[i].mono_template) {
                taint_lo |= g_ents[i].eff_lo; taint_hi |= g_ents[i].eff_hi;
            }
        }
        /* Rule C: taint an effect whose handler->performer call path routes through
         * a genuine fallback intermediary (severed DK chain). */
        for (int b = 0; b < CPS_MAX_EFFECTS; b++) {
            if (!path_nodes[b]) continue;
            bool already = (b < 64) ? ((taint_lo >> b) & 1)
                                    : ((taint_hi >> (b - 64)) & 1);
            if (already) continue;
            for (size_t i = 0; i < g_ents_n; i++) {
                if (!ent_bit(path_nodes[b], i)) continue;
                if (!g_ents[i].in_s && !g_ents[i].mono_template) {
                    if (b < 64) taint_lo |= (uint64_t)1 << b;
                    else        taint_hi |= (uint64_t)1 << (b - 64);
                    break;
                }
            }
        }
        for (size_t i = 0; i < g_ents_n; i++) {
            bool tainted = (g_ents[i].eff_lo & taint_lo) || (g_ents[i].eff_hi & taint_hi);
            if (g_ents[i].in_s && tainted) {
                g_ents[i].in_s = false;
                changed = true;
            }
            /* G3b: a mono-template whose effect is tainted by a genuine fiber peer
             * can no longer stand in for DK monomorphs -- evict it so it reverts
             * to tainting (monotone: only removals). */
            if (g_ents[i].mono_template && tainted) {
                g_ents[i].mono_template = false;
                changed = true;
            }
        }
    }
    g_ents_ctx = g_emit_ctx;   /* G3b: record the ctx this classification used */
}

static SEnt *ent_of(const FnDef *fd) {
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].fd == fd) return &g_ents[i];
    return NULL;
}

static SEnt *ent_of_binding(const Binding *b) {
    if (!b) return NULL;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == b) return &g_ents[i];
    return NULL;
}

/* G3b: resolve a colored-generic callee at a specific CPS call site to its
 * MONOMORPH clone name, so a cps->cps tail call to a mono-template emits
 * `<clone>__cps` rather than the unresolved generic `<callee>__cps`.  Matches the
 * spec whose callee binding is `fn` and whose concrete arg types (C spelling)
 * equal the call's atom types.  Returns NULL when 0 or >1 specs match (fall back
 * -- never guess a wrong monomorph). */
static const char *find_mono_clone_for_call(EmitCtx *ctx, const Binding *fn,
                                            const CAtom *args, uint32_t n) {
    if (!ctx || !fn) return NULL;
    const char *hit = NULL; int n_hit = 0;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->binding != fn || !spec->clone_name || spec->n_args != n) continue;
        bool ok = true;
        for (uint32_t j = 0; j < n && ok; j++) {
            if (!args[j].type) continue;   /* untyped scalar atom: not discriminating */
            const char *ac = emit_type_c_name(ctx, *(Type *)args[j].type);
            const char *sc = emit_type_c_name(ctx, spec->arg_types[j]);
            if (!ac || !sc || strcmp(ac, sc) != 0) ok = false;
        }
        if (ok) { hit = spec->clone_name; n_hit++; }
    }
    return (n_hit == 1) ? hit : NULL;
}

/* True when `fn` names a COLORED top-level function (g_ents holds exactly the
 * colored fns).  Used by the island analysis: a call to a colored callee couples
 * this body to that callee's effect machine, so it disqualifies an island. */
static bool binding_is_colored(const Binding *fn) {
    if (!fn) return false;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == fn) return true;
    return false;
}

/* G3a: is the CTerm a self-contained DK ISLAND -- safe to CPS-emit as
 * `<clone>__cps` + entry wrapper with NO whole-program taint/routing change?
 * Sufficient (conservative) conditions:
 *   - every `perform` is lexically enclosed by a `handle` for its effect (no
 *     effect escapes to a caller -- an escaping dk_perform would search only the
 *     wrapper's fresh root and never reach the caller's handler);
 *   - no colored callee (CT_TAILCALL / CT_LETCALL to a colored fn -- that would
 *     couple this body to the callee's machine);
 *   - no raw reset/shift (delimited control that could escape a missing prompt).
 * `handled` carries the effect Symbols of the enclosing handlers in scope. */
static bool sym_in(const Symbol *s, const Symbol **set, int n) {
    for (int i = 0; i < n; i++) if (set[i] == s) return true;
    return false;
}
static bool is_cps_island(const CTerm *t, const Symbol **handled, int nh) {
    if (!t) return true;
    switch (t->kind) {
        case CT_APPCONT: return true;
        case CT_LETVAL:  return is_cps_island(t->as.letval.body, handled, nh);
        case CT_LETPRIM: return is_cps_island(t->as.letprim.body, handled, nh);
        case CT_LETRAW:  return is_cps_island(t->as.letraw.body, handled, nh);
        case CT_LETCALL:
            if (binding_is_colored(t->as.letcall.fn)) return false;
            return is_cps_island(t->as.letcall.body, handled, nh);
        case CT_TAILCALL:
            return !binding_is_colored(t->as.tailcall.fn);
        case CT_LETCONT:
            return is_cps_island(t->as.letcont.jbody, handled, nh)
                && is_cps_island(t->as.letcont.body, handled, nh);
        case CT_IF:
            return is_cps_island(t->as.if_.then_, handled, nh)
                && is_cps_island(t->as.if_.else_, handled, nh);
        case CT_HANDLE: {
            /* delim runs under this handle's effects (+ enclosing); continuation
             * and case bodies run outside them. */
            const Symbol *ext[32];
            int ne = nh;
            for (int i = 0; i < nh && i < 32; i++) ext[i] = handled[i];
            for (uint32_t ci = 0; ci < t->as.handle.n_cases && ne < 32; ci++)
                ext[ne++] = t->as.handle.cases[ci].effect;
            if (!is_cps_island(t->as.handle.delim, ext, ne)) return false;
            if (!is_cps_island(t->as.handle.body, handled, nh)) return false;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++)
                if (!is_cps_island(t->as.handle.cases[ci].case_body, handled, nh)) return false;
            return true;
        }
        case CT_PERFORM:
            if (!sym_in(t->as.perform.effect, handled, nh)) return false;  /* escapes */
            return is_cps_island(t->as.perform.body, handled, nh);
        case CT_RESUME:
            return is_cps_island(t->as.resume.body, handled, nh);
        case CT_CALLCC:
            /* A local setjmp escape performs/handles no effect (pure control), so
             * it is transparent to effect co-classification -- recurse the body
             * (matching the prior CT_LETRAW-delegated behavior). */
            return is_cps_island(t->as.callcc.body, handled, nh);
        case CT_RESET:
        case CT_SHIFT:
        default:
            return false;   /* raw delimited control / unsupported: not an island */
    }
}

/* ---- emission -------------------------------------------------------- */

#define MAX_JOINS 64

typedef struct {
    EmitCtx    *ctx;
    Buf        *out;         /* current function/helper body buffer */
    Buf        *helpers;     /* file-scope accumulator for lifted reset/shift helpers */
    int         indent;
    /* active inline joins: KK_VAR id -> join-parameter C name */
    struct { uint32_t id; const char *param; } joins[MAX_JOINS];
    int         n_joins;
    const char *cur_k;       /* C expr for the innermost prompt chain (KK_PROMPT target) */
    const char *fn_cn;       /* enclosing function's mangled name (helper naming) */
    int        *helper_ctr;  /* per-function unique-helper counter */
    bool        shift_mode;  /* true inside a shift-body / handler-case helper: KK_PROMPT -> return */
    bool        ret_mode;    /* true inside a perform-continuation frame: KK_RET -> return value */
    /* Full Type a value has when it crosses the slot at each continuation target,
     * so Tier C by-value aggregates box/unbox with their real (monomorphized) C
     * type.  ret_ty = the function's return type (KK_RET); cur_ty = the innermost
     * prompt's result type (KK_PROMPT).  NULL => scalar (Tier A/B). */
    const Type *ret_ty;
    const Type *cur_ty;
} CE;

static void ce_line(CE *ce, const char *fmt, ...) {
    indent_buf(ce->out, ce->indent);
    va_list ap; va_start(ap, fmt);
    buf_vprintf(ce->out, fmt, ap);
    va_end(ap);
    buf_putc(ce->out, '\n');
}

static const char *join_param(CE *ce, uint32_t id) {
    for (int i = ce->n_joins - 1; i >= 0; i--)
        if (ce->joins[i].id == id) return ce->joins[i].param;
    return "0";  /* unreachable when the term is well-formed */
}

/* Materialize an atom as a malloc'd C expression string. */
static char *atom_str(CE *ce, const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return atom_int_typed(a->i, a->ty);  /* real width -> matching *_C() suffix */
        case CA_BOOL: return atom_bool(a->b);
        case CA_UNIT: return strdup("0");
        case CA_STR:  return atom_cstr(a->str);
        case CA_FLOAT: return a->ty == TY_FLOAT32 ? atom_float32(a->f) : atom_float(a->f);
        case CA_VAR:  return atom_var(ce->ctx, a->var);
        case CA_CVAR: return strdup(a->cvar_name ? a->cvar_name : "0");
        default:      return strdup("0");
    }
}

/* Build the RHS C expression for a supported non-println builtin. */
static char *prim_expr(const BuiltinSpec *sp, char **as, uint32_t n) {
    Buf b; buf_init(&b);
    switch (sp->shape) {
        case BS_BIN_INFIX:
            buf_printf(&b, "(%s) %s (%s)", as[0], sp->c_op, as[1]);
            break;
        case BS_VARIADIC_FOLD: {
            uint32_t opens = (n >= 2) ? n - 2 : 0;
            for (uint32_t i = 0; i < opens; i++) buf_putc(&b, '(');
            buf_printf(&b, "(%s)", as[0]);
            for (uint32_t i = 1; i < n; i++) {
                buf_printf(&b, " %s (%s)", sp->c_op, as[i]);
                if (i < n - 1) buf_putc(&b, ')');
            }
            break;
        }
        case BS_DIV_CHECK:
            buf_printf(&b,
                "((%s) ? ((%s) / (%s)) : "
                "(fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                as[1], as[0], as[1]);
            break;
        case BS_PREFIX_UNARY:
            buf_printf(&b, "%s(%s)", sp->c_op, as[0]);
            break;
        case BS_AND_SC:
        case BS_OR_SC: {
            const char *op = (sp->shape == BS_AND_SC) ? "&&" : "||";
            for (uint32_t i = 0; i < n; i++) {
                if (i) buf_printf(&b, " %s ", op);
                buf_printf(&b, "(%s)", as[i]);
            }
            if (n == 0) buf_puts(&b, (sp->shape == BS_AND_SC) ? "1" : "0");
            break;
        }
        case BS_FUNC_CALL: {
            /* A plain C runtime call `c_op(args)` -- e.g. cons, the continuation
             * resume/clone/drop/serialize primitives.  Args are slot carriers. */
            buf_printf(&b, "%s(", sp->c_op);
            for (uint32_t i = 0; i < n; i++) {
                if (i) buf_puts(&b, ", ");
                buf_printf(&b, "(%s)", as[i]);
            }
            buf_putc(&b, ')');
            break;
        }
        default:
            buf_puts(&b, "0");
            break;
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

static char *callee_name(const Binding *fn) {
    return raw_name_for_binding(fn);   /* malloc'd */
}

/* Join a term's atom arguments into a malloc'd "a0, a1, ..." string. */
static char *atoms_csv(CE *ce, const CAtom *args, uint32_t n) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        char *a = atom_str(ce, &args[i]);
        buf_puts(&b, a);
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

static void emit_term(CE *ce, const CTerm *t);
static void emit_binder_decls(CE *ce, const CTerm *t);
static void emit_reset(CE *ce, const CTerm *t);
static void emit_shift(CE *ce, const CTerm *t);
static void emit_cloneable(CE *ce, const CTerm *t);
static void emit_handle(CE *ce, const CTerm *t);
static void emit_perform(CE *ce, const CTerm *t);
static void emit_resume(CE *ce, const CTerm *t);
static void emit_letraw(CE *ce, const CTerm *t);
static void emit_callcc(CE *ce, const CTerm *t);
static void emit_heap_join(CE *ce, const CTerm *t);
static char *cvar_cname(CE *ce, CVar x);

/* The full Type a value has when delivered to continuation `kont` (the target's
 * result type), or NULL for a scalar / inline join.  Used as a fallback when the
 * delivered value's own Type is not in hand (a cps->direct tail result). */
static const Type *deliver_ty(CE *ce, const CKont *kont) {
    if (kont->kind == KK_RET)    return ce->ret_ty;
    if (kont->kind == KK_PROMPT) return ce->cur_ty;
    return NULL;   /* KK_VAR: inline join, a plain C-local assignment */
}

/* Deliver value-string `v` to continuation `kont`.  A Tier C by-value aggregate
 * is boxed into the slot on the way out.  `explicit_vty` is the delivered value's
 * own Type when the caller has it (a CT_APPCONT atom) -- it is the most precise
 * source (e.g. a cross-function shift delivers to a prompt whose result type the
 * enclosing function's cur_ty does not carry); NULL falls back to deliver_ty. */
static void emit_deliver_ty(CE *ce, const CKont *kont, const char *v, const Type *explicit_vty) {
    const Type *vty = explicit_vty ? explicit_vty : deliver_ty(ce, kont);
    if (kont->kind == KK_RET) {
        /* In a perform-continuation frame the delivered value IS the result --
         * return it (dk_perform routes it to the handler's outer continuation);
         * otherwise run the function's return continuation. */
        char *sv = slot_store(ce->ctx, kont->ty, vty, v);
        if (ce->ret_mode)
            ce_line(ce, "return %s;", sv);
        else
            ce_line(ce, "return dk_run(__kont, %s);", sv);
        free(sv);
    } else if (kont->kind == KK_PROMPT) {
        /* Deliver to the innermost prompt.  In a shift-body helper the delivered
         * value IS the shift result -- return it; otherwise run the prompt chain. */
        char *sv = slot_store(ce->ctx, kont->ty, vty, v);
        if (ce->shift_mode)
            ce_line(ce, "return %s;", sv);
        else
            ce_line(ce, "return dk_run(%s, %s);", ce->cur_k, sv);
        free(sv);
    } else { /* KK_VAR: an inline join */
        ce_line(ce, "%s = %s;", join_param(ce, kont->id), v);
        ce_line(ce, "goto L%u;", kont->id);
    }
}

/* Deliver with no explicit value Type (the crossing type comes from the
 * continuation target -- ret_ty / cur_ty). */
static void emit_deliver(CE *ce, const CKont *kont, const char *v) {
    emit_deliver_ty(ce, kont, v, NULL);
}

static void emit_println(CE *ce, BuiltinShape shape, const char *arg) {
    switch (shape) {
        case BS_PRINTLN_INT:
            ce_line(ce, "printf(\"%%lld\\n\", (long long)(%s));", arg);
            break;
        case BS_PRINTLN_UINT:
            ce_line(ce, "printf(\"%%llu\\n\", (unsigned long long)(%s));", arg);
            break;
        case BS_PRINTLN_BOOL:
            ce_line(ce, "puts((%s) ? \"true\" : \"false\");", arg);
            break;
        case BS_PRINTLN_CSTR:
            ce_line(ce, "puts(%s);", arg);
            break;
        case BS_PRINTLN_FLOAT:
            ce_line(ce, "printf(\"%%g\\n\", (double)(%s));", arg);
            break;
        case BS_PRINTLN_FLOAT32:
            ce_line(ce, "printf(\"%%.7g\\n\", (double)(%s));", arg);
            break;
        default: break;
    }
}

static void emit_term(CE *ce, const CTerm *t) {
    switch (t->kind) {
        case CT_APPCONT: {
            char *v = atom_str(ce, &t->as.appcont.v);
            /* The delivered atom carries its own Type -- the most precise source
             * for Tier C boxing (a cross-function shift delivers to a prompt whose
             * result type the enclosing cur_ty does not know). */
            emit_deliver_ty(ce, &t->as.appcont.kont, v, t->as.appcont.v.type);
            free(v);
            break;
        }
        case CT_LETVAL: {
            char *v = atom_str(ce, &t->as.letval.v);
            char *bn = cvar_cname(ce, t->as.letval.x);
            ce_line(ce, "%s = %s;", bn, v);
            free(bn); free(v);
            emit_term(ce, t->as.letval.body);
            break;
        }
        case CT_LETPRIM: {
            uint32_t n = t->as.letprim.n;
            char **as = (char **)calloc(n ? n : 1, sizeof(char *));
            for (uint32_t i = 0; i < n; i++) as[i] = atom_str(ce, &t->as.letprim.args[i]);
            const BuiltinSpec *sp = t->as.letprim.spec;
            char *bn = cvar_cname(ce, t->as.letprim.x);
            if (is_println_shape(sp->shape)) {
                emit_println(ce, sp->shape, n ? as[0] : "0");
                ce_line(ce, "%s = 0;", bn);  /* nil result */
            } else {
                char *rhs = prim_expr(sp, as, n);
                ce_line(ce, "%s = %s;", bn, rhs);
                free(rhs);
            }
            free(bn);
            for (uint32_t i = 0; i < n; i++) free(as[i]);
            free(as);
            emit_term(ce, t->as.letprim.body);
            break;
        }
        case CT_LETCALL: {
            /* cps->direct: an uncolored callee runs to completion and returns an
             * ordinary value; no continuation is threaded in. */
            char *fn = callee_name(t->as.letcall.fn);
            char *argv = atoms_csv(ce, t->as.letcall.args, t->as.letcall.n);
            char *bn = cvar_cname(ce, t->as.letcall.x);
            ce_line(ce, "%s = %s(%s); /* cps->direct */", bn, fn, argv);
            free(bn); free(fn); free(argv);
            emit_term(ce, t->as.letcall.body);
            break;
        }
        case CT_TAILCALL: {
            /* G3b: a colored-generic mono-template callee -- resolve this call
             * site to the monomorph clone so we thread `<clone>__cps` (the DK
             * monomorph), not the unresolved generic name. */
            const char *mclone = NULL;
            SEnt *fe = ent_of_binding(t->as.tailcall.fn);
            if (fe && fe->mono_template)
                mclone = find_mono_clone_for_call(ce->ctx, t->as.tailcall.fn,
                                                  t->as.tailcall.args, t->as.tailcall.n);
            char *fn = mclone ? strdup(mclone) : callee_name(t->as.tailcall.fn);
            char *argv = atoms_csv(ce, t->as.tailcall.args, t->as.tailcall.n);
            if (binding_in_s(t->as.tailcall.fn) || mclone) {
                /* cps->cps: both colored and emitted -- thread the continuation
                 * straight through, no trampoline.  The threaded continuation is
                 * the function's own k (KK_RET) or, inside a reset's delimited
                 * body, the enclosing prompt chain (KK_PROMPT).  A KK_VAR here
                 * would need a heap join, which excludes the caller. */
                const char *thread = (t->as.tailcall.kont.kind == KK_PROMPT)
                    ? ce->cur_k : "__kont";
                if (t->as.tailcall.n)
                    ce_line(ce, "return %s__cps(%s, %s); /* cps->cps */", fn, argv, thread);
                else
                    ce_line(ce, "return %s__cps(%s); /* cps->cps */", fn, thread);
            } else {
                /* cps->direct: the callee is uncolored or a colored function that
                 * fell back to direct-style; call it synchronously and deliver
                 * the ordinary result value to the continuation. */
                char *tmp = fresh_tmp(ce->ctx);
                /* __auto_type keeps the callee's real return type (int, cstr,
                 * ...); the slot cast at delivery narrows it to the word. */
                ce_line(ce, "__auto_type %s = %s(%s); /* cps->direct */", tmp, fn, argv);
                emit_deliver(ce, &t->as.tailcall.kont, tmp);
                free(tmp);
            }
            free(fn); free(argv);
            break;
        }
        case CT_LETCONT: {
            if (letcont_is_heap_join(t)) { emit_heap_join(ce, t); break; }
            /* Name the join-param SLOT the way every reference to it is named
             * (cvar_cname -> name_for_binding when the param carries a source
             * Binding, so a kebab-case `let` binder like `first-results` mangles
             * to `first_hyresults_<id>` at the decl, the delivery, AND the join
             * body's uses).  Using the raw `param.name` here desynced the slot
             * (`first-results`, an invalid C identifier) from the body's mangled
             * atom references.  Must match emit_binder_decls' CT_LETCONT decl. */
            char *pn = cvar_cname(ce, t->as.letcont.param);
            bool pushed = ce->n_joins < MAX_JOINS;
            if (pushed) {
                ce->joins[ce->n_joins].id = t->as.letcont.j.id;
                ce->joins[ce->n_joins].param = pn;
                ce->n_joins++;
            }
            emit_term(ce, t->as.letcont.body);
            if (pushed && ce->n_joins > 0) ce->n_joins--;
            /* the join landing pad */
            indent_buf(ce->out, ce->indent);
            buf_printf(ce->out, "L%u:;\n", t->as.letcont.j.id);
            emit_term(ce, t->as.letcont.jbody);
            free(pn);
            break;
        }
        case CT_IF: {
            char *c = atom_str(ce, &t->as.if_.cond);
            ce_line(ce, "if (%s) {", c);
            free(c);
            ce->indent += 4;
            emit_term(ce, t->as.if_.then_);
            ce->indent -= 4;
            ce_line(ce, "} else {");
            ce->indent += 4;
            emit_term(ce, t->as.if_.else_);
            ce->indent -= 4;
            ce_line(ce, "}");
            break;
        }
        case CT_RESET:   emit_reset(ce, t); break;
        case CT_SHIFT:   emit_shift(ce, t); break;
        case CT_HANDLE:  emit_handle(ce, t); break;
        case CT_PERFORM: emit_perform(ce, t); break;
        case CT_RESUME:  emit_resume(ce, t); break;
        case CT_LETRAW:  emit_letraw(ce, t); break;
        case CT_CLONEABLE: emit_cloneable(ce, t); break;
        case CT_CALLCC:  emit_callcc(ce, t); break;
        default:
            ce_line(ce, "abort(); /* cps-backend: unreachable */");
            break;
    }
}

/* Emit an owning-value op (rc/of, rc/drop, ...) by delegating to the direct
 * emitter.  The owning value is a local that never crosses a DK slot, so reusing
 * emit_value gives it the exact control-block / refcount / drop-glue discipline
 * the direct path uses -- no duplication, correct by construction. */
/* Name a CVar the way every reference site names it: through name_for_binding
 * when it stands for a source Binding, else its fresh cvar name.  Malloc'd. */
static char *cvar_cname(CE *ce, CVar x) {
    return x.bind ? name_for_binding(ce->ctx, x.bind) : strdup(x.name);
}

/* Name a CT_LETRAW binder consistently (see cvar_cname). */
static char *letraw_binder_name(CE *ce, const CTerm *t) {
    return cvar_cname(ce, t->as.letraw.x);
}

static void emit_letraw(CE *ce, const CTerm *t) {
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;           /* line the delegated statements up */
    char *rhs = emit_value(ce->ctx, ce->out, t->as.letraw.e);
    ce->ctx->indent = saved;
    char *bn = letraw_binder_name(ce, t);
    /* A nil-typed op yields a void/nil expression -- bind the unit placeholder
     * rather than assigning a void value.  But a nil/void EX_CALL is a special
     * case: emit_value returns the call as an UN-emitted expression (a void
     * result is not hoisted into a temp -- see emit_value), so its side effect is
     * only realized if we emit it as a statement here.  Other nil ops (rc/drop,
     * set!, while, ...) already emitted their statements and return a unit
     * placeholder, so they must NOT be re-emitted (that would double the effect). */
    if (t->as.letraw.x.ty == TY_NIL) {
        const Expr *le = t->as.letraw.e;
        while (le && le->kind == EX_ASCRIBE) le = le->as.ascribe_.inner;
        if (le && le->kind == EX_CALL && rhs && rhs[0])
            ce_line(ce, "%s;", rhs);
        ce_line(ce, "%s = 0;", bn);
    } else
        ce_line(ce, "%s = %s;", bn, rhs ? rhs : "0");
    free(bn);
    free(rhs);
    emit_term(ce, t->as.letraw.body);
}

/* ---- binder pre-declaration ------------------------------------------ *
 * Every let-bound C variable is declared (uninitialized) at the top of the
 * function so that forward `goto`s into a join never jump past a declaration.
 * All values thread as machine scalars, so int64_t is the uniform storage. */
static void emit_binder_decls(CE *ce, const CTerm *t) {
    if (!t) return;
    switch (t->kind) {
        case CT_APPCONT: break;
        case CT_LETVAL: {
            char *bn = cvar_cname(ce, t->as.letval.x);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.letval.x.ty, t->as.letval.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.letval.body);
            break;
        }
        case CT_LETPRIM: {
            char *bn = cvar_cname(ce, t->as.letprim.x);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.letprim.x.ty, t->as.letprim.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.letprim.body);
            break;
        }
        case CT_LETCALL: {
            char *bn = cvar_cname(ce, t->as.letcall.x);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.letcall.x.ty, t->as.letcall.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.letcall.body);
            break;
        }
        case CT_LETRAW: {
            char *bn = t->as.letraw.x.bind
                ? name_for_binding(ce->ctx, t->as.letraw.x.bind)
                : strdup(t->as.letraw.x.name);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.letraw.x.ty, t->as.letraw.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.letraw.body);
            break;
        }
        case CT_TAILCALL: break;
        case CT_LETCONT: {
            if (letcont_is_heap_join(t)) break;  /* param + jbody are lifted into a frame helper */
            /* Name the slot via cvar_cname so a source-Binding param mangles
             * consistently with the delivery + the join body's references (see
             * the CT_LETCONT emit in emit_term); the raw param.name would be an
             * invalid C identifier for a kebab-case `let` binder. */
            char *pn = cvar_cname(ce, t->as.letcont.param);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.letcont.param.ty, t->as.letcont.param.type), pn);
            free(pn);
            emit_binder_decls(ce, t->as.letcont.body);
            emit_binder_decls(ce, t->as.letcont.jbody);
            break;
        }
        case CT_IF:
            emit_binder_decls(ce, t->as.if_.then_);
            emit_binder_decls(ce, t->as.if_.else_);
            break;
        case CT_RESET:
            /* Only the delimited body lives in this function; reset.body is
             * lifted into a helper with its own decls. */
            emit_binder_decls(ce, t->as.reset.delim);
            break;
        case CT_SHIFT: break;   /* shift.body is lifted into a helper */
        case CT_HANDLE:
            /* Only the handled body lives here; the continuation and case are
             * lifted into helpers. */
            emit_binder_decls(ce, t->as.handle.delim);
            break;
        case CT_PERFORM: break;   /* terminal; the continuation is lifted */
        case CT_RESUME: {
            char *bn = cvar_cname(ce, t->as.resume.x);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.resume.x.ty, t->as.resume.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.resume.body);
            break;
        }
        case CT_CLONEABLE: {
            char *bn = cvar_cname(ce, t->as.cloneable.x);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.cloneable.x.ty, t->as.cloneable.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.cloneable.body);
            break;
        }
        case CT_CALLCC: {
            char *bn = cvar_cname(ce, t->as.callcc.x);
            ce_line(ce, "%s %s;", binder_ctype_full(ce->ctx, t->as.callcc.x.ty, t->as.callcc.x.type), bn);
            free(bn);
            emit_binder_decls(ce, t->as.callcc.body);
            break;
        }
        default: break;
    }
}

/* ---- C3: lifted reset/shift helpers ---------------------------------- *
 * A reset's continuation and a shift's body are reified as file-scope helper
 * functions (matching the DKFrame / DKBody signatures) so the DK machine can
 * invoke them.  The zero-capture cut passes the enclosing continuation `k`
 * directly as the frame env; a body needing to capture other locals is rejected
 * at classification time (has_capture) and falls back. */

/* The shape of a lifted helper. */
typedef enum {
    LH_RESET_CONT,    /* DKFrame (env=k, xval): reset continuation, KK_RET -> dk_run(k,..) */
    LH_SHIFT_BODY,    /* DKBody  (env, subk):   shift body, KK_PROMPT -> return value */
    LH_PERFORM_CONT,  /* DKFrame (env, xval):   perform continuation, KK_RET -> return value */
    LH_HANDLER_CASE,  /* DKHandler (env, arg, subk): binds params+k, KK_PROMPT -> return */
} LHMode;

/* Emit one lifted helper into ce->helpers.  `xname` is the incoming value
 * parameter name (reset/perform continuation result var); `xt` its full Type
 * (may be NULL for a scalar) so a Tier C aggregate value-param unboxes.  `hcase`
 * is the handler clause (for LH_HANDLER_CASE param/k binding), else NULL.
 * `caps` (N6.3) is the set of scalar source captures this body needs; when
 * non-empty (LH_PERFORM_CONT only, for now) the `env` slot carries a heap struct
 * of those values, which the helper reads back into real-typed locals. */
static void emit_lifted(CE *ce, const char *name, LHMode mode,
                        const char *xname, TypeKind xty, const Type *xt,
                        const CTerm *body, const CHandleCase *hcase,
                        const CapSet *caps) {
    bool has_caps = (caps && caps->n > 0);
    /* Emit the body into a temporary buffer first, so any nested reset/shift/
     * effect appends its own (inner) helpers ahead of this one in ce->helpers. */
    Buf tmp; buf_init(&tmp);
    CE hc = *ce;
    hc.out = &tmp;
    hc.indent = 4;
    hc.n_joins = 0;
    hc.cur_k = "__kont";
    /* A perform continuation returns its value however it is delivered (KK_RET
     * or KK_PROMPT), so it sets both return modes. */
    hc.shift_mode = (mode == LH_SHIFT_BODY || mode == LH_HANDLER_CASE || mode == LH_PERFORM_CONT);
    hc.ret_mode   = (mode == LH_PERFORM_CONT);

    /* N6.3: read the captured values out of the env struct into locals named the
     * same way the body references them (name_for_binding).  A reset/handle
     * continuation's env also carries the enclosing continuation `k`. */
    if (has_caps) {
        indent_buf(&tmp, 4);
        buf_printf(&tmp, "%s_env *__cap = (%s_env *)(intptr_t)env;\n", name, name);
        if (mode == LH_RESET_CONT) {
            indent_buf(&tmp, 4);
            buf_puts(&tmp, "DK *__kont = __cap->__k;\n");
        }
        for (int i = 0; i < caps->n; i++) {
            char *cn = caps->b[i] ? name_for_binding(ce->ctx, caps->b[i]) : strdup(caps->cvname[i]);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "%s %s = __cap->f%d;\n", cap_ctype(ce->ctx, caps, i), cn, i);
            free(cn);
        }
    }

    /* Load the incoming value out of the one-word slot into a real-typed local
     * (reset/perform continuation), or bind the handler case's params/k.  The
     * value-param load leaks a Tier C box (consume=false): this frame can run
     * more than once under a multi-shot resume. */
    if (mode == LH_RESET_CONT || mode == LH_PERFORM_CONT) {
        char slotexpr[160];
        snprintf(slotexpr, sizeof slotexpr, "%s__slot", xname);
        char *ld = slot_load(ce->ctx, xty, xt, slotexpr, false);
        indent_buf(&tmp, 4);
        buf_printf(&tmp, "%s %s = %s;\n", binder_ctype_full(ce->ctx, xty, xt), xname, ld);
        free(ld);
    }
    if (mode == LH_HANDLER_CASE && hcase) {
        if (hcase->n_params == 1) {
            const Binding *pb = hcase->params[0];
            char *pn = name_for_binding(ce->ctx, pb);
            const char *pty = binder_ctype_full(ce->ctx, pb->type.kind, &pb->type);
            char *ld = slot_load(ce->ctx, pb->type.kind, &pb->type, "arg", false);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "%s %s = %s;\n", pty, pn, ld);
            free(ld);
            free(pn);
        } else if (hcase->n_params > 1) {
            /* Multi-arg effect: `arg` is a pointer to the heap-packed
             * `int64_t[n]` slot-word array built at the perform site; unpack each
             * param from its slot (value-load, consume=false -- a multi-shot
             * resume may re-read the array). */
            indent_buf(&tmp, 4);
            buf_puts(&tmp, "int64_t *__eargs = (int64_t *)arg;\n");
            for (uint32_t pi = 0; pi < hcase->n_params; pi++) {
                const Binding *pb = hcase->params[pi];
                char *pn = name_for_binding(ce->ctx, pb);
                const char *pty = binder_ctype_full(ce->ctx, pb->type.kind, &pb->type);
                char src[32];
                snprintf(src, sizeof src, "__eargs[%u]", pi);
                char *ld = slot_load(ce->ctx, pb->type.kind, &pb->type, src, false);
                indent_buf(&tmp, 4);
                buf_printf(&tmp, "%s %s = %s;\n", pty, pn, ld);
                free(ld);
                free(pn);
            }
        }
        if (hcase->k) {
            char *kn = name_for_binding(ce->ctx, hcase->k);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "DK *%s = subk;\n", kn);
            free(kn);
        }
    }
    emit_binder_decls(&hc, body);
    emit_term(&hc, body);
    buf_putc(&tmp, '\0');

    /* N6.3: the env struct type (named <name>_env) shared with the alloc site.
     * A reset/handle continuation env leads with the enclosing continuation k. */
    if (has_caps) {
        buf_printf(ce->helpers, "typedef struct {");
        if (mode == LH_RESET_CONT) buf_puts(ce->helpers, " DK *__k;");
        for (int i = 0; i < caps->n; i++)
            buf_printf(ce->helpers, " %s f%d;", cap_ctype(ce->ctx, caps, i), i);
        buf_printf(ce->helpers, " } %s_env;\n", name);
    }

    switch (mode) {
        case LH_SHIFT_BODY:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, DK *subk) {\n", name);
            if (has_caps) buf_puts(ce->helpers, "    (void)subk;\n");
            else          buf_puts(ce->helpers, "    (void)env; (void)subk;\n");
            break;
        case LH_HANDLER_CASE:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t arg, DK *subk) {\n", name);
            if (has_caps) buf_puts(ce->helpers, "    (void)arg; (void)subk;\n");
            else          buf_puts(ce->helpers, "    (void)env; (void)arg; (void)subk;\n");
            break;
        case LH_PERFORM_CONT:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t %s__slot) {\n", name, xname);
            if (!has_caps) buf_puts(ce->helpers, "    (void)env;\n");
            break;
        case LH_RESET_CONT:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t %s__slot) {\n", name, xname);
            /* With caps, `k` is read from the env struct (above); else env IS k. */
            if (!has_caps) buf_puts(ce->helpers, "    DK *__kont = (DK *)env;\n");
            break;
    }
    buf_puts(ce->helpers, tmp.data);
    buf_puts(ce->helpers, "}\n");
    buf_free(&tmp);
}

/* A non-tail cps->cps call `let x = g(args) in jbody`, represented as a
 * CT_LETCONT whose body tailcalls g threading this join.  The join continuation
 * (jbody, parameterized by x) is reified as a DK frame chained onto the enclosing
 * continuation (`cur_k`), and g is called with that chain.  When g finishes it
 * `dk_run`s its result through the chain: the join frame transforms it (runs
 * jbody, returning jbody's value) and dk_run then delivers that to `cur_k`.
 *
 * The frame is a value-transform frame (LH_PERFORM_CONT: it *returns* jbody's
 * value rather than delivering it itself), and its `next` is `cur_k` -- so the
 * enclosing continuation, including any effect handler installed above this call,
 * stays reachable in g's continuation chain.  (A reset-cont-style frame with
 * next=dk_done would hide the handler from g and abort on a perform.)  The join
 * body is zero-capture (checked by needs_heap_join), so the frame carries no env. */
static void emit_heap_join(CE *ce, const CTerm *t) {
    const CTerm *call = t->as.letcont.body;   /* CT_TAILCALL to the colored callee */
    int id = (*ce->helper_ctr)++;
    char jname[256];
    snprintf(jname, sizeof(jname), "%s_j%d", ce->fn_cn, id);
    char *xn = cvar_cname(ce, t->as.letcont.param);
    emit_lifted(ce, jname, LH_PERFORM_CONT, xn, t->as.letcont.param.ty,
                t->as.letcont.param.type, t->as.letcont.jbody, NULL, NULL);
    free(xn);

    char *fn = callee_name(call->as.tailcall.fn);
    char *argv = atoms_csv(ce, call->as.tailcall.args, call->as.tailcall.n);
    char frame[512];
    snprintf(frame, sizeof frame, "dk_frame(%s, 0, %s)", jname, ce->cur_k);
    if (call->as.tailcall.n)
        ce_line(ce, "return %s__cps(%s, %s); /* cps->cps heap join */", fn, argv, frame);
    else
        ce_line(ce, "return %s__cps(%s); /* cps->cps heap join */", fn, frame);
    free(fn); free(argv);
}

/* N6.3: emit the alloc+populate of a lifted continuation's env (the body's
 * scalar captures, plus the enclosing continuation `k` when `k_expr` != NULL --
 * reset/handle continuations carry k in the env; a handler case gets k via subk,
 * so it passes k_expr = NULL and its env is caps-only).  Returns a malloc'd C
 * expr for the DK frame env: the env struct pointer when there are captures, else
 * plain (intptr_t)k (or 0 when k-less).  The struct is leaked with the DK nodes. */
static char *emit_cont_env(CE *ce, const char *hname, const CapSet *caps, const char *k_expr) {
    if (!caps || caps->n == 0) {
        Buf b; buf_init(&b);
        if (k_expr) buf_printf(&b, "(intptr_t)%s", k_expr); else buf_puts(&b, "0");
        buf_putc(&b, '\0');
        char *s = strdup(b.data); buf_free(&b); return s;
    }
    char envv[300];
    snprintf(envv, sizeof envv, "__ce_%s", hname);
    ce_line(ce, "%s_env *%s = (%s_env *)malloc(sizeof(%s_env));", hname, envv, hname, hname);
    if (k_expr) ce_line(ce, "%s->__k = %s;", envv, k_expr);
    for (int i = 0; i < caps->n; i++) {
        char *cn = caps->b[i] ? name_for_binding(ce->ctx, caps->b[i]) : strdup(caps->cvname[i]);
        ce_line(ce, "%s->f%d = %s;", envv, i, cn);
        free(cn);
    }
    Buf b; buf_init(&b); buf_printf(&b, "(intptr_t)%s", envv); buf_putc(&b, '\0');
    char *s = strdup(b.data); buf_free(&b); return s;
}

/* CT_RESET: install a prompt whose outer continuation is the lifted reset body
 * (carrying k + captures), then emit the delimited body threading that prompt
 * chain.  The per-reset DK nodes are leaked (DK is opaque; see
 * docs/reported/cps-delimited-dk-node-leak.md), matching the abortive path. */
static void emit_reset(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char hname[256];
    snprintf(hname, sizeof(hname), "%s_k%d", ce->fn_cn, id);
    CapSet cs;
    bool ok = collect_caps(t->as.reset.body, t->as.reset.x.id, &cs);
    const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
    char *rxn = cvar_cname(ce, t->as.reset.x);
    emit_lifted(ce, hname, LH_RESET_CONT, rxn, t->as.reset.x.ty, t->as.reset.x.type,
                t->as.reset.body, NULL, caps);
    free(rxn);

    char *envexpr = emit_cont_env(ce, hname, caps, ce->cur_k);
    char pchain[64];
    snprintf(pchain, sizeof(pchain), "__p%d", id);
    ce_line(ce, "DK *%s = dk_prompt(1, dk_frame(%s, %s, dk_done()));",
            pchain, hname, envexpr);
    free(envexpr);

    const char *save = ce->cur_k;
    const Type *save_ty = ce->cur_ty;
    ce->cur_k = arena_strdup(&g_arena, pchain, strlen(pchain));
    ce->cur_ty = t->as.reset.x.type;    /* KK_PROMPT crossing type = reset result */
    emit_term(ce, t->as.reset.delim);   /* delivers to the prompt chain, tail */
    ce->cur_k = save;
    ce->cur_ty = save_ty;
}

/* The C body expression of a single arithmetic context frame `(<op> <operand>
 * [])` -- `env` is the captured operand, `value` the resumed value.  Mirrors
 * emit_cps.c's frame_c_expr byte-for-byte (the proven generator). */
/* The C cast applied to an intptr-carried frame env/value handed to a call
 * target, keyed by the target's param kind.  A cstr rides the carrier like an
 * int (CC4 value-typed cont<cstr>); mirrors emit_cps.c's c_cast_for_kind. */
static const char *cc_cast_for_kind(TypeKind k) {
    return (k == TY_CSTR) ? "(const char *)" : "(int64_t)";
}

static const char *cloneable_frame_expr(const char *op, bool hole_left) {
    if (strcmp(op, "+") == 0) return "env + value";
    if (strcmp(op, "*") == 0) return "env * value";
    if (strcmp(op, "-") == 0) return hole_left ? "value - env" : "env - value";
    if (hole_left)  /* "/" hole-left: value / env */
        return "(env) ? (value / env) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0)";
    return "(value) ? (env / value) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0)";
}

/* U4 serial: the stable marshaler tag for an arithmetic context frame (op + hole
 * side), mirroring emit_cps.c's sk_tag_for_frame.  The serial runtime prelude
 * emits `__sk_frame_for_tag(tag)` returning the matching DKFrame, and the
 * marshaler maps frame <-> tag by this table so save-cont!/resume-cont! round-
 * trips.  1=ADD 2=MUL 3=SUBR(env-v) 4=SUBL(v-env) 5=DIVR(env/v) 6=DIVL(v/env). */
static int sk_tag_for_frame(const CloneFrame *fr) {
    if (strcmp(fr->op, "+") == 0) return 1;
    if (strcmp(fr->op, "*") == 0) return 2;
    if (strcmp(fr->op, "-") == 0) return fr->hole_left ? 4 : 3;
    return fr->hole_left ? 6 : 5;   /* "/" */
}

/* Resolve the Serializable instance's serialize/deserialize C names for a nominal
 * (TY_ADT) serial call-frame env type `t`.  Returns malloc'd names via out-params
 * and true, or false if no instance.  Mirrors emit_cps.c's sk_find_serializable
 * name path, kept in the native path so the CT-IR serial emitter owns its env
 * marshaling (the runtime Sk registry already encodes SK_ENV_SER). */
static bool serial_env_ser_names(CE *ce, const Type *t,
                                 char **ser_out, char **deser_out) {
    const Expr *program = ce->ctx->program_root;
    if (!program || program->kind != EX_PROGRAM) return false;
    if (!t || t->kind != TY_ADT || !t->as.adt_.def || !t->as.adt_.def->name) return false;
    const char *tname = t->as.adt_.def->name;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_INSTANCE_DEF) continue;
        const TypeClassInstance *inst = it->as.instance_def_.instance;
        if (!inst || !inst->typeclass || !inst->typeclass->name) continue;
        if (strcmp(inst->typeclass->name->name, "Serializable") != 0) continue;
        if (inst->n_type_args < 1) continue;
        Type at = inst->type_args[0];
        if (at.kind != TY_ADT || !at.as.adt_.def || !at.as.adt_.def->name) continue;
        if (strcmp(at.as.adt_.def->name, tname) != 0) continue;
        char *ser = NULL, *deser = NULL;
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
        free(ser); free(deser);
        return false;
    }
    return false;
}

/* CT_CLONEABLE (U3): (cloneable-reset (cloneable-shift receiver val)) and its
 * single-arithmetic-frame Shape 2 form.  Emits the cloneable multi-shot machinery
 * natively (no emit_cps.c), reusing the shared DK runtime (dk_copy_range,
 * __dk_cont_fn / __dk_env_clone / __dk_env_drop) byte-for-byte.
 *
 *   Shape 1 (n_frames == 0): identity continuation -- no dk_copy_range.  Alloc a
 *     tur_cloneable_cont over an identity fn (NULL env), call receiver, bind x.
 *   Shape 2 (n_frames >= 1): reify the `(<op> <operand> ... [])` context as DK
 *     frame; the shift body deep-clones the captured sub-continuation into a
 *     cloneable_cont and hands it to receiver.  Mirrors emit_cloneable_ctx. */
/* Direct-emit a self-contained (shift-free) context Expr -- a `let` init, an
 * `if` condition, or an `if` pure arm -- at the reset site, honoring the CT
 * emitter's current indent.  Returns the malloc'd C expression string. */
static char *emit_cloneable_direct(CE *ce, const Expr *e) {
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;
    char *v = emit_value(ce->ctx, ce->out, e);
    ce->ctx->indent = saved;
    return v;
}

/* Build the pure-arm value of an if-split cloneable/serial context: the direct
 * value of `if_pure` with the OUTER context frames re-applied (frames[0..
 * n_outer_frames), applied innermost-outer first).  The shift-bearing arm rides
 * the full frame chain through the DK machine; the pure arm carries no shift, so
 * its outer frames must be applied here as plain C.  Inner frames
 * (frames[n_outer_frames..n_frames)) live inside the shift arm and do NOT apply
 * to the pure arm.  Mirrors cloneable_frame_expr's per-op semantics (env =
 * operand, value = the hole).  Returns a malloc'd C expression. */
static char *emit_cloneable_pure_arm(CE *ce, const CTerm *t) {
    char *acc = emit_cloneable_direct(ce, t->as.cloneable.if_pure);
    for (int i = (int)t->as.cloneable.n_outer_frames - 1; i >= 0; i--) {
        const CloneFrame *fr = &t->as.cloneable.frames[i];
        Buf b; buf_init(&b);
        if (fr->call_fn) {
            char *cfn = callee_name(fr->call_fn);
            if (!fr->ignore_value && fr->call_fn->type.as.fn.arity == 2) {
                /* 2-arg outer call frame: apply f to (pure, env) in source order
                 * (hole side = the pure value; the other arg is the captured env). */
                char *opv = atom_str(ce, &fr->operand);
                if (fr->hole_left)
                    buf_printf(&b, "((int64_t)%s((int64_t)(%s), (int64_t)(%s)))", cfn, acc, opv);
                else
                    buf_printf(&b, "((int64_t)%s((int64_t)(%s), (int64_t)(%s)))", cfn, opv, acc);
                free(opv);
            } else {
                /* 1-arg outer call frame: apply f to the pure value. */
                buf_printf(&b, "((int64_t)%s((int64_t)(%s)))", cfn, acc);
            }
            free(cfn);
        } else {
            char *opv = atom_str(ce, &fr->operand);
            const char *op = fr->op;
            if (strcmp(op, "+") == 0)
                buf_printf(&b, "((%s) + (%s))", opv, acc);
            else if (strcmp(op, "*") == 0)
                buf_printf(&b, "((%s) * (%s))", opv, acc);
            else if (strcmp(op, "-") == 0)
                buf_printf(&b, fr->hole_left ? "((%s) - (%s))" : "((%s) - (%s))",
                           fr->hole_left ? acc : opv, fr->hole_left ? opv : acc);
            else /* "/" -- guard div-by-zero exactly like cloneable_frame_expr */
                buf_printf(&b,
                    "((%s) ? ((%s) / (%s)) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                    fr->hole_left ? opv : acc,
                    fr->hole_left ? acc : opv,
                    fr->hole_left ? opv : acc);
            free(opv);
        }
        char *nv = strdup(b.data);
        buf_free(&b);
        free(acc);
        acc = nv;
    }
    return acc;
}

/* Emit the Shape 2 shift-body helper `bodyfn`.  `cont_setup` is the C that builds
 * the continuation the receiver is handed (a raw DK chain for serial, a
 * cloneable_cont for cloneable); `cont_arg` names it.  For a named-fn receiver the
 * dk_shift env carries the fn ptr and the body calls it as one; for a CLOSURE
 * receiver (receiver_expr) the closure's thunk is baked in here and the dk_shift
 * env carries the closure env instead (see emit_cl_shift_env). */
static void emit_cl_shift_bodyfn(CE *ce, const char *bodyfn, const CTerm *t,
                                 const char *cont_setup, const char *cont_arg) {
    if (t->as.cloneable.receiver_expr) {
        const Expr *f = t->as.cloneable.receiver_expr;
        struct Closure *closure = f->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        /* The thunk's first param is the closure env (void*); its second is the
         * receiver's continuation param `k`.  Cast each arg to the thunk's actual
         * param type so the generated call is warning-clean.  The k param's C type
         * is set by the family, not decidable from the param kind (a cloneable
         * `:cont` receiver and a serial `ptr<void>` receiver both carry kind
         * TY_PTR_VOID): the cloneable receiver is handed a `tur_cloneable_cont *`
         * that rides the int64_t carrier, so its thunk param is int64_t, while the
         * serial receiver is handed a raw DK chain typed `ptr<void>`, so its thunk
         * param is void*.  A blanket int64_t cast makes a pointer from an integer
         * for the serial void* k; a blanket void* cast makes an integer from a
         * pointer for the cloneable int64_t k. */
        const char *kty = t->as.cloneable.serial ? "void *" : "int64_t";
        buf_printf(ce->helpers,
            "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
            "    return (intptr_t)%s((void *)env, (%s)(intptr_t)%s);\n}\n",
            bodyfn, cont_setup, thunk_name, kty, cont_arg);
        free(thunk_name);
    } else {
        buf_printf(ce->helpers,
            "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
            "    return (intptr_t)((int64_t (*)(int64_t))(intptr_t)env)((int64_t)(intptr_t)%s);\n}\n",
            bodyfn, cont_setup, cont_arg);
    }
}

/* The dk_shift env expression (malloc'd) for a Shape 2 node: a closure receiver's
 * env value -- emitted at the reset site so its captures are read from the visible
 * locals / lifted env -- else the named receiver's fn name. */
static char *emit_cl_shift_env(CE *ce, const CTerm *t, const char *rfn) {
    if (t->as.cloneable.receiver_expr) {
        int saved = ce->ctx->indent;
        ce->ctx->indent = ce->indent;
        char *fval = emit_value(ce->ctx, ce->out, t->as.cloneable.receiver_expr);
        ce->ctx->indent = saved;
        return fval;
    }
    return strdup(rfn);
}

static void emit_cloneable(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char *xn  = cvar_cname(ce, t->as.cloneable.x);
    /* Named-fn receiver: its C name.  A closure receiver (receiver_expr, Shape 1)
     * has no bare fn name -- it is emitted as a value + thunk call below. */
    char *rfn = t->as.cloneable.receiver ? callee_name(t->as.cloneable.receiver) : NULL;

    /* Pure `let` prelude (Shape 2, let-bearing): lay each binding down as a C
     * local at the reset site, ahead of the frame-operand evaluations that may
     * reference it.  Emitted before any `if` branch point below, so a `let` above
     * an `if` is in scope for both the outer frame operands (shift arm) and the
     * pure arm. */
    for (uint32_t li = 0; li < t->as.cloneable.n_lets; li++) {
        const CloneLet *cl = &t->as.cloneable.lets[li];
        char *iv = emit_cloneable_direct(ce, cl->init);
        if (!cl->binding) {
            /* Side-effect prelude (do-sequence prefix): emit for effect only. */
            ce_line(ce, "(void)(%s);", iv);
            free(iv);
            continue;
        }
        char *bn = name_for_binding(ce->ctx, cl->binding);
        ce_line(ce, "%s %s = %s;", emit_type_c_name(ce->ctx, cl->binding->type), bn, iv);
        ce_line(ce, "(void)%s;", bn);
        free(bn);
        free(iv);
    }

    /* One `if` branch point (Shape 2, if-bearing): evaluate the pure condition
     * once, run the DK chain on the shift-bearing arm, and yield the direct-
     * emitted pure arm on the other branch. */
    bool has_if = (t->as.cloneable.if_cond != NULL);
    if (has_if) {
        char *cv = emit_cloneable_direct(ce, t->as.cloneable.if_cond);
        ce_line(ce, "if (%s(%s)) {", t->as.cloneable.if_when ? "" : "!", cv);
        free(cv);
        ce->indent++;
    }

    if (t->as.cloneable.n_frames == 0 && !t->as.cloneable.serial) {
        /* Shape 1 (cloneable): identity continuation.  The serial Shape 1
         * (n_frames == 0 && serial) falls through to the serial branch below,
         * where the empty frame loops produce a bare marshalable prompt chain --
         * a cloneable_cont would not round-trip through save-cont!/resume-cont!. */
        char cfn[256];
        snprintf(cfn, sizeof(cfn), "%s_clc%d", ce->fn_cn, id);
        buf_printf(ce->helpers,
            "static int64_t %s(void *__e, int64_t __v) { (void)__e; return __v; }\n", cfn);
        char cv[64];
        snprintf(cv, sizeof(cv), "__clc%d", id);
        ce_line(ce, "tur_cloneable_cont *%s = tur_cloneable_cont_alloc(%s, NULL, NULL, NULL);",
                cv, cfn);
        if (t->as.cloneable.receiver_expr) {
            /* U7 closure receiver: emit the closure value (its captures ride the
             * env -- visible locals in the main body, or env-carried in a lifted
             * body per collect_caps), then call its thunk with (closure-env, cont),
             * mirroring emit_callcc's closure call. */
            const Expr *f = t->as.cloneable.receiver_expr;
            int saved = ce->ctx->indent;
            ce->ctx->indent = ce->indent;
            char *fval = emit_value(ce->ctx, ce->out, f);
            ce->ctx->indent = saved;
            struct Closure *closure = f->as.closure_.closure;
            char *thunk_name;
            if (closure->fn->binding) {
                thunk_name = raw_name_for_binding(closure->fn->binding);
            } else {
                thunk_name = malloc(64);
                snprintf(thunk_name, 64, "__fn_anon_%d",
                         closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
            }
            ce_line(ce, "%s = %s(%s, (int64_t)(intptr_t)%s);", xn, thunk_name, fval, cv);
            free(thunk_name);
            free(fval);
        } else {
            ce_line(ce, "%s = %s((int64_t)(intptr_t)%s);", xn, rfn, cv);
        }
    } else if (t->as.cloneable.serial) {
        /* U4 Shape 2 (serial): an arithmetic context marshaled via the shared
         * tagged frames (`__sk_frame_for_tag`), and a shift body that hands the
         * receiver the copied DK chain directly so the captured continuation
         * round-trips through save-cont!/resume-cont!.  No per-site frame fns. */
        uint32_t nf = t->as.cloneable.n_frames;
        char bodyfn[256];
        snprintf(bodyfn, sizeof(bodyfn), "%s_skbody%d", ce->fn_cn, id);
        emit_cl_shift_bodyfn(ce, bodyfn, t,
            "    DK *__cap = dk_copy_range(subk, NULL);\n", "__cap");
        /* A 1-arg call frame gets a per-site wrapper fn plus a SkReg entry that
         * self-registers (constructor) so the marshaler maps the frame <-> a stable
         * name ("<fn>$L") for save/restore.  Arithmetic frames need no per-site
         * emission -- they ride the shared tagged marshaler. */
        for (uint32_t i = 0; i < nf; i++) {
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            if (!fr->call_fn) continue;
            char *cfn = callee_name(fr->call_fn);
            /* A do-tail ignore-value frame runs f() regardless of the resumed
             * value and marshals under the "$0" side; a 1-arg hole call applies f
             * to the resumed value under "$L"; a 2-arg call applies f to the
             * resumed value + the captured int env (source order per hole side)
             * under "$2L"/"$2R", the env serialized inline. */
            uint32_t ar = fr->call_fn->type.as.fn.arity;
            bool two_arg = !fr->ignore_value && ar == 2;
            bool env1    = fr->ignore_value && ar == 1;  /* 1-arg captured-config tail */
            bool has_env = two_arg || env1;
            /* Env marshal kind: SK_ENV_INT (0) / SK_ENV_CSTR (1) inline codec, or
             * SK_ENV_SER (2) via the env type's Serializable instance.  Keyed off
             * the captured operand's type. */
            int ekc = 0;
            char *eser = NULL, *edeser = NULL;
            if (has_env) {
                if (fr->operand.ty == TY_CSTR) ekc = 1;
                else if (fr->operand.type
                         && serial_env_ser_names(ce, fr->operand.type, &eser, &edeser))
                    ekc = 2;
            }
            const char *ecast = ekc == 1 ? "(const char *)" : "(int64_t)";
            const char *side;
            if (fr->ignore_value && ar == 0) {
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { (void)env; (void)value; return (intptr_t)%s(); }\n",
                    ce->fn_cn, id, i, cfn);
                side = "$0";
            } else if (env1) {
                /* Run f(cap) on resume, ignoring the resumed value; cap = the env. */
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { (void)value; return (intptr_t)%s(%senv); }\n",
                    ce->fn_cn, id, i, cfn, ecast);
                side = "$E";
            } else if (two_arg) {
                TypeKind k0 = fr->call_fn->type.as.fn.arg_kinds[0];
                TypeKind k1 = fr->call_fn->type.as.fn.arg_kinds[1];
                const char *a0 = fr->hole_left ? "value" : "env";
                const char *a1 = fr->hole_left ? "env" : "value";
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { return (intptr_t)%s(%s%s, %s%s); }\n",
                    ce->fn_cn, id, i, cfn,
                    cc_cast_for_kind(k0), a0, cc_cast_for_kind(k1), a1);
                side = fr->hole_left ? "$2L" : "$2R";
            } else {
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { (void)env; return (intptr_t)%s(%svalue); }\n",
                    ce->fn_cn, id, i, cfn, cc_cast_for_kind(fr->call_fn->type.as.fn.arg_kinds[0]));
                side = "$L";
            }
            if (ekc == 2) {
                /* SER env: carry the instance serialize/deserialize fn pointers. */
                buf_printf(ce->helpers,
                    "static SkReg %s_skreg%d_%u = { \"%s%s\", %s_skcall%d_%u, %d,"
                    " (void *(*)(int64_t))%s, (int64_t (*)(void *))%s, 0 };\n"
                    "__attribute__((constructor)) static void %s_skreginit%d_%u(void) { __sk_register(&%s_skreg%d_%u); }\n",
                    ce->fn_cn, id, i, cfn, side, ce->fn_cn, id, i, ekc, eser, edeser,
                    ce->fn_cn, id, i, ce->fn_cn, id, i);
            } else {
                buf_printf(ce->helpers,
                    "static SkReg %s_skreg%d_%u = { \"%s%s\", %s_skcall%d_%u, %d, 0, 0, 0 };\n"
                    "__attribute__((constructor)) static void %s_skreginit%d_%u(void) { __sk_register(&%s_skreg%d_%u); }\n",
                    ce->fn_cn, id, i, cfn, side, ce->fn_cn, id, i, ekc,
                    ce->fn_cn, id, i, ce->fn_cn, id, i);
            }
            free(eser); free(edeser);
            free(cfn);
        }
        char dv[48];
        snprintf(dv, sizeof(dv), "__skd%d", id);
        ce_line(ce, "DK *%s = dk_prompt(1, dk_done());", dv);
        /* Push frames outermost-first (frames[0] deepest, closest to the prompt).
         * Arithmetic -> shared tagged frame + operand; call frame -> per-site
         * wrapper, no env (0). */
        for (uint32_t i = 0; i < nf; i++) {
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            if (fr->call_fn) {
                /* A 2-arg hole call or a 1-arg captured-config do-tail frame carries
                 * the captured env; a 1-arg hole call / 0-arg do-tail pass 0.  The
                 * env rides q->env and is marshaled by kind (int/cstr inline, or a
                 * Serializable instance).  A non-atomic env is emit_value'd here at
                 * the reset site (once, cold-start capture). */
                uint32_t ar = fr->call_fn->type.as.fn.arity;
                bool has_env = (!fr->ignore_value && ar == 2)
                            || (fr->ignore_value && ar == 1);
                char *opv = NULL;
                if (has_env)
                    opv = fr->env_expr ? emit_cloneable_direct(ce, fr->env_expr)
                                       : atom_str(ce, &fr->operand);
                ce_line(ce, "%s = dk_frame(%s_skcall%d_%u, (intptr_t)(%s), %s);",
                        dv, ce->fn_cn, id, i, opv ? opv : "0", dv);
                free(opv);
            } else {
                /* D6a parity: a non-atomic pure arithmetic operand (env_expr) is
                 * emit_value'd at the reset site; its int value rides the tagged
                 * frame env, serialized inline by the shared marshaler. */
                char *opv = fr->env_expr ? emit_cloneable_direct(ce, fr->env_expr)
                                         : atom_str(ce, &fr->operand);
                ce_line(ce, "%s = dk_frame(__sk_frame_for_tag(%d), (intptr_t)(%s), %s);",
                        dv, sk_tag_for_frame(fr), opv, dv);
                free(opv);
            }
        }
        char *senv = emit_cl_shift_env(ce, t, rfn);
        ce_line(ce, "%s = dk_shift(1, %s, (intptr_t)(%s), %s);", dv, bodyfn, senv, dv);
        free(senv);
        ce_line(ce, "%s = (%s)dk_run(%s, 0);", xn,
                binder_ctype_full(ce->ctx, t->as.cloneable.x.ty, t->as.cloneable.x.type), dv);
        ce_line(ce, "dk_free(%s);", dv);
    } else {
        /* Shape 2: an arithmetic context (1+ frames) + dk_copy_range capture. */
        uint32_t nf = t->as.cloneable.n_frames;
        char bodyfn[256];
        snprintf(bodyfn, sizeof(bodyfn), "%s_ccbody%d", ce->fn_cn, id);
        /* One frame fn per context frame: an arithmetic op, a 1-arg call to a
         * top-level fn applied to the resumed value (no captured env), or a 2-arg
         * call whose non-hole arg rides the frame env (deep-cloned with the chain
         * on each resume, so multi-shot is correct without any marshaling). */
        for (uint32_t i = 0; i < nf; i++) {
            char ctxfn[256];
            snprintf(ctxfn, sizeof(ctxfn), "%s_ccctx%d_%u", ce->fn_cn, id, i);
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            if (fr->call_fn) {
                char *cfn = callee_name(fr->call_fn);
                bool two_arg = !fr->ignore_value && fr->call_fn->type.as.fn.arity == 2;
                if (fr->ignore_value)
                    /* 0-arg ignore-value tail: run f() regardless of resume value. */
                    buf_printf(ce->helpers,
                        "static intptr_t %s(intptr_t env, intptr_t value) { (void)env; (void)value; return (intptr_t)%s(); }\n",
                        ctxfn, cfn);
                else if (two_arg) {
                    /* 2-arg call: apply f to (value, env) in source order (env =
                     * the captured non-hole arg), casting each to its param kind. */
                    TypeKind k0 = fr->call_fn->type.as.fn.arg_kinds[0];
                    TypeKind k1 = fr->call_fn->type.as.fn.arg_kinds[1];
                    const char *a0 = fr->hole_left ? "value" : "env";
                    const char *a1 = fr->hole_left ? "env" : "value";
                    buf_printf(ce->helpers,
                        "static intptr_t %s(intptr_t env, intptr_t value) { return (intptr_t)%s(%s%s, %s%s); }\n",
                        ctxfn, cfn, cc_cast_for_kind(k0), a0, cc_cast_for_kind(k1), a1);
                } else
                    /* 1-arg hole call: apply f to the resumed value. */
                    buf_printf(ce->helpers,
                        "static intptr_t %s(intptr_t env, intptr_t value) { (void)env; return (intptr_t)%s(%svalue); }\n",
                        ctxfn, cfn, cc_cast_for_kind(fr->call_fn->type.as.fn.arg_kinds[0]));
                free(cfn);
            } else {
                buf_printf(ce->helpers,
                    "static intptr_t %s(intptr_t env, intptr_t value) { return %s; }\n",
                    ctxfn, cloneable_frame_expr(fr->op, fr->hole_left));
            }
        }
        emit_cl_shift_bodyfn(ce, bodyfn, t,
            "    DK *__cap = dk_copy_range(subk, NULL);\n"
            "    tur_cloneable_cont *__k = tur_cloneable_cont_alloc(__dk_cont_fn, __cap, __dk_env_clone, __dk_env_drop);\n",
            "__k");
        char dv[48];
        snprintf(dv, sizeof(dv), "__ccd%d", id);
        ce_line(ce, "DK *%s = dk_prompt(1, dk_done());", dv);
        /* Push frames outermost-first (frames[0] deepest, closest to the prompt).
         * A 2-arg call frame carries its captured int env; 1-arg / 0-arg call and
         * do-tail frames pass 0. */
        for (uint32_t i = 0; i < nf; i++) {
            char ctxfn[256];
            snprintf(ctxfn, sizeof(ctxfn), "%s_ccctx%d_%u", ce->fn_cn, id, i);
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            bool two_arg = fr->call_fn && !fr->ignore_value
                && fr->call_fn->type.as.fn.arity == 2;
            /* A non-atomic env operand (fr->env_expr set -- an arithmetic frame
             * or a 2-arg call frame) is emit_value'd at the reset site; a 1-arg
             * call frame carries no env (pass 0); everything else rides the atom. */
            char *opv = (fr->call_fn && !two_arg) ? strdup("0")
                      : fr->env_expr ? emit_cloneable_direct(ce, fr->env_expr)
                      : atom_str(ce, &fr->operand);
            ce_line(ce, "%s = dk_frame(%s, (intptr_t)(%s), %s);", dv, ctxfn, opv, dv);
            free(opv);
        }
        char *senv = emit_cl_shift_env(ce, t, rfn);
        ce_line(ce, "%s = dk_shift(1, %s, (intptr_t)(%s), %s);", dv, bodyfn, senv, dv);
        free(senv);
        ce_line(ce, "%s = (%s)dk_run(%s, 0);", xn,
                binder_ctype_full(ce->ctx, t->as.cloneable.x.ty, t->as.cloneable.x.type), dv);
        ce_line(ce, "dk_free(%s);", dv);
    }

    if (has_if) {
        ce->indent--;
        ce_line(ce, "} else {");
        ce->indent++;
        /* The pure arm re-applies the OUTER context frames (those outside the
         * `if`); inner frames belong to the shift-bearing arm only. */
        char *pv = emit_cloneable_pure_arm(ce, t);
        ce_line(ce, "%s = %s;", xn, pv);
        free(pv);
        ce->indent--;
        ce_line(ce, "}");
    }

    free(xn);
    free(rfn);
    emit_term(ce, t->as.cloneable.body);
}

/* CT_SHIFT: capture the sub-continuation up to the nearest enclosing prompt and
 * run the shift body (abortive: the identity receiver delivers the body value,
 * ignoring the captured continuation). */
static void emit_shift(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char hname[256];
    snprintf(hname, sizeof(hname), "%s_s%d", ce->fn_cn, id);
    /* N6.3: the shift body's scalar captures (source vars / CPS vars) ride the
     * shift-body helper's env struct; the captured continuation k is excluded
     * (it is `subk`, not an env slot). */
    CapSet cs;
    bool ok = collect_caps(t->as.shift.body, t->as.shift.k.id, &cs);
    const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
    emit_lifted(ce, hname, LH_SHIFT_BODY, NULL, TY_INT, NULL, t->as.shift.body, NULL, caps);
    char *env = emit_cont_env(ce, hname, caps, NULL);   /* k-less env */
    /* shift0 does NOT reinstall the delimiting prompt; plain shift does. */
    ce_line(ce, "return dk_run(%s(1, %s, %s, %s), 0);",
            t->as.shift.shift0 ? "dk_shift0" : "dk_shift", hname, env, ce->cur_k);
    free(env);
}

/* ---- C4: algebraic effects (handle / perform / resume) --------------- *
 * handle mirrors reset: lift the handle continuation to a DKFrame, install a
 * dk_handler (carrying the case as a DKHandler), and run the handled body
 * threading that handler prompt.  perform mirrors shift: lift the perform
 * continuation (a pure value transform) onto the chain and dk_perform against
 * the effect's tag; the handler runs the case, and resume = dk_invoke. */
static void emit_handle(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char kname[256];
    snprintf(kname, sizeof(kname), "%s_hk%d", ce->fn_cn, id);
    /* lift the handle continuation (like a reset continuation), carrying k + any
     * scalar captures of the continuation body. */
    CapSet cs;
    bool ok = collect_caps(t->as.handle.body, t->as.handle.x.id, &cs);
    const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
    char *hxn = cvar_cname(ce, t->as.handle.x);
    emit_lifted(ce, kname, LH_RESET_CONT, hxn, t->as.handle.x.ty, t->as.handle.x.type,
                t->as.handle.body, NULL, caps);
    free(hxn);
    char *hkenv = emit_cont_env(ce, kname, caps, ce->cur_k);

    /* Lift each handler case as its own DKHandler and chain a dk_handler per case
     * onto the continuation.  dk_perform searches the chain by effect tag, so N
     * chained handlers dispatch N effects; the innermost `next` is the handle
     * continuation frame carrying the enclosing continuation `k`. */
    uint32_t nc = t->as.handle.n_cases;
    char **cnames = (char **)calloc(nc ? nc : 1, sizeof(char *));
    char **cenvs  = (char **)calloc(nc ? nc : 1, sizeof(char *));  /* per-case env expr */
    for (uint32_t ci = 0; ci < nc; ci++) {
        cnames[ci] = (char *)malloc(256);
        snprintf(cnames[ci], 256, "%s_hc%d_%u", ce->fn_cn, id, ci);
        /* A handler case may capture enclosing scalars (beyond its params + k),
         * carried in a caps-only env (k arrives via subk). */
        CapSet ccs;
        bool cok = collect_caps_case(t->as.handle.cases[ci].case_body, &t->as.handle.cases[ci], &ccs);
        const CapSet *ccaps = (cok && ccs.n > 0) ? &ccs : NULL;
        emit_lifted(ce, cnames[ci], LH_HANDLER_CASE, NULL, TY_INT, NULL,
                    t->as.handle.cases[ci].case_body, &t->as.handle.cases[ci], ccaps);
        cenvs[ci] = emit_cont_env(ce, cnames[ci], ccaps, NULL);   /* k-less env */
    }

    char hchain[64];
    snprintf(hchain, sizeof(hchain), "__h%d", id);
    /* Build the chain inside-out: base is the continuation frame; wrap each case
     * as dk_handler(tag_i, case_i, case_env_i, <inner>). */
    Buf chain; buf_init(&chain);
    buf_printf(&chain, "dk_frame(%s, %s, dk_done())", kname, hkenv);
    for (int ci = (int)nc - 1; ci >= 0; ci--) {
        int tag = effect_tag(t->as.handle.cases[ci].effect);
        Buf nxt; buf_init(&nxt);
        buf_printf(&nxt, "dk_handler(%d, %s, %s, %.*s)", tag, cnames[ci], cenvs[ci],
                   (int)chain.len, chain.data);
        buf_free(&chain);
        chain = nxt;
    }
    buf_putc(&chain, '\0');
    ce_line(ce, "DK *%s = %s;", hchain, chain.data);
    buf_free(&chain);
    free(hkenv);
    for (uint32_t ci = 0; ci < nc; ci++) { free(cnames[ci]); free(cenvs[ci]); }
    free(cnames); free(cenvs);

    const char *save = ce->cur_k;
    const Type *save_ty = ce->cur_ty;
    ce->cur_k = arena_strdup(&g_arena, hchain, strlen(hchain));
    ce->cur_ty = t->as.handle.x.type;    /* KK_PROMPT crossing type = handle result */
    emit_term(ce, t->as.handle.delim);   /* body runs threading the handler prompt */
    ce->cur_k = save;
    ce->cur_ty = save_ty;
}

/* Is the perform continuation trivial -- deliver the result straight to the
 * function's return continuation (a tail perform)?  Then no frame is needed. */
static bool perform_cont_trivial(const CTerm *t) {
    const CTerm *b = t->as.perform.body;
    return b && b->kind == CT_APPCONT
        && (b->as.appcont.kont.kind == KK_RET || b->as.appcont.kont.kind == KK_PROMPT)
        && b->as.appcont.v.kind == CA_CVAR
        && b->as.appcont.v.cvar_id == t->as.perform.x.id;
}

static void emit_perform(CE *ce, const CTerm *t) {
    int tag = effect_tag(t->as.perform.effect);
    /* effect value word: a 0-arg effect passes 0; a 1-arg effect passes the arg's
     * slot value directly; a multi-arg effect (n>1) heap-packs each arg's slot
     * word into an `int64_t[n]` and passes the array pointer -- the handler case
     * unpacks each param from it (emit_lifted).  The array is leaked with the DK
     * nodes (a multi-shot resume shares it read-only), matching the env-leak
     * discipline. */
    char *arg = NULL, *sa;
    if (t->as.perform.n > 1) {
        int aid = (*ce->helper_ctr)++;
        char av[64];
        snprintf(av, sizeof av, "__eargs%d", aid);
        ce_line(ce, "int64_t *%s = (int64_t *)malloc(%u * sizeof(int64_t));",
                av, (unsigned)t->as.perform.n);
        for (uint32_t i = 0; i < t->as.perform.n; i++) {
            char *ai = atom_str(ce, &t->as.perform.args[i]);
            char *si = slot_store(ce->ctx, t->as.perform.args[i].ty,
                                  t->as.perform.args[i].type, ai);
            ce_line(ce, "%s[%u] = %s;", av, i, si);
            free(si); free(ai);
        }
        Buf sb; buf_init(&sb);
        buf_printf(&sb, "(intptr_t)%s", av);
        buf_putc(&sb, '\0');
        sa = strdup(sb.data); buf_free(&sb);
    } else {
        /* argument: 0-arg effects pass 0; 1-arg pass the arg (subset). */
        arg = (t->as.perform.n >= 1)
            ? atom_str(ce, &t->as.perform.args[0]) : strdup("0");
        TypeKind argty = (t->as.perform.n >= 1) ? t->as.perform.args[0].ty : TY_NIL;
        const Type *argt = (t->as.perform.n >= 1) ? t->as.perform.args[0].type : NULL;
        sa = slot_store(ce->ctx, argty, argt, arg);
    }

    if (perform_cont_trivial(t)) {
        ce_line(ce, "return dk_perform(%d, %s, %s);", tag, sa, ce->cur_k);
    } else {
        int id = (*ce->helper_ctr)++;
        char pname[256];
        snprintf(pname, sizeof(pname), "%s_pf%d", ce->fn_cn, id);
        char *pxn = cvar_cname(ce, t->as.perform.x);
        /* N6.3: collect the continuation's scalar captures; when present, carry
         * them in a heap env struct passed as the frame env. */
        CapSet cs;
        bool ok = collect_caps(t->as.perform.body, t->as.perform.x.id, &cs);
        const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
        emit_lifted(ce, pname, LH_PERFORM_CONT, pxn, t->as.perform.x.ty, t->as.perform.x.type,
                    t->as.perform.body, NULL, caps);
        free(pxn);
        if (caps) {
            /* Allocate + populate the env from the enclosing locals (same names).
             * The struct is leaked with the DK nodes (a multi-shot resume shares
             * it read-only). */
            char envv[64];
            snprintf(envv, sizeof envv, "__pfe%d", id);
            ce_line(ce, "%s_env *%s = (%s_env *)malloc(sizeof(%s_env));", pname, envv, pname, pname);
            for (int i = 0; i < cs.n; i++) {
                char *cn = cs.b[i] ? name_for_binding(ce->ctx, cs.b[i]) : strdup(cs.cvname[i]);
                ce_line(ce, "%s->f%d = %s;", envv, i, cn);
                free(cn);
            }
            ce_line(ce, "return dk_perform(%d, %s, dk_frame(%s, (intptr_t)%s, %s));",
                    tag, sa, pname, envv, ce->cur_k);
        } else {
            ce_line(ce, "return dk_perform(%d, %s, dk_frame(%s, 0, %s));",
                    tag, sa, pname, ce->cur_k);
        }
    }
    free(sa);
    free(arg);
}

static void emit_resume(CE *ce, const CTerm *t) {
    /* resume k v = dk_invoke((DK*)k, v); slot-load the result and continue. */
    char *kk = atom_str(ce, &t->as.resume.k);
    char *vv = atom_str(ce, &t->as.resume.v);
    char *sv = slot_store(ce->ctx, t->as.resume.v.ty, t->as.resume.v.type, vv);  /* resume value -> slot */
    Buf inv; buf_init(&inv);
    buf_printf(&inv, "dk_invoke((DK *)(%s), %s)", kk, sv);
    buf_putc(&inv, '\0');
    /* result <- slot; leak a Tier C box (the resumed continuation may be re-run). */
    char *ld = slot_load(ce->ctx, t->as.resume.x.ty, t->as.resume.x.type, inv.data, false);
    char *bn = cvar_cname(ce, t->as.resume.x);
    ce_line(ce, "%s = %s;", bn, ld);
    buf_free(&inv);
    free(bn); free(ld); free(sv); free(kk); free(vv);
    emit_term(ce, t->as.resume.body);
}

/* CT_CALLCC (U7): (call/cc f)/(escape f) as a LOCAL setjmp escape landing --
 * a native port of emit_cps_callcc.  Establish a tur_escape_cont landing, run
 * the (capture-free) receiver f with the landing handle as its continuation,
 * and bind x to f's normal return; an upward (tur_escape_resume &cc v) longjmps
 * back and delivers v instead.  Does NOT thread the DK continuation, so the
 * enclosing prompt chain is untouched.  The escape runtime (tur_escape_cont /
 * tur_escape_resume) is emitted by emit_cps_callcc_prelude, gated on the
 * program containing EX_CALLCC (fires regardless of backend). */
static void emit_callcc(CE *ce, const CTerm *t) {
    const Expr *e = t->as.callcc.e;
    const Expr *f = e->as.callcc_.fn;
    while (f && f->kind == EX_ASCRIBE) f = f->as.ascribe_.inner;   /* peel like build_callcc */
    char *xn = cvar_cname(ce, t->as.callcc.x);
    const char *rty = binder_ctype_full(ce->ctx, t->as.callcc.x.ty, t->as.callcc.x.type);
    int id = (*ce->helper_ctr)++;
    char cc[48];
    snprintf(cc, sizeof(cc), "__cc%d", id);

    ce_line(ce, "tur_escape_cont %s;", cc);
    ce_line(ce, "%s.valid = 1;", cc);
    ce_line(ce, "if (setjmp(%s.buf) == 0) {", cc);
    ce->indent += 4;
    /* Normal path: emit the receiver value, then call it with &cc. */
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;
    char *fval = emit_value(ce->ctx, ce->out, f);
    ce->ctx->indent = saved;
    if (f->kind == EX_CLOSURE) {
        struct Closure *closure = f->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        ce_line(ce, "%s = (%s)%s(%s, (int64_t)(intptr_t)&%s);", xn, rty, thunk_name, fval, cc);
        free(thunk_name);
    } else {
        ce_line(ce, "%s = (%s)%s((int64_t)(intptr_t)&%s);", xn, rty, fval, cc);
    }
    /* f returned normally: the captured continuation is now dead. */
    ce_line(ce, "%s.valid = 0;", cc);
    free(fval);
    ce->indent -= 4;
    ce_line(ce, "} else {");
    ce->indent += 4;
    /* Resumed path: an upward (tur_escape_resume &cc v) delivered v here. */
    ce_line(ce, "%s = (%s)%s.result;", xn, rty, cc);
    ce->indent -= 4;
    ce_line(ce, "}");
    free(xn);
    emit_term(ce, t->as.callcc.body);
}

/* ---- signatures ------------------------------------------------------ */

static void emit_params(EmitCtx *ctx, Buf *file, const FnDef *fd) {
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        const char *pty;
        /* A rank-2 poly fn value crosses as a fat closure (`tur_poly_fn_t`),
         * matching the direct emitter's signature -- not the `void *` its scalar-
         * pointer type kind would otherwise pick.  A delegated indirect call
         * (`fnv.fn` / `fnv.env`) reads the struct's fields, so the parameter must
         * be declared as the fat closure, not an opaque pointer. */
        if (fd->params[i]->is_poly_fn)
            pty = "tur_poly_fn_t";
        else if (fd->params[i]->type.kind == TY_FN) {
            /* A plain (non-rank-2) fn param crosses as the direct emitter's
             * function-pointer spelling: a typed C-ABI pointer becomes its
             * registered `R (*)(A...)` typedef, an ordinary closure carrier the
             * opaque `int64_t`.  type_c_name(TY_FN) leaks a bad spelling, so this
             * mirrors emit_module.c's signature emission exactly -- otherwise a
             * delegated call through the param (which the direct emitter emits)
             * and the __cps declaration would disagree. */
            const char *td = fd->params[i]->type.as.fn.cfnptr
                ? register_fn_ptr_typedef(&fd->params[i]->type) : NULL;
            pty = td ? td : "int64_t";
        }
        else
            /* A by-value aggregate param needs its real (monomorphized) C type,
             * which emit_type_from_kind(TY_ADT/APP/STRUCT) loses -- use the full
             * Type. */
            pty = binder_ctype_full(ctx, fd->params[i]->type.kind, &fd->params[i]->type);
        char *pn = name_for_binding(ctx, fd->params[i]);
        buf_printf(file, "%s %s", pty, pn);
        free(pn);
    }
}

/* Emit forward declarations for every emittable __cps function, once per
 * program.  Goes into `file` (appended after the DK prelude in `out`). */
static void emit_forward_decls(EmitCtx *ctx, Buf *file) {
    for (size_t i = 0; i < g_ents_n; i++) {
        if (!g_ents[i].in_s) continue;
        const FnDef *fd = g_ents[i].fd;
        char *cn = raw_name_for_binding(fd->binding);
        buf_printf(file, "static int64_t %s__cps(", cn);
        emit_params(ctx, file, fd);
        if (fd->n_params) buf_puts(file, ", ");
        buf_puts(file, "DK *__kont);\n");
        free(cn);
    }
    /* G3b: forward-declare each mono-template monomorph's `<clone>__cps`, since a
     * cps->cps caller (a DK peer) references it before the spec-body emit loop
     * defines it.  emit_params + the return type resolve to concrete types under
     * the active spec. */
    for (size_t i = 0; i < g_ents_n; i++) {
        if (!g_ents[i].mono_template) continue;
        FnDef *fd = (FnDef *)g_ents[i].fd;
        for (uint32_t s = 0; s < ctx->n_abi_specializations; s++) {
            EmitAbiSpecialization *spec = &ctx->abi_specializations[s];
            if (spec->fn != fd || !spec->clone_name) continue;
            const EmitAbiSpecialization *saved = ctx->current_abi_specialization;
            ctx->current_abi_specialization = spec;
            g_cps_mono_resolver = ctx;
            buf_printf(file, "static int64_t %s__cps(", spec->clone_name);
            emit_params(ctx, file, fd);
            if (fd->n_params) buf_puts(file, ", ");
            buf_puts(file, "DK *__kont);\n");
            g_cps_mono_resolver = NULL;
            ctx->current_abi_specialization = saved;
        }
    }
}

bool emit_cps_ir_program_has_emittable(const Expr *program) {
    ensure_S(program);
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].in_s) return true;
    return false;
}

/* Whether the CT-IR CPS backend emits `b`'s function as `<b>__cps(..., DK*)`.
 * The legacy CPS3 `--cps-path` wrapper path (emit_module.c / emit_fns.c) uses
 * this to skip a colored function the cps-backend already emits -- otherwise the
 * two mechanisms declare the same `__cps` symbol with different signatures
 * (`void(tur_cps_cont_t*, ...)` vs `int64_t(..., DK*)`), a `conflicting types`
 * error.  Post-graduation the cps-backend is always-on, so this guard is what
 * keeps `--cps-path` from colliding with it. */
bool emit_cps_ir_emits_binding(const Expr *program, const Binding *b) {
    if (!program || !b) return false;
    ensure_S(program);
    return binding_in_s(b);
}

/* G1 (cps-backend-generic-monomorph-classification-plan): analysis-only probe.
 * When --dump-cps-mono is set and this call is emitting a colored-generic
 * MONOMORPH body (ctx->current_abi_specialization pins fd to concrete type args),
 * report whether that monomorph's concrete signature + body would land in the
 * CPS-backend subset -- the information the generic template cannot give (it
 * sig-rejects on the tyvar TY_APP and is never a candidate).  Substitution is via
 * emit_resolve_type through the active spec (the g_cps_mono_resolver hook that
 * slot_ok_t consults).  Changes no emitted code; a pure `stderr` report. */
/* G2: monomorph signature admissibility using the direct emitter's MATERIALIZED
 * concrete types (spec->result_type / arg_types[]) rather than re-resolving the
 * generic annotation.  emit_resolve_type can collapse `(Box A)` to the bare,
 * unapplied `TY_ADT Box` that the by-value-product gate rejects; the spec types
 * are the fully-applied `TY_APP (Box int)` the gate recognizes.  Mirrors
 * fn_sig_ok's per-binding checks (is_poly_fn / is_borrow / effect-free fn param /
 * name clash) but reads the concrete type at each position. */
static bool mono_sig_ok(const FnDef *fd, const EmitAbiSpecialization *spec) {
    const Type *rt = (spec->result_type.kind != TY_UNKNOWN)
                   ? &spec->result_type : fn_ret_type(fd);
    if (rt->kind != TY_NIL && !sig_slot_ok(rt, rt->kind)) return false;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        const Binding *p = fd->params[i];
        const Type *pt = (i < spec->n_args) ? &spec->arg_types[i] : &p->type;
        if (p->is_poly_fn) return false;   /* poly-fat param -- evict (see fn_sig_ok) */
        bool fn_param_ok = pt->kind == TY_FN;   /* effectful admitted -- see fn_sig_ok */
        if (!p->is_borrow && !fn_param_ok
            && !sig_slot_ok(pt, pt->kind)) return false;
        if (param_name_clashes_cps(p)) return false;
    }
    return true;
}

static void cps_dump_mono_admissible(EmitCtx *ctx, FnDef *fd) {
    const EmitAbiSpecialization *spec = ctx->current_abi_specialization;
    if (!spec || spec->fn != fd || !fd->body) return;
    if (!fd->cps_colored) return;   /* only colored generics are interesting */
    Arena tmp; arena_init(&tmp, 1 << 16);
    CTerm *t = cps_ir_translate_fn(&tmp, (Expr *)ctx->program_root, fd);
    /* Signature: materialized spec types (no resolver hook needed -- they are
     * already concrete).  Body: the generic binders are tyvar-typed, so the
     * resolver hook substitutes each through the active spec while term_core_ok
     * runs. */
    bool sig  = mono_sig_ok(fd, spec);
    g_cps_mono_resolver = ctx;
    bool body = t && term_core_ok(t);
    g_cps_mono_resolver = NULL;
    const Symbol *handled0[1];
    bool island = t && is_cps_island(t, handled0, 0);
    const char *nm = spec->clone_name ? spec->clone_name
                   : (fd->binding && fd->binding->name ? fd->binding->name->name : "?");
    bool admissible = sig && body;
    fprintf(stderr, "[cps-mono] %s: sig=%s body=%s island=%s => %s\n",
            nm, sig ? "ok" : "no", body ? "ok" : "no", island ? "yes" : "no",
            admissible ? (island ? "ISLAND-EMITTABLE" : "ADMISSIBLE(cross-fn)") : "fallback");
    arena_free(&tmp);
}

bool emit_cps_ir_try_fn(EmitCtx *ctx, Buf *file, const Expr *e) {
    if (g_dump_cps_mono && e && e->kind == EX_FN_DEF && e->as.fn_def_.fn
        && ctx->current_abi_specialization)
        cps_dump_mono_admissible(ctx, e->as.fn_def_.fn);
    if (!e || e->kind != EX_FN_DEF || !e->as.fn_def_.fn) return false;
    FnDef *fd = e->as.fn_def_.fn;
    if (!fd->binding) return false;
    const Expr *program = ctx->program_root;
    if (!program) return false;

    /* ensure_S colors the program (idempotently) and classifies it; the
     * pipeline has not colored by emit time, so this must precede any read of
     * fd->cps_colored / g_ents.  g_emit_ctx gives it the complete spec set
     * (ctx->abi_specializations) for the G3b mono-template classification. */
    g_emit_ctx = ctx;
    ensure_S(program);
    SEnt *se = ent_of(fd);

    /* G3a: a colored-generic MONOMORPH emitted as a self-contained DK island.
     * The generic template sig-rejects (tyvar TY_APP) so `se->in_s` is false, but
     * this specific monomorph -- concrete signature admissible, body core, and a
     * self-contained island (is_cps_island: every effect handled within, no
     * colored callee) -- is safe to CPS-emit standalone: its entry wrapper keeps
     * the SAME `<clone>` symbol callers already call, and its effects never touch
     * another function's machine.  No taint/routing change. */
    const EmitAbiSpecialization *spec = ctx->current_abi_specialization;
    bool island_mono = false;   /* G3a: self-contained island (callers use wrapper) */
    bool mono_emit = false;     /* G3a or G3b: this monomorph is CPS-emitted */
    if (spec && spec->fn == fd && fd->cps_colored && se && se->term
        && spec->clone_name) {
        g_cps_mono_resolver = ctx;
        bool ok = mono_sig_ok(fd, spec) && term_core_ok(se->term);
        g_cps_mono_resolver = NULL;
        const Symbol *h0[1];
        island_mono = ok && is_cps_island(se->term, h0, 0);
        /* G3b: a NON-island monomorph (an escaping perform / colored callee) is
         * also DK-emittable when its generic is a surviving mono-template -- the
         * taint fixpoint proved its effect stays clean (all monomorphs admissible,
         * no fiber peer shares the effect).  Colored callers route to `__cps`. */
        mono_emit = ok && (island_mono || se->mono_template);
    }

    if (!mono_emit && (!se || !se->in_s)) {
        /* N6.5 readiness measurement (TUR_TRACE_EVICT): for every COLORED function
         * that falls back to the direct emitter, report WHY it was not admitted to
         * the CPS set S, in a category that says whether the general whole-function
         * fallback is being exercised (BODY-*) or the function is on a permanent,
         * non-fallback routing (SIG-*: exported symbol / program entry / ABI-
         * incompatible signature the direct emitter owns).  Deleting the general
         * fallback (gate item 7) is only safe once BODY-* is empty modulo the
         * delimited-control carve-out; this trace is how that gate is checked.
         * Off by default (one getenv per evicted colored fn); no codegen effect. */
        if (getenv("TUR_TRACE_EVICT") && fd->cps_colored) {
            const char *nm = (fd->binding && fd->binding->name) ? fd->binding->name->name : "?";
            const char *cat; const char *why = "";
            if (fd->binding->c_export_name)                cat = "SIG-EXPORT";
            else if (fn_is_main(fd) && !fn_is_d2b_main(fd)) cat = "SIG-MAIN";
            else if (!fn_sig_ok(fd))                        cat = "SIG-REJECT";
            else {
                const CTerm *u = se ? first_unsupported(se->term) : NULL;
                if (u) { cat = "BODY-UNSUPPORTED"; why = u->as.unsupported.why ? u->as.unsupported.why : "?"; }
                else   cat = "BODY-STRUCT-OR-TAINT";
            }
            fprintf(stderr, "[EVICT] %-22s %s %s\n", cat, nm, why);
        }
        return false;   /* fall back */
    }

    if (!g_fwd_done) { emit_forward_decls(ctx, file); g_fwd_done = true; }

    /* An island monomorph is emitted under its concrete clone name; the type
     * spellings + slot tiers in the reused generic CTerm resolve through the
     * active spec (g_cps_mono_resolver, set around the emit below). */
    char *cn = mono_emit ? strdup(spec->clone_name) : raw_name_for_binding(fd->binding);
    const Type *mono_ret = NULL;
    if (mono_emit) {
        g_cps_mono_resolver = ctx;
        mono_ret = &spec->result_type;
        /* Mirror emit_fns.c's by-value spec-param flagging: a concrete by-value
         * ADT-app param (`tur_adt_Option__int`) is passed by value, so a delegated
         * call *through* it (unwrap-or o ...) must NOT re-cross it as the int64
         * carrier.  The CPS emit_params does not set this, so set it here for each
         * such param -- otherwise the direct emitter reconstructs the aggregate
         * from a bogus `(intptr_t)(o)` carrier cast. */
        for (uint32_t i = 0; i < fd->n_params && i < spec->n_args; i++) {
            Type rat = emit_resolve_type(ctx, spec->arg_types[i]);
            const char *pc = emit_type_c_name(ctx, rat);
            if (fd->params[i] && pc && strcmp(pc, "int64_t") != 0
                && (type_uses_carrier_abi(rat) || type_app_is_concrete_adt(&rat)))
                fd->params[i]->emit_byvalue_carrier_abi = true;
        }
    }

    /* Set the function-parameter context so every reference to a parameter --
     * whether emitted by the CPS backend (name_for_binding) or by the direct
     * emitter inside a delegated CT_LETRAW body (emit_call_name / emit_value) --
     * resolves to the same raw (id-less) C name the direct emitter uses.  Without
     * this a fn-value parameter's declaration (id-suffixed) and its delegated use
     * (raw, e.g. `fnv.fn`) diverge; see
     * docs/reported/cps-backend-indirect-call-fatclosure-param-divergence.md.
     * Saved/restored around the whole emission (body + signature + wrapper). */
    Binding **saved_fn_params   = ctx->fn_params;
    uint8_t    saved_n_fn_params = ctx->n_fn_params;
    ctx->fn_params   = fd->params;
    ctx->n_fn_params = fd->n_params;

    /* Emit the body into a temporary buffer, accumulating any lifted reset/shift
     * helpers into `helpers`; then write helpers (which must precede their uses),
     * the signature, and the body. */
    Buf body_buf; buf_init(&body_buf);
    Buf helpers;  buf_init(&helpers);
    int helper_ctr = 0;
    CE ce; memset(&ce, 0, sizeof(ce));
    ce.ctx = ctx; ce.out = &body_buf; ce.helpers = &helpers; ce.indent = 4;
    ce.cur_k = "__kont"; ce.fn_cn = cn; ce.helper_ctr = &helper_ctr;
    ce.ret_ty = mono_ret ? mono_ret : fn_ret_type(fd);   /* KK_RET crossing type (Tier C aggregate return) */
    emit_binder_decls(&ce, se->term);
    emit_term(&ce, se->term);
    buf_putc(&body_buf, '\0');
    buf_putc(&helpers, '\0');

    if (helpers.len > 1) buf_puts(file, helpers.data);

    /* A CT_LETRAW delegation to the direct emitter (e.g. a cloneable-reset)
     * emits its file-scope helper fns into ctx->pending_handler_fns, which the
     * direct-function path flushes ahead of the using function.  A CPS function
     * has its own emission path, so flush those helpers here too -- before the
     * __cps body that references them -- otherwise the helper is defined after
     * its use ('<helper>' undeclared). */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }

    /* ---- CPS body: int64_t <name>__cps(<params>, DK *k) ---- */
    buf_printf(file, "static int64_t %s__cps(", cn);
    emit_params(ctx, file, fd);
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "DK *__kont) {\n");
    buf_puts(file, body_buf.data);
    buf_puts(file, "}\n");
    buf_free(&body_buf);
    buf_free(&helpers);

    /* ---- D2b fixed-ABI entry wrapper: int main(int argc, char **argv) ----
     * A delimited zero-arg `main` (cps-backend-direct-lowering-removal-plan D2b)
     * keeps the program's fixed entry ABI: the wrapper reproduces the direct
     * emitter's `main` prologue (panic-trace flag + the *args* cons build from
     * argv, mirroring emit_fns.c / emit_module.c), seeds the root prompt exactly
     * like the generic wrapper below, trampolines into `main__cps`, and returns
     * the delivered value as the process exit code. */
    if (fn_is_d2b_main(fd)) {
        const Type *mrt = mono_ret ? mono_ret : fn_ret_type(fd);
        bool mvoid = (mrt->kind == TY_NIL);
        buf_puts(file, "int main(int argc, char **argv) {\n");
        if (g_emit_panic_trace)
            buf_puts(file, "    g_panic_trace = 1;\n");
        buf_puts(file, "    /* *args*: build cons list from argv[1..argc-1] */\n");
        buf_puts(file, "    g_tur_args = 0;\n");
        buf_puts(file, "    for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        buf_puts(file, "        typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        buf_puts(file, "        __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        buf_puts(file, "        _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        buf_puts(file, "        _c->next = g_tur_args;\n");
        buf_puts(file, "        g_tur_args = (int64_t)(intptr_t)_c;\n");
        buf_puts(file, "    }\n");
        buf_puts(file, "    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());\n");
        buf_printf(file, "    %s%s__cps(__root);\n", mvoid ? "(void)" : "int64_t __r = ", cn);
        buf_puts(file, "    dk_free(__root);\n");
        if (mvoid) {
            buf_puts(file, "    return 0;\n}\n");
        } else {
            char *ld = slot_load(ctx, mrt->kind, mrt, "__r", true);
            buf_printf(file, "    return (int)(%s);\n}\n", ld);
            free(ld);
        }
        ctx->fn_params   = saved_fn_params;
        ctx->n_fn_params = saved_n_fn_params;
        g_cps_mono_resolver = NULL;
        free(cn);
        return true;
    }

    /* ---- direct->cps entry wrapper: int64_t <name>(<params>) ----
     * The boundary trampoline for an uncolored/direct caller (including `main`)
     * entering a colored function by its plain name unchanged.  It seeds the
     * initial continuation as the implicit root prompt (CPS5.3, DK_ROOT_TAG) on
     * a dk_done()-terminated chain -- the structural equivalent of dk_run_root,
     * so an undelimited capture inside the CPS body reaches program entry -- runs
     * the CPS body, and returns (unwraps) the value delivered to the root.  The
     * seed chain is freed after the body returns; the CPS body never retains it
     * (a captured sub-continuation is always a copy). */
    const Type *rt = mono_ret ? mono_ret : fn_ret_type(fd);
    /* A nil/void return: the entry wrapper is declared `void` (matching the
     * direct emitter's forward decl) and discards the unit the CPS body delivers,
     * rather than returning an int64. */
    bool void_ret = (rt->kind == TY_NIL);
    const char *rety = void_ret ? "void" : binder_ctype_full(ctx, rt->kind, rt);
    buf_printf(file, "__attribute__((unused)) static %s %s(", rety, cn);
    emit_params(ctx, file, fd);
    buf_puts(file, ") {\n");
    buf_puts(file, "    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());\n");
    buf_printf(file, "    %s%s__cps(", void_ret ? "(void)" : "int64_t __r = ", cn);
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        char *pn = name_for_binding(ctx, fd->params[i]);
        buf_puts(file, pn);
        free(pn);
    }
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "__root);\n");
    buf_puts(file, "    dk_free(__root);\n");
    if (void_ret) {
        buf_puts(file, "    return;\n}\n");
    } else {
        /* slot-load the final delivered value back to the real return type.  This
         * is the single-shot root boundary, so a Tier C box is consumed here. */
        char *ld = slot_load(ctx, rt->kind, rt, "__r", true);
        buf_printf(file, "    return %s;\n}\n", ld);
        free(ld);
    }

    ctx->fn_params   = saved_fn_params;
    ctx->n_fn_params = saved_n_fn_params;
    g_cps_mono_resolver = NULL;   /* G3a: end of island-monomorph emit (no-op otherwise) */
    free(cn);
    return true;
}
