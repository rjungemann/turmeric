/* elab_concurrent.c -- threads, async/await, select, panics, and software transactional memory. */
#include "elab_internal.h"

/* Phase T19-B: (thread-spawn closure)
 *
 * Performs a Send-safety check on all variables captured by the closure.
 * Types that are not Send (ref<T>, rc<T>, weak<T>, cont<T>, &T, &mut T)
 * are rejected with TUR-E0010.
 *
 * This form is the elaboration-level gate for cross-thread closure passing.
 * Actual OS-thread spawning is provided by stdlib/thread.tur (T19-C); this
 * form only validates send-safety and returns nil as a compile-time assertion.
 *
 * Usage: (thread-spawn (fn [...] body))
 */
Expr *elab_thread_spawn(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "thread-spawn requires exactly one argument: "
                  "(thread-spawn closure)");
        return NULL;
    }

    Expr *closure_expr = elab_form(e, call->as.list.items[1]);
    if (!closure_expr) return NULL;

    /* Send-check: walk the capture set of any closure argument. */
    if (closure_expr->kind == EX_CLOSURE) {
        struct Closure *cl = closure_expr->as.closure_.closure;
        bool had_error = false;
        for (uint8_t i = 0; i < cl->n_captures; i++) {
            Binding *cap = cl->captures[i];
            if (!type_is_send(cap->type)) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0010_NOT_SEND,
                    "type `%s` cannot be sent across thread boundaries "
                    "(not `Send`); captured variable `%s`",
                    type_name(cap->type), cap->name->name);
                had_error = true;
            }
        }
        if (had_error) return NULL;
    }

    /* Return the closure expression (type flows through; T19-C provides the
     * actual runtime thread-spawn wrapper in stdlib/thread.tur).            */
    return closure_expr;
}

/* Phase T21-F: (async fn-expr) — launch fn-expr (no-arg function) in a new
 * thread; return a TurAsyncTask* as ptr<void> (the future handle).          */
Expr *elab_async(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(async fn-expr) requires exactly one argument");
        return NULL;
    }
    /* CF6: mark that we are elaborating an async body so elab_await can check
     * Send for all bindings in scope at each await point. */
    bool saved_in_async_body = e->in_async_body;
    e->in_async_body = true;
    Expr *fn_expr = elab_form(e, call->as.list.items[1]);
    e->in_async_body = saved_in_async_body;
    if (!fn_expr) return NULL;
    
    /* AW-012 / AW-011B-1: Send-check for async closures.
     * Values captured in async blocks must be Send (can be moved to fiber context).
     * T25: Also check that no effect-handler continuation (k) escapes into an async
     * block, which would cause a resume-on-wrong-fiber runtime error. */
    if (fn_expr->kind == EX_FN || fn_expr->kind == EX_CLOSURE) {
        const struct Closure *cl = NULL;
        if (fn_expr->kind == EX_CLOSURE) {
            cl = fn_expr->as.closure_.closure;
        } else if (fn_expr->kind == EX_FN) {
            cl = fn_expr->as.fn_.fn->closure;
        }
        if (cl) {
            bool had_error = false;
            for (uint8_t i = 0; i < cl->n_captures; i++) {
                Binding *cap = cl->captures[i];
                if (!type_is_send(cap->type)) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0010_NOT_SEND,
                        "type `%s` cannot be sent across thread boundaries "
                        "(not `Send`); captured variable `%s` in async block",
                        type_name(cap->type), cap->name->name);
                    had_error = true;
                }
                /* T25: Prevent continuation escape into async scope.
                 * A handler continuation k cannot be captured by an async block because
                 * the continuation is bound to the fiber where perform was called; resuming
                 * it from a different fiber produces a runtime error. Catching this at
                 * compile time is safer. */
                if (cap->is_continuation) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0017_CONT_ESCAPE_ASYNC,
                        "effect handler continuation `%s` cannot be captured by an async block; "
                        "resuming it from a different fiber would cause a runtime error",
                        cap->name->name);
                    had_error = true;
                }
            }
            if (had_error) return NULL;
        }
    }
    
    Expr *out = expr_new(e->arena, EX_ASYNC, TYPE_PTR_VOID, call->span);
    out->as.async_.fn_expr = fn_expr;
    return out;
}

/* Phase T21-F: (await fut) — block on TurAsyncTask* future; return int64_t. */
Expr *elab_await(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(await fut) requires exactly one argument");
        return NULL;
    }
    /* CF6 (TUR-E0022): conservative Send-across-await check.
     * When inside an inline async closure, every binding currently in scope is
     * treated as live across this await.  Any non-Send binding is a soundness
     * hole: the fiber may resume on a different OS thread, racing with e.g. the
     * rc<T> refcount.  This check applies to inline closures only; pre-defined
     * functions passed to (async fn) are not re-elaborated here. */
    if (e->in_async_body) {
        bool had_error = false;
        for (Scope *sc = e->scope; sc != NULL; sc = sc->parent) {
            for (uint32_t i = 0; i < sc->n; i++) {
                Binding *b = sc->bindings[i];
                if (!type_is_send(b->type)) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0022_AWAIT_LIVE_NOT_SEND,
                        "binding `%s` of type `%s` is not Send and may be live "
                        "across this await point; all bindings in scope at an "
                        "await must be Send (1.0 conservative check)",
                        b->name->name, type_name(b->type));
                    had_error = true;
                }
            }
        }
        if (had_error) return NULL;
    }
    Expr *fut_expr = elab_form(e, call->as.list.items[1]);
    if (!fut_expr) return NULL;
    Expr *out = expr_new(e->arena, EX_AWAIT, TYPE_INT, call->span);
    out->as.await_.fut_expr = fut_expr;
    return out;
}

/* Phase SEL1: (select ((ch :recv v) body) ... (:default body))
 * Waiter-based fair multi-channel select. Returns the value of the selected clause body.
 * All channel expressions must be ptr<void>. Clause bodies must be type-compatible. */
Expr *elab_select(Elab *e, const Form *call) {
    /* call->as.list.items[0] = "select"
     * call->as.list.items[1..n] = clauses
     * Each clause is a 2-element list:
     *   ((chan :recv v) body)   -- recv clause
     *   ((chan :send val) body) -- send clause
     *   (:default body)        -- default arm (only keyword at head, rest is body)
     */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(select ...) requires at least one clause");
        return NULL;
    }
    uint32_t n_clauses_raw = call->as.list.len - 1;
    SelectClauseEntry *clauses = (SelectClauseEntry *)arena_alloc(
        e->arena, n_clauses_raw * sizeof(SelectClauseEntry));
    uint32_t n_clauses = 0;
    int has_default = 0;
    Expr *default_body = NULL;
    const Symbol *sym_default = intern_cstr(e->st, "default");

    for (uint32_t ci = 0; ci < n_clauses_raw; ci++) {
        Form *clause_form = call->as.list.items[1 + ci];
        /* Must be a list with exactly 2 items: descriptor and body */
        if (clause_form->tag != F_LIST || clause_form->as.list.len != 2) {
            diag_emit(DIAG_ERROR, clause_form->span,
                      "select clause must be ((chan :recv v) body) or ((chan :send val) body) or (:default body)");
            return NULL;
        }
        Form *desc      = clause_form->as.list.items[0];
        Form *body_form = clause_form->as.list.items[1];

        /* Check for (:default body) */
        if (desc->tag == F_KEYWORD && desc->as.sym == sym_default) {
            if (has_default) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "select: at most one :default arm is allowed");
                return NULL;
            }
            has_default = 1;
            default_body = elab_form(e, body_form);
            if (!default_body) return NULL;
            continue;
        }

        /* Channel clause: desc must be (chan :recv v) or (chan :send val) */
        if (desc->tag != F_LIST || desc->as.list.len < 2) {
            diag_emit(DIAG_ERROR, desc->span,
                      "select channel descriptor must be (chan :recv binding) or (chan :send val)");
            return NULL;
        }
        Form *chan_form = desc->as.list.items[0];
        Form *op_form   = desc->as.list.items[1];

        if (op_form->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, op_form->span,
                      "select clause operation must be :recv or :send");
            return NULL;
        }
        int op;
        if (op_form->as.sym == e->sym_recv) {
            op = 0;
        } else if (op_form->as.sym == e->sym_send) {
            op = 1;
        } else {
            diag_emit(DIAG_ERROR, op_form->span,
                      "select clause operation must be :recv or :send");
            return NULL;
        }

        /* Elaborate channel expression */
        Expr *chan_expr = elab_form(e, chan_form);
        if (!chan_expr) return NULL;

        Expr *send_val = NULL;
        Binding *recv_bind = NULL;

        if (op == 0) {
            /* recv: (chan :recv binding) -- 3 elements */
            if (desc->as.list.len != 3) {
                diag_emit(DIAG_ERROR, desc->span,
                          "select :recv clause descriptor: (chan :recv binding)");
                return NULL;
            }
            Form *bind_form = desc->as.list.items[2];
            if (bind_form->tag != F_SYM) {
                diag_emit(DIAG_ERROR, bind_form->span,
                          "select :recv binding must be a symbol");
                return NULL;
            }
            recv_bind = binding_new(e, bind_form->as.sym, TYPE_INT, false, false, body_form->span);
        } else {
            /* send: (chan :send val) -- 3 elements */
            if (desc->as.list.len != 3) {
                diag_emit(DIAG_ERROR, desc->span,
                          "select :send clause descriptor: (chan :send val)");
                return NULL;
            }
            Form *val_form = desc->as.list.items[2];
            send_val = elab_form(e, val_form);
            if (!send_val) return NULL;
        }

        /* Elaborate body with recv binding in scope (if recv clause) */
        Expr *body_expr;
        if (op == 0 && recv_bind) {
            /* Introduce a scope with the received value binding */
            Scope recv_scope;
            scope_init(&recv_scope, e->scope);
            e->scope = &recv_scope;
            scope_add(&recv_scope, recv_bind);
            body_expr = elab_form(e, body_form);
            e->scope = recv_scope.parent;
            scope_free(&recv_scope);
        } else {
            body_expr = elab_form(e, body_form);
        }
        if (!body_expr) return NULL;

        SelectClauseEntry *sc = &clauses[n_clauses++];
        sc->chan          = chan_expr;
        sc->op            = op;
        sc->send_val      = send_val;
        sc->recv_binding  = recv_bind;
        sc->body          = body_expr;
    }

    if (n_clauses == 0 && !has_default) {
        diag_emit(DIAG_ERROR, call->span,
                  "select: requires at least one channel clause");
        return NULL;
    }

    /* Determine return type from first clause body (or default body) */
    Type result_type = TYPE_INT;
    if (n_clauses > 0) result_type = clauses[0].body->type;
    else if (default_body) result_type = default_body->type;

    Expr *out = expr_new(e->arena, EX_SELECT, result_type, call->span);
    out->as.select_.clauses      = clauses;
    out->as.select_.n_clauses    = n_clauses;
    out->as.select_.has_default  = has_default;
    out->as.select_.default_body = default_body;
    return out;
}

/* Phase R2: (panic msg) — print msg to stderr, then abort.
 * msg must be of type :cstr (or a string literal).
 * Return type is TYPE_NEVER (diverging; caller never observes a value). */
Expr *elab_panic(Elab *e, const Form *call) {
    /* Phase R6: Lint panic usage */
    if (g_lint_panic && e->fn_body_depth > 0) {
        /* We're inside a function body - check if it's main or a test function */
        bool in_test_or_main = false;
        if (e->current_fn_name) {
            if (strcmp(e->current_fn_name->name, "main") == 0) {
                in_test_or_main = true;
            } else if (strncmp(e->current_fn_name->name, "test-", 5) == 0) {
                in_test_or_main = true;
            }
        }
        if (!in_test_or_main) {
            diag_emit(DIAG_WARNING, call->span,
                      "panic called outside of main or test function; consider using Result instead");
        }
    }
    
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic msg) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC, TYPE_NEVER, call->span);
    out->as.panic_.payload = payload;
    return out;
}

/* Phase R2: (panic-with payload) — panic with typed payload.
 * payload is any value; its TypeKind is stored for catch-panic-of filtering.
 * Return type is TYPE_NEVER (diverging). */
Expr *elab_panic_with(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-with payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_WITH, TYPE_NEVER, call->span);
    out->as.panic_with_.payload = payload;
    return out;
}

/* Phase R2: (catch-unwind thunk) — catch any panic at a boundary.
 * thunk is a nullary function; returns result<T, panic-payload>.
 * In v1, result is ptr<void> and lowering uses tur_catch_unwind from runtime. */
Expr *elab_catch_unwind(Elab *e, const Form *call) {
    /* Phase R6: Lint catch_unwind usage */
    if (g_lint_panic) {
        diag_emit(DIAG_WARNING, call->span,
                  "catch_unwind should only be used at effect boundaries, not for normal error handling; consider using Result instead");
    }
    
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(catch-unwind thunk) requires exactly one argument");
        return NULL;
    }
    Expr *thunk = elab_form(e, call->as.list.items[1]);
    if (!thunk) return NULL;
    /* Result type is result<T, panic-payload> - ptr<void> in v1 */
    Expr *out = expr_new(e->arena, EX_CATCH_UNWIND, TYPE_PTR_VOID, call->span);
    out->as.catch_unwind_.thunk = thunk;
    return out;
}

/* Phase R2: (catch-panic-of Type thunk) — catch panics of a specific type.
 * Type is a type identifier (symbol); thunk is a nullary function.
 * Returns result<T, panic-payload> if type matches, otherwise re-panics.
 * In v1, result is ptr<void> and lowering uses tur_catch_panic_of from runtime. */
Expr *elab_catch_panic_of(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(catch-panic-of Type thunk) requires exactly two arguments");
        return NULL;
    }
    /* First arg: type identifier */
    Form *type_form = call->as.list.items[1];
    TypeKind type_kind = TY_UNKNOWN;
    if (type_form->tag == F_SYM) {
        const char *type_name = type_form->as.sym->name;
        type_kind = typekind_from_name(type_name);
    }
    /* Second arg: thunk */
    Expr *thunk = elab_form(e, call->as.list.items[2]);
    if (!thunk) return NULL;
    /* Result type is result<T, panic-payload> - ptr<void> in v1 */
    Expr *out = expr_new(e->arena, EX_CATCH_PANIC_OF, TYPE_PTR_VOID, call->span);
    out->as.catch_panic_of_.type_kind = type_kind;
    out->as.catch_panic_of_.thunk = thunk;
    return out;
}

/* Phase S4: (throw expr) -- raise a catchable exception; always TY_NEVER */
Expr *elab_throw(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "throw: expected exactly one argument");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    Expr *out = expr_new(e->arena, EX_THROW, TYPE_NEVER, call->span);
    out->as.throw_.value = val;
    return out;
}

/* Phase S4: (try body (catch [bind] handler) ...)
 * Each catch clause is (catch [name] body) or (catch [name :Type] body).
 * The [name] vector is parsed directly as a binding pattern (not a value). */
Expr *elab_try_catch(Elab *e, const Form *call) {
    uint32_t len = call->as.list.len;
    if (len < 2) {
        diag_emit(DIAG_ERROR, call->span, "try: expected body");
        return NULL;
    }
    /* Elaborate the body */
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;

    /* Parse catch clauses: (catch [binding] handler) */
    uint32_t n_catches = len - 2;
    Binding  **bindings = n_catches ? (Binding **)arena_alloc(e->arena, n_catches * sizeof(Binding *)) : NULL;
    Expr     **handlers = n_catches ? (Expr **)arena_alloc(e->arena, n_catches * sizeof(Expr *))    : NULL;

    for (uint32_t i = 0; i < n_catches; i++) {
        Form *clause = call->as.list.items[i + 2];
        /* clause should be (catch [bind] handler) */
        if (!clause || clause->tag != F_LIST || clause->as.list.len < 3) {
            diag_emit(DIAG_ERROR, clause ? clause->span : call->span,
                      "try: catch clause must be (catch [binding] handler)");
            return NULL;
        }
        /* Second element: binding vector [name] or [name :Type] */
        Form *bind_vec = clause->as.list.items[1];
        Binding *catch_bind = NULL;
        if (bind_vec && bind_vec->tag == F_VEC && bind_vec->as.list.len >= 1) {
            Form *name_form = bind_vec->as.list.items[0];
            if (name_form && name_form->tag == F_SYM) {
                /* Create a fresh binding for the caught value */
                catch_bind = binding_new(e, name_form->as.sym, TYPE_INT, false, false, name_form->span);
            }
        }
        bindings[i] = catch_bind;

        /* Push scope with the binding, elaborate handler */
        Scope catch_scope;
        Scope *saved_scope = e->scope;
        scope_init(&catch_scope, e->scope);
        e->scope = &catch_scope;
        if (catch_bind) scope_add(&catch_scope, catch_bind);
        Expr *handler = elab_form(e, clause->as.list.items[2]);
        e->scope = saved_scope;
        scope_free(&catch_scope);
        if (!handler) return NULL;
        handlers[i] = handler;
    }

    /* Result type: use body type (simplification; could be union with handler types) */
    Expr *out = expr_new(e->arena, EX_TRY_CATCH, body->type, call->span);
    out->as.try_catch_.body           = body;
    out->as.try_catch_.catch_bindings = bindings;
    out->as.try_catch_.catch_handlers = handlers;
    out->as.try_catch_.n_catches      = n_catches;
    return out;
}

/* Phase R2: (panic-payload-type p) — get the TypeKind tag from a panic payload. */
Expr *elab_panic_payload_type(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-type payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_TYPE, TYPE_INT, call->span);
    out->as.panic_payload_type_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-value p) — get the boxed value from a panic payload. */
Expr *elab_panic_payload_value(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-value payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_VALUE, TYPE_PTR_VOID, call->span);
    out->as.panic_payload_value_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-file p) — get the source file from a panic payload. */
Expr *elab_panic_payload_file(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-file payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_FILE, TYPE_CSTR, call->span);
    out->as.panic_payload_file_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-line p) — get the source line from a panic payload. */
Expr *elab_panic_payload_line(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-line payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_LINE, TYPE_INT, call->span);
    out->as.panic_payload_line_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-downcast p Type) — cast panic payload to a specific type.
 * Returns the boxed value if type matches, NULL otherwise. */
Expr *elab_panic_payload_downcast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-downcast payload Type) requires exactly two arguments");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    /* Second arg: type identifier */
    Form *type_form = call->as.list.items[2];
    TypeKind target_type = TY_UNKNOWN;
    if (type_form->tag == F_SYM) {
        const char *type_name = type_form->as.sym->name;
        target_type = typekind_from_name(type_name);
    }
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_DOWNS, TYPE_PTR_VOID, call->span);
    out->as.panic_payload_downs_.payload = payload;
    out->as.panic_payload_downs_.target_type = target_type;
    return out;
}

Expr *elab_stm(Elab *e, const Form *call) {
    if (call->as.list.len < 1) {
        diag_emit(DIAG_ERROR, call->span, "stm requires at least one body expression");
        return NULL;
    }

    /* Save previous stm state */
    bool prev_in_stm = elab_in_stm;
    elab_in_stm = true;

    /* Elaborate body expressions */
    Expr **body = arena_alloc(e->arena, (call->as.list.len - 1) * sizeof(Expr *));
    uint32_t n_body = 0;
    for (uint32_t i = 1; i < call->as.list.len; i++) {
        Expr *expr = elab_form(e, call->as.list.items[i]);
        if (!expr) {
            elab_in_stm = prev_in_stm;
            return NULL;
        }
        body[n_body++] = expr;
    }

    /* Create STM block expression */
    /* The type is the type of the last expression, or NIL if empty */
    Type result_type = TYPE_NIL;
    if (n_body > 0) {
        result_type = body[n_body - 1]->type;
    }
    Expr *result = expr_new(e->arena, EX_STM, result_type, call->span);
    result->as.stm_.body = body;
    result->as.stm_.n_body = n_body;

    /* Restore stm state */
    elab_in_stm = prev_in_stm;

    return result;
}

Expr *elab_atomically(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "atomically requires exactly one stm block argument");
        return NULL;
    }

    Form *arg = call->as.list.items[1];

    /* MS2: Set atomically flag so elab_resume can detect TUR-E0502 */
    bool prev_in_atomically = elab_in_atomically;
    elab_in_atomically = true;

    /* The argument should be an stm block or something that evaluates to one */
    /* For v1, we just wrap it in atomically */
    Expr *stm_expr = elab_form(e, arg);
    elab_in_atomically = prev_in_atomically;
    if (!stm_expr) return NULL;

    /* Check if it's already an stm block */
    if (stm_expr->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->span, "atomically requires an stm block as argument");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_ATOMICALLY, stm_expr->type, call->span);
    result->as.atomically_.stm_expr = stm_expr;
    return result;
}

Expr *elab_retry(Elab *e, const Form *call) {
    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "retry can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_RETRY, TYPE_NIL, call->span);
    return result;
}

Expr *elab_check(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "check requires exactly one condition argument");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "check can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;

    /* Check should return bool */
    if (cond->type.kind != TY_BOOL) {
        diag_emit(DIAG_ERROR, call->span, "check condition must be a bool");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_CHECK, TYPE_NIL, call->span);
    result->as.check_.cond = cond;
    return result;
}

Expr *elab_or_else(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "or-else requires exactly two stm block arguments");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "or-else can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *stm1 = elab_form(e, call->as.list.items[1]);
    Expr *stm2 = elab_form(e, call->as.list.items[2]);
    if (!stm1 || !stm2) return NULL;

    if (stm1->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span, "or-else first argument must be an stm block");
        return NULL;
    }
    if (stm2->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->as.list.items[2]->span, "or-else second argument must be an stm block");
        return NULL;
    }

    /* or-else result type is the common type of both branches */
    /* For simplicity, use stm2's type (the fallback branch) */
    Type result_type = stm2->type;
    /* If stm2 returns nil, try stm1's type */
    if (result_type.kind == TY_NIL) {
        result_type = stm1->type;
    }

    Expr *result = expr_new(e->arena, EX_OR_ELSE, result_type, call->span);
    result->as.or_else_.stm1 = stm1;
    result->as.or_else_.stm2 = stm2;
    return result;
}

Expr *elab_tvar_new(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "TVar/new requires exactly one initial value argument");
        return NULL;
    }

    Expr *init = elab_form(e, call->as.list.items[1]);
    if (!init) return NULL;

    /* TVar::new returns a TVar pointer (ptr), not the type of the initial value */
    Expr *result = expr_new(e->arena, EX_TVAR_NEW, TYPE_PTR_VOID, call->span);
    result->as.tvar_new_.init = init;
    return result;
}

Expr *elab_tvar_read(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "TVar/read requires exactly one TVar argument");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/read can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    if (!tvar) return NULL;

    /* TVar/read returns the value stored in the TVar, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_READ, TYPE_PTR_VOID, call->span);
    result->as.tvar_read_.tvar = tvar;
    return result;
}

Expr *elab_tvar_write(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/write requires exactly two arguments: tvar and value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/write can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!tvar || !value) return NULL;

    Expr *result = expr_new(e->arena, EX_TVAR_WRITE, TYPE_NIL, call->span);
    result->as.tvar_write_.tvar = tvar;
    result->as.tvar_write_.value = value;
    return result;
}

Expr *elab_tvar_modify(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/modify requires exactly two arguments: tvar and function");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/modify can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *fn = elab_form(e, call->as.list.items[2]);
    if (!tvar || !fn) return NULL;

    /* TVar/modify returns the old value, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_MODIFY, TYPE_PTR_VOID, call->span);
    result->as.tvar_modify_.tvar = tvar;
    result->as.tvar_modify_.fn = fn;
    return result;
}

Expr *elab_tvar_swap(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/swap requires exactly two arguments: tvar and new value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/swap can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *new_val = elab_form(e, call->as.list.items[2]);
    if (!tvar || !new_val) return NULL;

    /* TVar/swap returns the old value, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_SWAP, TYPE_PTR_VOID, call->span);
    result->as.tvar_swap_.tvar = tvar;
    result->as.tvar_swap_.new_val = new_val;
    return result;
}

Expr *elab_tvar_cas(Elab *e, const Form *call) {
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span, "TVar/cas requires exactly three arguments: tvar, old value, new value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/cas can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *old_val = elab_form(e, call->as.list.items[2]);
    Expr *new_val = elab_form(e, call->as.list.items[3]);
    if (!tvar || !old_val || !new_val) return NULL;

    Expr *result = expr_new(e->arena, EX_TVAR_CAS, TYPE_BOOL, call->span);
    result->as.tvar_cas_.tvar = tvar;
    result->as.tvar_cas_.old_val = old_val;
    result->as.tvar_cas_.new_val = new_val;
    return result;
}
