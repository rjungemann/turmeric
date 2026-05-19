/* elab_sessions.c -- session type channel operations (-Xsessions). */
#include "elab_internal.h"

/* Helper: return a TY_SESSION Expr wrapping the given protocol Type, using
 * EX_INLINE_C as a placeholder (codegen is deferred to SS2).  The returned
 * expression carries the correct type for subsequent type-level checks. */
static Expr *session_placeholder(Elab *e, Type proto_type, Span span) {
    Type *proto = (Type *)arena_alloc(e->arena, sizeof(Type));
    *proto = proto_type;
    Type sess_type = type_session(proto);
    Expr *out = expr_new(e->arena, EX_INLINE_C, sess_type, span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("NULL /*session-placeholder*/", 28);
    ic->return_type = sess_type;
    ic->captures = NULL;
    ic->n_captures = 0;
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (make-session P) — create a dual pair [Session[P], Session[Dual[P]]].
 * Duality checking is deferred to SS1; for SS0b we return a placeholder. */
Expr *elab_session_make(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-session requires a protocol: (make-session Protocol)");
        return NULL;
    }
    /* Return TY_INT as a placeholder; SS2 will emit the actual pair-creation code. */
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_INT, call->span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("0 /*make-session-placeholder*/", 30);
    ic->return_type = TYPE_INT;
    ic->captures = NULL;
    ic->n_captures = 0;
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* Helper: validate that an expression is a Session[...] channel and extract
 * its inner protocol type.  On failure, emits a diagnostic and returns NULL. */
static Type *session_protocol_of(Elab *e, Expr *chan, const char *op, Span span) {
    if (!chan) return NULL;
    if (chan->type.kind != TY_SESSION) {
        diag_emit_with_code(DIAG_ERROR, span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "%s requires a Session[...] channel, got %s",
                            op, type_name(chan->type));
        return NULL;
    }
    return chan->type.as.session_.fst;
}

/* (send chan val) — consume chan : Session[Send[T, Q]]; return chan' : Session[Q].
 * Emits TUR-E0212 if chan's protocol is not Send[...]. */
Expr *elab_session_send(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "send requires two arguments: (send chan val)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;
    Expr *val  = elab_form(e, call->as.list.items[2]);
    if (!val) return NULL;

    Type *proto = session_protocol_of(e, chan, "send", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_SEND) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "send is not valid on Session[%s] -- "
                            "this endpoint's next action is not a send",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed (it's linear) */
    if (chan->kind == EX_VAR) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Return Session[Q] where Q = proto->as.session_.snd */
    return session_placeholder(e, *proto->as.session_.snd, call->span);
}

/* (recv chan) — consume chan : Session[Recv[T, Q]]; return placeholder for [T, Session[Q]].
 * Emits TUR-E0212 if chan's protocol is not Recv[...]. */
Expr *elab_session_recv(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "recv requires one argument: (recv chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "recv", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_RECV) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "recv is not valid on Session[%s] -- "
                            "this endpoint's next action is not a recv",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    if (chan->kind == EX_VAR) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Return TY_INT placeholder; SS2 will emit the actual pair-creation code. */
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_INT, call->span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("0 /*recv-placeholder*/", 22);
    ic->return_type = TYPE_INT;
    ic->captures = NULL;
    ic->n_captures = 0;
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (close chan) — consume chan : Session[Close]; return nil.
 * Emits TUR-E0212 if chan's protocol is not Close. */
Expr *elab_session_close(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "close requires one argument: (close chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "close", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_CLOSE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "close is not valid on Session[%s] -- "
                            "protocol is not yet complete",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    if (chan->kind == EX_VAR) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("0 /*close-placeholder*/", 23);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (offer chan) — consume chan : Session[Branch[P, Q]]; return Either(Session[P], Session[Q]).
 * Emits TUR-E0212 if chan's protocol is not Branch[...]. */
Expr *elab_session_offer(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "offer requires one argument: (offer chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "offer", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_BRANCH) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "offer is not valid on Session[%s] -- "
                            "only Branch sessions support offer",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    if (chan->kind == EX_VAR) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Return TY_INT placeholder; SS2 will emit the ADT construction. */
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_INT, call->span);
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("0 /*offer-placeholder*/", 23);
    ic->return_type = TYPE_INT;
    ic->captures = NULL;
    ic->n_captures = 0;
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (choose-left chan) — consume chan : Session[Choose[P, Q]]; return Session[P].
 * Emits TUR-E0212 if chan's protocol is not Choose[...]. */
Expr *elab_session_choose_left(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "choose-left requires one argument: (choose-left chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "choose-left", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_CHOOSE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "choose-left is not valid on Session[%s] -- "
                            "only Choose sessions support choose-left",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    if (chan->kind == EX_VAR) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Return Session[P] where P = proto->as.session_.fst (left branch) */
    return session_placeholder(e, *proto->as.session_.fst, call->span);
}

/* (choose-right chan) — consume chan : Session[Choose[P, Q]]; return Session[Q].
 * Emits TUR-E0212 if chan's protocol is not Choose[...]. */
Expr *elab_session_choose_right(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "choose-right requires one argument: (choose-right chan)");
        return NULL;
    }
    Expr *chan = elab_form(e, call->as.list.items[1]);
    if (!chan) return NULL;

    Type *proto = session_protocol_of(e, chan, "choose-right", call->span);
    if (!proto) return NULL;

    if (proto->kind != TY_CHOOSE) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0212_SESSION_PROTO_MISMATCH,
                            "choose-right is not valid on Session[%s] -- "
                            "only Choose sessions support choose-right",
                            type_name(*proto));
        return NULL;
    }

    /* Mark chan as consumed */
    if (chan->kind == EX_VAR) {
        binding_mark_moved(chan->as.var.binding, call->span);
    }

    /* Return Session[Q] where Q = proto->as.session_.snd (right branch) */
    return session_placeholder(e, *proto->as.session_.snd, call->span);
}
