/* emit_core.c -- shared codegen helpers: naming, atoms, builtins, captures. */
#include "emit_internal.h"

/* ------------ helpers ------------ */

/* Phase M0: Flatten EX_PROGRAM items into a contiguous array, expanding any
 * EX_DEFMODULE nodes into their body items. The returned array is malloc'd
 * and must be freed by the caller. */
const Expr **flatten_program_items(const Expr *program, uint32_t *out_n) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEFMODULE)
            total += e->as.defmodule_.mod->n_body;
        else
            total += 1;
    }
    const Expr **flat = (const Expr **)malloc(total * sizeof(Expr *));
    if (!flat && total > 0) { fprintf(stderr, "tur: oom\n"); abort(); }
    uint32_t k = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEFMODULE) {
            DefModule *mod = e->as.defmodule_.mod;
            for (uint32_t j = 0; j < mod->n_body; j++)
                flat[k++] = mod->body[j];
        } else {
            flat[k++] = e;
        }
    }
    *out_n = total;
    return flat;
}

/* Helper to create a Type from TypeKind (mirrors the one in types.c). */
Type emit_type_from_kind(TypeKind k) {
    Type t = {0};
    t.kind = k;
    t.as.fn.arity = 0;
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    return t;
}

/* ASan/LSan plan (Option C): allocate transient Type scratch from the emit
 * pass's arena when one is available, falling back to malloc otherwise. The
 * arena is bulk-freed at the end of emit_program / emit_implementation, so
 * these nodes are no longer leaked (they are reachable via the arena until
 * the compile finishes, which is exactly their lifetime). */
static void *emit_type_scratch(EmitCtx *ctx, size_t size) {
    if (ctx && ctx->type_arena) return arena_alloc(ctx->type_arena, size);
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    return p;
}

static bool emit_find_abi_binding(const EmitAbiSpecialization *spec,
                                  const char *name, uint8_t *out_idx) {
    if (!spec || !name) return false;
    for (uint8_t i = 0; i < spec->n_bindings; i++) {
        if (spec->bindings[i].name && strcmp(spec->bindings[i].name, name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

Type emit_resolve_type(EmitCtx *ctx, Type t) {
    const EmitAbiSpecialization *spec = ctx ? ctx->current_abi_specialization : NULL;
    if (!spec) return t;
    switch (t.kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t.as.tyvar_.name && emit_find_abi_binding(spec, t.as.tyvar_.name, &idx)) {
                return spec->bindings[idx].type;
            }
            return t;
        }
        case TY_APP: {
            if (!t.as.app.fn || !t.as.app.arg) return t;
            Type fn = emit_resolve_type(ctx, *t.as.app.fn);
            Type arg = emit_resolve_type(ctx, *t.as.app.arg);
            Type out = t;
            out.as.app.fn = (Type *)emit_type_scratch(ctx, sizeof(Type));
            out.as.app.arg = (Type *)emit_type_scratch(ctx, sizeof(Type));
            *out.as.app.fn = fn;
            *out.as.app.arg = arg;
            return out;
        }
        case TY_UNION: {
            Type out = t;
            if (t.as.union_.n_members == 0 || !t.as.union_.members) return out;
            Type **members = (Type **)emit_type_scratch(ctx, t.as.union_.n_members * sizeof(Type *));
            out.as.union_.members = members;
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                members[i] = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *members[i] = emit_resolve_type(ctx, *t.as.union_.members[i]);
            }
            return out;
        }
        case TY_INTERSECTION: {
            Type out = t;
            if (t.as.intersection_.n_members == 0 || !t.as.intersection_.members) return out;
            Type **members = (Type **)emit_type_scratch(ctx, t.as.intersection_.n_members * sizeof(Type *));
            out.as.intersection_.members = members;
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                members[i] = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *members[i] = emit_resolve_type(ctx, *t.as.intersection_.members[i]);
            }
            return out;
        }
        default:
            return t;
    }
}

const char *emit_type_c_name(EmitCtx *ctx, Type t) {
    return type_c_name(emit_resolve_type(ctx, t));
}

/* fn-typed-return: does this body statically evaluate to a *thin* (bare,
 * non-capturing) function pointer?
 *
 * A non-capturing fn literal is lifted to a top-level function and the body
 * becomes an EX_VAR referencing that global (non-boxed TY_FN) binding -- a
 * bare C function pointer value.  By contrast a capturing EX_CLOSURE is a fat
 * box, and a call/local-binding result is not statically known to be thin (it
 * may be a fat box typed as a plain fn, e.g. the value a capturing-closure
 * constructor returns).  Only the thin case may be typed as a fn pointer; the
 * rest stay on the existing int64_t/void* carrier.  do/let/if/ascribe wrappers
 * are transparent; an `if` is thin only when both arms are thin. */
static bool body_yields_thin_fn(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_FN:
            return true;
        case EX_VAR:
            return e->as.var.binding && e->as.var.binding->is_global &&
                   e->as.var.binding->type.kind == TY_FN &&
                   !e->as.var.binding->type.as.fn.boxed;
        case EX_ASCRIBE:
            return body_yields_thin_fn(e->as.ascribe_.inner);
        case EX_DO:
            for (int i = (int)e->as.do_.n - 1; i >= 0; i--)
                if (e->as.do_.items[i]->kind != EX_DEFER)
                    return body_yields_thin_fn(e->as.do_.items[i]);
            return false;
        case EX_LET:
        case EX_LETREC:
            return body_yields_thin_fn(e->as.let_.body);
        case EX_IF:
            return e->as.if_.else_or_null &&
                   body_yields_thin_fn(e->as.if_.then_) &&
                   body_yields_thin_fn(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* fn-typed-return: a `defn` whose declared return type is a concrete,
 * non-boxed function type returns a *function value* (a bare/thin function
 * pointer), not a value of the function's result type.  type_c_name(TY_FN)
 * lowers a non-boxed primitive-result fn to its result type's C name (the
 * "bare function reference returns its result" convention used by inline-C
 * function values), which is wrong for a producer that returns the closure:
 * the C signature would say `double` while the body returns `double (*)(double)`.
 *
 * Return the matching fn-ptr typedef name (e.g. tur_fnptr_double_double_t) so
 * the signature, forward declaration, and the thin-fn-pointer let binding at
 * the consumer all agree.  Returns NULL when the return type is not a concrete
 * thin function value (boxed closures, dict-dispatched method impls, bodies
 * that do not statically yield a thin fn pointer, non-primitive arg/result
 * kinds, and non-fn returns all fall back to the existing lowering). */
const char *emit_fn_return_typedef(const FnDef *fd, const Type *rft) {
    if (!fd || !rft || rft->kind != TY_FN) return NULL;
    /* A boxed first-class closure is the void* carrier, not a thin fn ptr. */
    if (rft->as.fn.boxed) return NULL;
    /* A typeclass-method impl is reached through its dictionary, whose field
     * type lowers the fn-typed return via type_c_name (the int64_t fn carrier)
     * -- see the dict-struct emission in emit_stmt.c.  The impl signature must
     * match that field type, so it must NOT use the thin fn-ptr typedef.  The
     * generated impl name is `__inst_<class>_<method>_<typeargs>`. */
    if (fd->binding && fd->binding->name && fd->binding->name->name &&
        strncmp(fd->binding->name->name, "__inst_", 7) == 0)
        return NULL;
    /* Only a body that statically yields a thin (non-capturing) function
     * pointer may be typed as one.  Capturing closures and other fn-typed
     * values stay on the existing int64_t/void* carrier so the box is not
     * mistyped as a bare function pointer. */
    if (!body_yields_thin_fn(fd->body)) return NULL;
    /* register_fn_ptr_typedef returns NULL unless every arg/result kind is a
     * concrete primitive -- exactly the thin-fn-pointer case. */
    return register_fn_ptr_typedef(rft);
}

/* KB-021: the single arbiter of which types may use the int64_t carrier ABI
 * (a heap-pointer handle) for dictionary-dispatched typeclass methods.
 *
 * Carrier-ABI types:
 *   - TY_APP            (e.g. (Vec int), (Box int)) -- parametric container apps
 *   - TY_ADT            (algebraic data types, already int64_t in type_c_name)
 *   - parametric struct (TY_STRUCT with n_type_params > 0)
 *
 * These types have two coexisting C value representations: the int64_t carrier
 * (returned by carrier-ABI stdlib functions like (vec-new)/(some x)) and a
 * by-value concrete struct (a struct constructor literal, or an ABI-specialized
 * clone that returns the concrete type).  The dictionary-dispatch callsites and
 * the let-binding declaration path both consult this predicate (together with
 * the per-expression representation check) so they agree about whether a value
 * is already a carrier or a by-value aggregate that must be bridged. */
bool type_uses_carrier_abi(Type t) {
    /* SC7: a transparent int newtype has a single int64 representation -- it is
     * not a carrier-ABI aggregate, so no spill/box/deref bridging applies. */
    if (type_is_transparent_int_newtype(t)) return false;
    if (t.kind == TY_APP || t.kind == TY_ADT) return true;
    if (t.kind == TY_STRUCT && t.as.struct_.def &&
        t.as.struct_.def->n_type_params > 0) return true;
    return false;
}

/* RT/SC5: see emit_internal.h.  A by-value (non-carrier) struct body type is
 * the signal: the declared return is the dispatch tyvar (which would otherwise
 * emit the int64_t carrier), but the instance resolves it to a concrete struct.
 * For carrier-ABI bodies (ADT, type-app, parametric struct) the int64_t carrier
 * is already consistent, so no override is needed.  For non-struct bodies (int,
 * cstr, inline-C TY_NIL, type variables) there is nothing to override.  The
 * override is idempotent for functions that already declare a concrete struct
 * return (it re-emits the same struct type). */
Type emit_carrier_return_override(const FnDef *fd) {
    Type none = (Type){ 0 };
    none.kind = TY_UNKNOWN;
    if (!fd || !fd->body) return none;
    Type bt = fd->body->type;
    if (bt.kind == TY_STRUCT && !type_uses_carrier_abi(bt)) return bt;
    return none;
}

void indent_buf(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, ' ');
}

/* Phase R1: Helper to check if an expression is fully divergent (never
 * produces a value).  Used by emit_let_value to decide whether to assign
 * the body's emitted value to the let's result tmp: a divergent body
 * emits no value to assign. */
bool expr_is_divergent(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_THROW:
        case EX_DISCONTINUE:
            return true;
        case EX_DO:
            if (e->as.do_.n == 0) return false;
            /* find last non-defer item */
            for (int i = (int)e->as.do_.n - 1; i >= 0; i--) {
                if (e->as.do_.items[i]->kind != EX_DEFER) {
                    return expr_is_divergent(e->as.do_.items[i]);
                }
            }
            return false;
        case EX_LET:
            return expr_is_divergent(e->as.let_.body);
        case EX_IF:
            if (!e->as.if_.else_or_null) return false;
            return expr_is_divergent(e->as.if_.then_) &&
                   expr_is_divergent(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* Phase 3/4: Helper to check if an expression contains return or panic */
bool expr_contains_return_or_throw(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_THROW:
            return true;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_contains_return_or_throw(e->as.do_.items[i])) {
                    return true;
                }
            }
            return false;
        case EX_LET:
            /* Check body for return/throw */
            return expr_contains_return_or_throw(e->as.let_.body);
        case EX_IF:
            /* Check both branches */
            if (expr_contains_return_or_throw(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null) {
                return expr_contains_return_or_throw(e->as.if_.else_or_null);
            }
            return false;
        case EX_WHILE:
            /* Check body */
            return expr_contains_return_or_throw(e->as.while_.body);
        /* Phase 19: Algebraic effects - discontinue is like throw */
        case EX_DISCONTINUE:
            return true;
        default:
            return false;
    }
}

/* Check whether the fall-through point after `e` is unreachable -- i.e. every
 * path through `e` ends in a return/panic.  Unlike expr_is_divergent (which
 * only inspects the last item of a `do`), this treats a `do` as divergent when
 * ANY item diverges, since statements after an unconditional return/panic are
 * unreachable.  Used to decide whether a function body still needs a trailing
 * `return <body-value>`. */
bool expr_tail_diverges(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_DISCONTINUE:
            return true;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_tail_diverges(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_LET:
            return expr_tail_diverges(e->as.let_.body);
        case EX_IF:
            /* Diverges only if both branches diverge; a missing else falls
             * through. */
            if (!e->as.if_.else_or_null) return false;
            return expr_tail_diverges(e->as.if_.then_) &&
                   expr_tail_diverges(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* MS1: Check if a program contains any handle expression with a ^multishot case.
 * Used to decide whether to emit the cloneable cont preamble. */
bool expr_has_multishot_handler(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_HANDLE: {
            const HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (h->cases[i].cont_kind == CK_MULTISHOT) return true;
            }
            if (expr_has_multishot_handler(h->body)) return true;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (expr_has_multishot_handler(h->cases[i].body)) return true;
            }
            return false;
        }
        case EX_HANDLER_LIT: {
            const HandleExpr *h = e->as.handler_lit_.handle;
            if (!h) return false;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (h->cases[i].cont_kind == CK_MULTISHOT) return true;
                if (expr_has_multishot_handler(h->cases[i].body)) return true;
            }
            return false;
        }
        case EX_WITH_HANDLER:
            if (expr_has_multishot_handler(e->as.with_handler_.handler)) return true;
            return expr_has_multishot_handler(e->as.with_handler_.body);
        case EX_COMPOSE_HANDLERS:
            if (expr_has_multishot_handler(e->as.compose_handlers_.h1)) return true;
            return expr_has_multishot_handler(e->as.compose_handlers_.h2);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (expr_has_multishot_handler(e->as.let_.bindings[i].init)) return true;
            }
            return expr_has_multishot_handler(e->as.let_.body);
        case EX_IF:
            if (expr_has_multishot_handler(e->as.if_.cond)) return true;
            if (expr_has_multishot_handler(e->as.if_.then_)) return true;
            return e->as.if_.else_or_null && expr_has_multishot_handler(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_has_multishot_handler(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && expr_has_multishot_handler(e->as.fn_def_.fn->body);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (expr_has_multishot_handler(e->as.program.items[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

char *fresh_tmp(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__t%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh frame variable name */
char *fresh_frame(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__frame_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer thunk function name */
char *fresh_defer_thunk(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer env struct name */
char *fresh_defer_env(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_env_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Register a defer thunk to be emitted at file scope */
void register_defer_thunk(EmitCtx *ctx, const char *name, const Expr *body, 
                                  Binding **captures, uint8_t n_captures, 
                                  const char *env_name) {
    DeferThunk *thunk = (DeferThunk *)malloc(sizeof(DeferThunk));
    if (!thunk) { fprintf(stderr, "tur: oom\n"); abort(); }
    thunk->name = (char *)name;
    thunk->body = (Expr *)body;  /* Cast away const for storage */
    thunk->captures = captures;
    thunk->n_captures = n_captures;
    thunk->env_name = env_name ? strdup(env_name) : NULL;
    thunk->next = ctx->pending_defer_thunks;
    ctx->pending_defer_thunks = thunk;
}

/* Phase 4 v1: Emit all registered defer thunks to the output buffer */
void emit_pending_defer_thunks(EmitCtx *ctx, Buf *out) {
    /* First pass: emit env struct definitions for thunks with captures */
    DeferThunk *thunk = ctx->pending_defer_thunks;
    while (thunk) {
        if (thunk->env_name) {
            /* Emit env struct type definition */
            buf_printf(out, "struct %s {", thunk->env_name);
            for (uint8_t i = 0; i < thunk->n_captures; i++) {
                if (i > 0) buf_puts(out, "; ");
                Binding *captured = thunk->captures[i];
                char *field = raw_name_for_binding(captured);
                buf_printf(out, "%s %s",
                           type_c_name(captured->type), field);
                free(field);
            }
            buf_puts(out, "; };\n\n");
        }
        thunk = thunk->next;
    }
    
    /* Second pass: emit thunk functions */
    thunk = ctx->pending_defer_thunks;
    while (thunk) {
        if (thunk->env_name) {
            /* Thunk with captures - cast void* to env struct type */
            buf_printf(out, "static void %s(void *__env) {\n", thunk->name);
            buf_printf(out, "    struct %s *__e = (struct %s *)__env;\n",
                       thunk->env_name, thunk->env_name);
            
            /* Set up env access context for name_for_binding */
            const char *saved_env_var_name = ctx->env_var_name;
            Binding **saved_defer_captures = ctx->defer_captures;
            uint8_t saved_n_defer_captures = ctx->n_defer_captures;
            
            ctx->env_var_name = "__e";
            ctx->defer_captures = thunk->captures;
            ctx->n_defer_captures = thunk->n_captures;
            
            /* Emit the body with env access */
            int saved_indent = ctx->indent;
            ctx->indent = 4;
            emit_stmt(ctx, out, thunk->body);
            ctx->indent = saved_indent;
            
            ctx->env_var_name = saved_env_var_name;
            ctx->defer_captures = saved_defer_captures;
            ctx->n_defer_captures = saved_n_defer_captures;
        } else {
            /* Thunk without captures */
            buf_printf(out, "static void %s(void *__env) {\n", thunk->name);
            int saved_indent = ctx->indent;
            ctx->indent = 4;
            emit_stmt(ctx, out, thunk->body);
            ctx->indent = saved_indent;
        }
        buf_puts(out, "}\n\n");
        thunk = thunk->next;
    }
    
    /* Free the list */
    while (ctx->pending_defer_thunks) {
        DeferThunk *tmp = ctx->pending_defer_thunks;
        ctx->pending_defer_thunks = tmp->next;
        free(tmp->name);
        free(tmp->env_name);  /* May be NULL */
        free(tmp);
    }
}








/* DV2: Return a malloc'd C identifier for a dynamic var name.
 * Strips leading/trailing '*' (earmuffs), then converts non-alphanumeric
 * characters to '_'. E.g. "*log-level*" -> "log_level". Caller frees. */
char *mangle_dynvar_name(const char *name) {
    const char *start = name;
    size_t mlen = strlen(name);
    if (mlen > 0 && start[0] == '*') { start++; mlen--; }
    if (mlen > 0 && start[mlen - 1] == '*') { mlen--; }
    char *p = (char *)malloc(mlen + 1);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (size_t i = 0; i < mlen; i++) {
        char c = start[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[i] = c;
        } else {
            p[i] = '_';
        }
    }
    p[mlen] = '\0';
    return p;
}

/* Mangle a Turmeric struct field name to a valid C identifier.
 * Replaces hyphens and other non-id-safe chars with underscores. Caller frees. */
char *mangle_field_name(const char *name) {
    size_t len = strlen(name);
    char *p = (char *)malloc(len + 1);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[i] = c;
        } else {
            p[i] = '_';
        }
    }
    p[len] = '\0';
    return p;
}

/* Return a sanitized C identifier for a Binding, without the ID suffix.
 * Used for function names and parameters. Caller frees.
 *
 * Phase M3: For module-level bindings (defining_module_name != NULL), the
 * C name is prefixed with the mangled module name: geom/vector → geom__vector__,
 * so binding `add2` in module `geom/vector` → `geom__vector__add2`.
 * Phase M6: If b->c_export_name is set, use it directly (bypasses mangling). */
char *raw_name_for_binding(const Binding *b) {
    if (b->c_export_name) {
        return strdup(b->c_export_name);
    }
    /* Build module prefix if this binding belongs to a named module.
     * Exception: `main` is always the C entry point, never prefixed. */
    char mod_prefix[512];
    size_t mod_prefix_len = 0;
    bool is_main_binding = (b->name->len == 4 &&
                             memcmp(b->name->name, "main", 4) == 0);
    /* Phase M7: Only globals get module-prefixed C names. Function parameters
     * and locals are scoped to their function, so they don't collide across
     * modules — and inline-C bodies reference them by their source names. */
    if (b->defining_module_name != NULL && !is_main_binding && b->is_global) {
        const char *mn = b->defining_module_name->name;
        size_t mn_len = b->defining_module_name->len;
        size_t j = 0;
        for (size_t i = 0; i < mn_len && j < sizeof(mod_prefix) - 3; i++) {
            char c = mn[i];
            if (c == '/') {
                mod_prefix[j++] = '_';
                mod_prefix[j++] = '_';
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_') {
                mod_prefix[j++] = c;
            } else {
                mod_prefix[j++] = '_';
            }
        }
        mod_prefix[j++] = '_';
        mod_prefix[j++] = '_';
        mod_prefix[j]   = '\0';
        mod_prefix_len  = j;
    }

    size_t total = mod_prefix_len + b->name->len + 1;
    char *p = (char *)malloc(total);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    if (mod_prefix_len > 0) {
        memcpy(p, mod_prefix, mod_prefix_len);
        k = mod_prefix_len;
    }
    for (uint32_t i = 0; i < b->name->len; i++) {
        char c = b->name->name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[k++] = c;
        } else {
            p[k++] = '_';
        }
    }
    p[k] = '\0';
    return p;
}

/* GHE (constrained-generic-instance-dispatch): the single-component type suffix
 * used in instance-method mangled names (__inst_<Class>_<method>_<component>).
 * Mirrors the type_suffix switch in emit_stmt.c's EX_INSTANCE_DEF.  Returns NULL
 * for types we do not re-resolve (the caller then keeps the baked representative
 * callee, which is the carrier-correct TY_INT instance). */
static const char *emit_inst_suffix_component(TypeKind k) {
    switch (k) {
        case TY_INT:     return "int";
        case TY_BOOL:    return "bool";
        case TY_CSTR:    return "cstr";
        case TY_NIL:     return "nil";
        case TY_INT8:    return "int8";
        case TY_INT16:   return "int16";
        case TY_INT32:   return "int32";
        case TY_UINT8:   return "uint8";
        case TY_UINT16:  return "uint16";
        case TY_UINT32:  return "uint32";
        case TY_UINT64:  return "uint64";
        case TY_FLOAT:   return "float";
        case TY_FLOAT32: return "float32";
        case TY_FLOAT64: return "float64";
        case TY_SYM:     return "Sym";
        default:         return NULL;
    }
}

/* GHE: re-resolve a typeclass-method call inside a monomorphized constrained
 * generic.  When the call carries a dict_arg annotation (so it is a typeclass
 * method call) and its receiver argument's type is a type variable that the
 * current ABI specialization binds to a concrete scalar, return the correct
 * __inst_<Class>_<sanitized-method>_<component> name.  Returns NULL when no
 * re-resolution applies (the caller keeps the baked representative callee, which
 * is the TY_INT carrier instance -- correct for the int / base-clone case). */
static char *emit_reresolve_method_call(EmitCtx *ctx, const Expr *call) {
    if (!ctx || !ctx->current_abi_specialization || !call ||
        call->kind != EX_CALL) {
        return NULL;
    }
    const Expr *dict = call->as.call_.dict_arg;
    if (!dict || dict->kind != EX_DICT || !dict->as.dict_.instance) return NULL;
    if (dict->as.dict_.method_name[0] == '\0') return NULL;
    if (call->as.call_.n_args < 1 || !call->as.call_.args) return NULL;

    /* Receiver is arg 0.  Strip ascriptions, then require a type variable so we
     * only act on genuinely-polymorphic constrained-generic dispatch (a concrete
     * receiver already baked the correct instance at elaboration). */
    const Expr *recv = call->as.call_.args[0];
    while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
    if (!recv || recv->type.kind != TY_TYVAR) return NULL;

    Type resolved = emit_resolve_type(ctx, recv->type);
    if (resolved.kind == TY_TYVAR) return NULL; /* still unbound: keep base/repr */
    const char *component = emit_inst_suffix_component(resolved.kind);
    /* WKC3: a named struct/ADT key resolves to TY_STRUCT; its instance is
     * mangled by the (sanitized) type name, mirroring the EX_INSTANCE_DEF and
     * dict-name switches.  Without this, an aggregate-keyed constrained-generic
     * call falls back to the int carrier representative (the same failure float
     * keys hit before TY_FLOAT was added to the suffix manglers). */
    char struct_comp[64];
    if (!component && resolved.kind == TY_STRUCT &&
        resolved.as.struct_.def && resolved.as.struct_.def->name) {
        const char *nm = resolved.as.struct_.def->name;
        size_t i = 0;
        for (; nm[i] && i < sizeof(struct_comp) - 1; i++) {
            unsigned char c = (unsigned char)nm[i];
            struct_comp[i] = (char)(((c >= '0' && c <= '9') ||
                                     (c >= 'A' && c <= 'Z') ||
                                     (c >= 'a' && c <= 'z')) ? (char)c : '_');
        }
        struct_comp[i] = '\0';
        component = struct_comp;
    }
    /* GDE2: a TY_APP receiver (e.g. Map[cstr int]) needs the head constructor
     * name as the instance suffix component, mirroring the HKT-instance naming
     * on the definition side (__inst_Eq_eq__Map for definstance Eq [Map]).
     * Walk the app-chain to the constructor, which must be a TY_STRUCT. */
    char app_comp[64];
    if (!component && resolved.kind == TY_APP) {
        Type head = resolved;
        while (head.kind == TY_APP && head.as.app.fn)
            head = *head.as.app.fn;
        if (head.kind == TY_STRUCT && head.as.struct_.def && head.as.struct_.def->name) {
            const char *nm = head.as.struct_.def->name;
            size_t i = 0;
            for (; nm[i] && i < sizeof(app_comp) - 1; i++) {
                unsigned char c = (unsigned char)nm[i];
                app_comp[i] = (char)(((c >= '0' && c <= '9') ||
                                      (c >= 'A' && c <= 'Z') ||
                                      (c >= 'a' && c <= 'z')) ? (char)c : '_');
            }
            app_comp[i] = '\0';
            component = app_comp;
        }
    }
    if (!component) return NULL;

    const TypeClass *tc = dict->as.dict_.instance->typeclass;
    if (!tc || !tc->name) return NULL;

    Buf nm; buf_init(&nm);
    buf_printf(&nm, "__inst_%s_%s_%s", tc->name->name,
               dict->as.dict_.method_name, component);
    buf_putc(&nm, '\0');
    char *result = strdup(nm.data);
    buf_free(&nm);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    return result;
}

char *emit_call_name(EmitCtx *ctx, const Expr *call, const Binding *b) {
    const Expr *cur = NULL;
    /* GHE: typeclass-method dispatch inside a monomorphized constrained generic
     * takes precedence over the generic-function specialization lookup below. */
    {
        char *reresolved = emit_reresolve_method_call(ctx, call);
        if (reresolved) return reresolved;
    }
    if (ctx && call) {
        for (uint32_t i = 0; i < ctx->n_specialized_calls; i++) {
            if (ctx->specialized_call_exprs[i] == call) {
                char *name = strdup(ctx->specialized_call_names[i]);
                if (!name) { fprintf(stderr, "tur: oom\n"); abort(); }
                return name;
            }
        }
        if (call->kind == EX_CALL && b) {
            for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
                const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
                if (spec->binding != b || spec->n_args != call->as.call_.n_args) continue;
                bool args_match = true;
                for (uint32_t ai = 0; ai < call->as.call_.n_args; ai++) {
                    cur = call->as.call_.args[ai];
                    while (cur && cur->kind == EX_ASCRIBE) cur = cur->as.ascribe_.inner;
                    Type actual = (cur && cur->kind == EX_REINTERPRET && cur->as.reinterpret_.expr)
                        ? cur->as.reinterpret_.expr->type
                        : (cur ? cur->type : emit_type_from_kind(TY_UNKNOWN));
                    /* CS3: When emitting a specialized body (current_abi_specialization is
                     * set), resolve any type variables in the arg type against the outer
                     * specialization's bindings.  This lets inner calls like pair_second(p)
                     * match the correct specialization even though p still has a generic
                     * TY_TYVAR type in the original AST. */
                    if (ctx->current_abi_specialization) {
                        actual = emit_resolve_type(ctx, actual);
                    }
                    if (!type_eq(spec->arg_types[ai], actual)) {
                        args_match = false;
                        break;
                    }
                }
                if (args_match) {
                    char *name = strdup(spec->clone_name);
                    if (!name) { fprintf(stderr, "tur: oom\n"); abort(); }
                    return name;
                }
            }
        }
    }
    return raw_name_for_binding(b);
}

/* Return a sanitized C identifier for a Binding. If the binding is a
 * function parameter in the current context, use the raw name (without ID).
 * Otherwise, append the ID suffix. Caller frees. */
char *name_for_binding(EmitCtx *ctx, const Binding *b) {
    /* GF1: If inside a generator _next function, redirect struct fields to __g->field */
    if (ctx->gen_var_name && ctx->gen_struct_bindings) {
        for (uint32_t i = 0; i < ctx->n_gen_struct_bindings; i++) {
            if (ctx->gen_struct_bindings[i] == b) {
                char *field_name = raw_name_for_binding(b);
                size_t sz = strlen(ctx->gen_var_name) + strlen(field_name) + 4;
                char *result = (char *)malloc(sz);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, sz, "%s->%s", ctx->gen_var_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Phase 3: If this is a captured binding in a closure thunk, emit as env->field */
    if (ctx->closure && ctx->env_var_name) {
        for (uint8_t i = 0; i < ctx->closure->n_captures; i++) {
            if (ctx->closure->captures[i] == b) {
                /* This is a captured binding - emit as env_var_name->field_name */
                char *field_name = raw_name_for_binding(b);
                char *result = (char *)malloc(strlen(ctx->env_var_name) + strlen(field_name) + 4);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(ctx->env_var_name) + strlen(field_name) + 4, "%s->%s", ctx->env_var_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Phase 4 v1: If this is a captured binding in a defer thunk, emit as env->field */
    if (ctx->env_var_name && ctx->defer_captures) {
        for (uint8_t i = 0; i < ctx->n_defer_captures; i++) {
            if (ctx->defer_captures[i] == b) {
                /* This is a captured binding - emit as env_var_name->field_name */
                char *field_name = raw_name_for_binding(b);
                char *result = (char *)malloc(strlen(ctx->env_var_name) + strlen(field_name) + 4);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(ctx->env_var_name) + strlen(field_name) + 4, "%s->%s", ctx->env_var_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Phase 19D: If this is a captured binding in a handle body fiber function, emit as __env->field */
    if (ctx->handle_captures && ctx->handle_env_name) {
        for (uint32_t i = 0; i < ctx->n_handle_captures; i++) {
            if (ctx->handle_captures[i] == b) {
                char *field_name = raw_name_for_binding(b);
                char *result = (char *)malloc(strlen(ctx->handle_env_name) + strlen(field_name) + 4);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(ctx->handle_env_name) + strlen(field_name) + 4,
                         "%s->%s", ctx->handle_env_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Check if this binding is a function parameter in the current context */
    if (ctx->fn_params) {
        for (uint8_t i = 0; i < ctx->n_fn_params; i++) {
            if (ctx->fn_params[i] == b) {
                return raw_name_for_binding(b);
            }
        }
    }
    /* If this is a bare function reference (captureless fn / top-level defn),
     * use the raw name without ID -- its C name *is* the function symbol.
     * CRU B-1: a *boxed* TY_FN is a first-class closure *value* (a local box),
     * not a function symbol; fall through to the id-suffixed mangling path so
     * distinct closures don't collide and a source name that is a C keyword
     * (e.g. `double`) is disambiguated to `double_<id>`. */
    if (b->type.kind == TY_FN && !b->type.as.fn.boxed) {
        return raw_name_for_binding(b);
    }
    /* Phase M6: If c_export_name is set, use it directly (bypasses mangling and id suffix). */
    if (b->c_export_name) {
        return strdup(b->c_export_name);
    }
    /* `<name>_<id>` with non-id-safe chars mangled to underscores. We append
     * the unique id so different bindings with the same source name don't
     * collide in C. */
    size_t cap = b->name->len + 16;
    char *p = (char *)malloc(cap);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    for (uint32_t i = 0; i < b->name->len; i++) {
        char c = b->name->name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[k++] = c;
        } else {
            p[k++] = '_';
        }
    }
    snprintf(p + k, cap - k, "_%u", b->id);
    return p;
}

/* Emit a C string literal from a StrSlice. */
void emit_c_string(Buf *out, StrSlice s) {
    buf_putc(out, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.p[i];
        switch (c) {
            case '"':  buf_puts(out, "\\\""); break;
            case '\\': buf_puts(out, "\\\\"); break;
            case '\n': buf_puts(out, "\\n");  break;
            case '\t': buf_puts(out, "\\t");  break;
            case '\r': buf_puts(out, "\\r");  break;
            default:
                if (c < 0x20 || c == 0x7f) buf_printf(out, "\\x%02x", c);
                else                       buf_putc(out, (char)c);
        }
    }
    buf_putc(out, '"');
}

/* ------------ atomic emitters (no statements emitted) ------------ */

char *atom_nil(void)         { return strdup("((void)0)"); }
char *atom_bool(bool b)      { return strdup(b ? "true" : "false"); }
/* Phase N: emit integer literal with correct C macro for fixed-width type */
char *atom_int_typed(int64_t i, TypeKind k) {
    char buf[64];
    switch (k) {
        case TY_INT8:   snprintf(buf, sizeof buf, "INT8_C(%lld)",   (long long)i); break;
        case TY_INT16:  snprintf(buf, sizeof buf, "INT16_C(%lld)",  (long long)i); break;
        case TY_INT32:  snprintf(buf, sizeof buf, "INT32_C(%lld)",  (long long)i); break;
        case TY_INT64:  snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
        case TY_UINT8:  snprintf(buf, sizeof buf, "UINT8_C(%llu)",  (unsigned long long)(uint64_t)i); break;
        case TY_UINT16: snprintf(buf, sizeof buf, "UINT16_C(%llu)", (unsigned long long)(uint64_t)i); break;
        case TY_UINT32: snprintf(buf, sizeof buf, "UINT32_C(%llu)", (unsigned long long)(uint64_t)i); break;
        case TY_UINT64: snprintf(buf, sizeof buf, "UINT64_C(%llu)", (unsigned long long)(uint64_t)i); break;
        default:        snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
    }
    return strdup(buf);
}
/* Phase N: emit float32 literal as a (float) cast */
char *atom_float32(double f) {
    char buf[80];
    snprintf(buf, sizeof buf, "((float)%.9g)", f);
    return strdup(buf);
}

char *atom_float(double f) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.15g", f);
    /* Ensure it's a double literal by appending .0 if needed */
    char *p = strchr(buf, '.');
    char *e = strchr(buf, 'e');
    if (!p && !e) {
        /* No decimal point or exponent - append .0 */
        strcat(buf, ".0");
    }
    return strdup(buf);
}

/* Phase N: type-dispatched float literal emitter */
static char *atom_float_typed(TypeKind k, double f) __attribute__((unused));
static char *atom_float_typed(TypeKind k, double f) {
    if (k == TY_FLOAT32) {
        char buf[64];
        snprintf(buf, sizeof buf, "(float)(%.7g)", f);
        return strdup(buf);
    }
    return atom_float(f);
}
/* GHE2: when emitting inside a monomorphized constrained generic, a reference to
 * a *generic function as a value* (e.g. a comparator fn passed to map-assoc-eq)
 * must name the per-K child clone that the abi scan created under the active
 * specialization's bindings -- not the carrier base clone whose body bakes the
 * Eq[int] instance.  Mirrors emit_reresolve_method_call (which handles method
 * *calls*) but for fn *values*.  Returns the clone name, or NULL when no child
 * spec applies (caller falls back to name_for_binding). */
static char *emit_reresolve_fn_value(EmitCtx *ctx, const Binding *b) {
    const EmitAbiSpecialization *outer = ctx ? ctx->current_abi_specialization : NULL;
    if (!outer || !b || b->type.kind != TY_FN) return NULL;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->binding != b || !spec->clone_name) continue;
        /* The child spec was interned under the same tyvar->type bindings the
         * outer spec carries, so it matches iff those bindings agree. */
        if (spec->n_bindings != outer->n_bindings) continue;
        bool ok = true;
        for (uint8_t bi = 0; bi < spec->n_bindings; bi++) {
            const AbiTypeBinding *sb = &spec->bindings[bi];
            const AbiTypeBinding *ob = &outer->bindings[bi];
            if (!sb->name || !ob->name || strcmp(sb->name, ob->name) != 0 ||
                !type_eq(sb->type, ob->type)) { ok = false; break; }
        }
        if (!ok) continue;
        char *name = strdup(spec->clone_name);
        if (!name) { fprintf(stderr, "tur: oom\n"); abort(); }
        return name;
    }
    return NULL;
}

char *atom_var(EmitCtx *ctx, const Binding *b) {
    char *reresolved = emit_reresolve_fn_value(ctx, b);
    if (reresolved) return reresolved;
    return name_for_binding(ctx, b);
}
char *atom_cstr(StrSlice s) {
    /* Build into a Buf, then strdup out. */
    Buf b; buf_init(&b);
    emit_c_string(&b, s);
    buf_putc(&b, '\0');
    char *p = strdup(b.data);
    buf_free(&b);
    return p;
}

/* Phase G: scan an inline-C body for __TUR_TY_<NAME>__ template markers.
 * Used by emit_abi_register_call to decide whether an inline-C function
 * opts in to ABI specialization. */
bool inline_c_has_ty_template(const InlineC *ic) {
    if (!ic || !ic->code.p) return false;
    const char *code = ic->code.p;
    uint32_t len = ic->code.len;
    if (len < 10) return false;
    for (uint32_t i = 0; i + 9 < len; i++) {
        if (memcmp(code + i, "__TUR_TY_", 9) == 0) return true;
    }
    return false;
}

/* SS2: Perform __TUR_CAP_N__ / __TUR_VAL_N__ substitution on an InlineC node.
 * Emits val_exprs[N] into temp vars in body, then returns a malloc'd C string
 * with all __TUR_CAP_N__ and __TUR_VAL_N__ placeholders substituted.
 *
 * Phase G: also substitutes __TUR_TY_<NAME>__ placeholders. Under an active
 * ABI specialization, <NAME> resolves to the concrete C type bound to that
 * type variable; otherwise (generic / carrier emission) it resolves to
 * int64_t (the carrier). val_expr temporary types are also resolved through
 * the active specialization, so __TUR_VAL_N__ temps land at the concrete
 * C width rather than the generic carrier width. */
char *inline_c_substitute(EmitCtx *ctx, Buf *body, InlineC *ic) {
    bool has_ty_template = inline_c_has_ty_template(ic);

    /* Fast path: no substitution needed. */
    if (ic->n_captures == 0 && ic->n_val_exprs == 0 && !has_ty_template) {
        return strndup(ic->code.p, ic->code.len);
    }

    /* Build capture name array. */
    char **cap_names = NULL;
    if (ic->n_captures > 0) {
        cap_names = (char **)calloc(ic->n_captures, sizeof(char *));
        for (int ci = 0; ci < ic->n_captures; ci++) {
            cap_names[ci] = name_for_binding(ctx, ic->captures[ci]);
        }
    }

    /* Evaluate val_exprs into temp variables. Phase G: resolve the temp
     * declared type through the active specialization so a TY_TYVAR-typed
     * subexpression ends up at the concrete C width. */
    char **val_temps = NULL;
    if (ic->n_val_exprs > 0) {
        val_temps = (char **)calloc(ic->n_val_exprs, sizeof(char *));
        for (int vi = 0; vi < ic->n_val_exprs; vi++) {
            char *tmp = fresh_tmp(ctx);
            char *vv = emit_value(ctx, body, ic->val_exprs[vi]);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = %s;\n",
                       emit_type_c_name(ctx, ic->val_exprs[vi]->type), tmp, vv);
            free(vv);
            val_temps[vi] = tmp;
        }
    }

    /* Scan the code string and perform substitution. */
    const char *code = ic->code.p;
    uint32_t len = ic->code.len;
    Buf result; buf_init(&result);
    for (uint32_t i = 0; i < len; ) {
        bool matched = false;
        /* Check for __TUR_CAP_N__ */
        if (i + 12 <= len && memcmp(code + i, "__TUR_CAP_", 10) == 0) {
            uint32_t j = i + 10;
            int n = 0; bool have_digit = false;
            while (j < len && code[j] >= '0' && code[j] <= '9') {
                n = n * 10 + (code[j] - '0'); j++; have_digit = true;
            }
            if (have_digit && j + 1 < len && code[j] == '_' && code[j+1] == '_') {
                j += 2;
                buf_puts(&result, (cap_names && n < ic->n_captures) ? cap_names[n] : "NULL");
                i = j; matched = true;
            }
        }
        /* Check for __TUR_VAL_N__ */
        if (!matched && i + 12 <= len && memcmp(code + i, "__TUR_VAL_", 10) == 0) {
            uint32_t j = i + 10;
            int n = 0; bool have_digit = false;
            while (j < len && code[j] >= '0' && code[j] <= '9') {
                n = n * 10 + (code[j] - '0'); j++; have_digit = true;
            }
            if (have_digit && j + 1 < len && code[j] == '_' && code[j+1] == '_') {
                j += 2;
                buf_puts(&result, (val_temps && n < ic->n_val_exprs) ? val_temps[n] : "0");
                i = j; matched = true;
            }
        }
        /* Phase G: Check for __TUR_TY_<NAME>__ — substitutes to the concrete
         * C type bound to <NAME> in the active ABI specialization, or to
         * int64_t (the carrier) when no specialization is active. */
        if (!matched && i + 11 <= len && memcmp(code + i, "__TUR_TY_", 9) == 0) {
            uint32_t j = i + 9;
            uint32_t name_start = j;
            while (j < len && (
                (code[j] >= 'A' && code[j] <= 'Z') ||
                (code[j] >= 'a' && code[j] <= 'z') ||
                (code[j] >= '0' && code[j] <= '9') ||
                code[j] == '_')) {
                /* a single trailing "__" closes the placeholder; check that
                 * we have not stepped into the closing pair before consuming */
                if (code[j] == '_' && j + 1 < len && code[j+1] == '_') {
                    /* peek: only treat as terminator if this leaves a real
                     * name (j > name_start) and the chars after are not
                     * additional underscores (a name like FOO__BAR is allowed
                     * by greedy match below) */
                    break;
                }
                j++;
            }
            if (j > name_start && j + 1 < len && code[j] == '_' && code[j+1] == '_') {
                uint32_t name_len = j - name_start;
                char name_buf[64];
                if (name_len < sizeof(name_buf)) {
                    memcpy(name_buf, code + name_start, name_len);
                    name_buf[name_len] = '\0';
                    const char *resolved = "int64_t";
                    const EmitAbiSpecialization *spec =
                        ctx ? ctx->current_abi_specialization : NULL;
                    if (spec) {
                        for (uint8_t bi = 0; bi < spec->n_bindings; bi++) {
                            if (spec->bindings[bi].name &&
                                strcmp(spec->bindings[bi].name, name_buf) == 0) {
                                resolved = type_c_name(spec->bindings[bi].type);
                                break;
                            }
                        }
                    }
                    buf_puts(&result, resolved);
                    i = j + 2; matched = true;
                }
            }
        }
        if (!matched) { buf_putc(&result, code[i++]); }
    }

    /* Free temporaries. */
    for (int ci = 0; ci < ic->n_captures; ci++) free(cap_names[ci]);
    free(cap_names);
    for (int vi = 0; vi < ic->n_val_exprs; vi++) free(val_temps[vi]);
    free(val_temps);

    buf_putc(&result, '\0');
    char *out = strdup(result.data);
    buf_free(&result);
    return out;
}

/* ------------ builtin emitters ------------ */

char *emit_builtin(EmitCtx *ctx, Buf *body, const Expr *e) {
    const BuiltinSpec *spec = e->as.builtin.spec;
    uint32_t n = e->as.builtin.n;
    Expr **args = e->as.builtin.args;

    /* Eagerly emit all args (left-to-right) for non-short-circuit ops. */
    if (spec->shape == BS_AND_SC || spec->shape == BS_OR_SC) {
        /* Short-circuit: evaluate left, then conditionally evaluate the rest.
         * Lower (and a b c) to: bool t = a; if (t) t = b; if (t) t = c;
         * Lower (or  a b c) to: bool t = a; if (!t) t = b; if (!t) t = c; */
        char *tmp = fresh_tmp(ctx);
        char *first = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        buf_printf(body, "bool %s = %s;\n", tmp, first);
        free(first);
        for (uint32_t i = 1; i < n; i++) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s%s) {\n",
                       spec->shape == BS_OR_SC ? "!" : "", tmp);
            ctx->indent += 4;
            char *next = emit_value(ctx, body, args[i]);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, next);
            free(next);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
        }
        return tmp;
    }

    /* println — emits a stmt and returns a nil placeholder. */
    if (spec->shape == BS_PRINTLN_INT ||
        spec->shape == BS_PRINTLN_FLOAT ||
        spec->shape == BS_PRINTLN_BOOL ||
        spec->shape == BS_PRINTLN_CSTR ||
        spec->shape == BS_PRINTLN_UINT ||
        spec->shape == BS_PRINTLN_FLOAT32) {
        char *arg = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        switch (spec->shape) {
            case BS_PRINTLN_INT:
                buf_printf(body, "printf(\"%%lld\\n\", (long long)(%s));\n", arg);
                break;
            case BS_PRINTLN_FLOAT:
                buf_printf(body, "printf(\"%%g\\n\", (double)(%s));\n", arg);
                break;
            case BS_PRINTLN_BOOL:
                buf_printf(body, "puts((%s) ? \"true\" : \"false\");\n", arg);
                break;
            case BS_PRINTLN_CSTR:
                buf_printf(body, "puts(%s);\n", arg);
                break;
            case BS_PRINTLN_UINT:
                buf_printf(body, "printf(\"%%llu\\n\", (unsigned long long)(%s));\n", arg);
                break;
            case BS_PRINTLN_FLOAT32:
                buf_printf(body, "printf(\"%%.7g\\n\", (double)(%s));\n", arg);
                break;
            default: break;
        }
        free(arg);
        return atom_nil();
    }

    /* For everything else, evaluate args and build a C expression. */
    char **arg_strs = (char **)malloc(n * sizeof(char *));
    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (uint32_t i = 0; i < n; i++) arg_strs[i] = emit_value(ctx, body, args[i]);

    Buf out; buf_init(&out);
    switch (spec->shape) {
        case BS_BIN_INFIX:
            buf_printf(&out, "(%s) %s (%s)",
                       arg_strs[0], spec->c_op, arg_strs[1]);
            break;
        case BS_VARIADIC_FOLD: {
            /* ((a OP b) OP c) OP d ... -- no redundant outermost wrap */
            uint32_t group_opens = (n >= 2) ? n - 2 : 0;
            for (uint32_t i = 0; i < group_opens; i++) buf_putc(&out, '(');
            buf_printf(&out, "(%s)", arg_strs[0]);
            for (uint32_t i = 1; i < n; i++) {
                buf_printf(&out, " %s (%s)", spec->c_op, arg_strs[i]);
                if (i < n - 1) buf_putc(&out, ')');
            }
            break;
        }
        case BS_DIV_CHECK:
            /* Division with zero check: (a / b) checks b != 0 */
            buf_printf(&out, "((%s) ? ((%s) / (%s)) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                       arg_strs[1], arg_strs[0], arg_strs[1]);
            break;
        case BS_PREFIX_UNARY:
            buf_printf(&out, "%s(%s)", spec->c_op, arg_strs[0]);
            break;
        case BS_PREFIX_UNARY_FREE:
            /* Special case for drop!: free the ref pointer directly
             * arg is a ref<T> which is stored as void* in C for v1
             * Emit: free(arg) as a statement
             * Unlike other builtins, this has side effects and is always emitted
             * as a statement, so we emit it directly here and return nil */
            indent_buf(body, ctx->indent);
            buf_printf(body, "free(%s);\n", arg_strs[0]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_PTR_WRITE:
            /* Special case for ptr-write: *ptr = value as a statement
             * Emit: *((int64_t *)arg0) = arg1; as a statement, return nil
             * We cast to int64_t * because all Turmeric values are int64_t in v1 */
            indent_buf(body, ctx->indent);
            buf_printf(body, "*((int64_t *)%s) = %s;\n", arg_strs[0], arg_strs[1]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_PTR_ARITH:
            /* Pointer arithmetic: (char *)ptr op offset
             * In C, pointer arithmetic requires a typed pointer, so we cast to char *
             * Emit: (char *)arg0 op arg1 */
            buf_printf(&out, "((char *)%s %s %s)", arg_strs[0], spec->c_op, arg_strs[1]);
            break;
        case BS_PTR_DEREF:
            /* Pointer dereference: *((T *)ptr)
             * c_op is the prefix like "*((int64_t *)" and we append arg + ")"
             * Emit: c_op + arg + ")" */
            buf_printf(&out, "%s%s)", spec->c_op, arg_strs[0]);
            break;
        /* Phase U3: Unsafe primitives - type casting */
        case BS_UNSAFE_CAST:
            /* unsafe-cast: C-style cast from one type to another
             * arg0 = value to cast, arg1 = type keyword (we ignore it and cast to int64_t for v1)
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        case BS_REINTERPRET:
            /* reinterpret: bitwise reinterpretation - same as unsafe-cast for v1
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        case BS_TRANSMUTE:
            /* transmute: bitwise reinterpretation; size equality verified at
             * elaboration time. Cast through the result type. */
            buf_printf(&out, "((%s)%s)", type_c_name(e->type), arg_strs[0]);
            break;
        /* Phase U3: Unsafe primitives - unchecked array ops */
        case BS_ARRAY_GET_UNCHECKED:
            /* array-get-unchecked: *(ptr + index)
             * arg0 = pointer, arg1 = index
             * Emit: *((int64_t *)arg0 + arg1) */
            buf_printf(&out, "(*((int64_t *)%s + %s))", arg_strs[0], arg_strs[1]);
            break;
        case BS_ARRAY_SET_UNCHECKED:
            /* array-set-unchecked: *(ptr + index) = value
             * arg0 = pointer, arg1 = index, arg2 = value
             * Emit as statement: *((int64_t *)arg0 + arg1) = arg2; */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(*((int64_t *)%s + %s) = %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        /* Phase U3: Unsafe primitives - raw memory */
        case BS_RAW_MALLOC:
            /* raw-malloc: malloc(size)
             * arg0 = size
             * Emit: malloc(arg0) */
            buf_printf(&out, "malloc(%s)", arg_strs[0]);
            break;
        case BS_RAW_FREE:
            /* raw-free: free(ptr)
             * arg0 = pointer
             * Emit as statement: free(arg0); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "free(%s);\n", arg_strs[0]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_RAW_REALLOC:
            /* raw-realloc: realloc(ptr, new_size)
             * arg0 = pointer, arg1 = new size
             * Emit: realloc(arg0, arg1) */
            buf_printf(&out, "realloc(%s, %s)", arg_strs[0], arg_strs[1]);
            break;
        case BS_RAW_MEMCPY:
            /* raw-memcpy: memcpy(dest, src, n)
             * arg0 = dest, arg1 = src, arg2 = n
             * Emit as statement: memcpy(arg0, arg1, arg2); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "memcpy(%s, %s, %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_RAW_MEMSET:
            /* raw-memset: memset(dest, byte, n)
             * arg0 = dest, arg1 = byte, arg2 = n
             * Emit as statement: memset(arg0, arg1, arg2); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "memset(%s, %s, %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        /* Phase U3: FFI */
        case BS_DLOPEN:
            /* dlopen: dlopen(path, RTLD_LAZY)
             * arg0 = path (cstr)
             * Emit: dlopen(arg0, RTLD_LAZY) */
            buf_printf(&out, "dlopen(%s, RTLD_LAZY)", arg_strs[0]);
            break;
        case BS_DLSYM:
            /* dlsym: dlsym(handle, symbol)
             * arg0 = handle (ptr<void>), arg1 = symbol (cstr)
             * Emit: dlsym(arg0, arg1) */
            buf_printf(&out, "dlsym(%s, %s)", arg_strs[0], arg_strs[1]);
            break;
        case BS_DLCLOSE:
            /* dlclose: dlclose(handle)
             * arg0 = handle (ptr<void>)
             * Emit: dlclose(arg0) */
            buf_printf(&out, "dlclose(%s)", arg_strs[0]);
            break;
        case BS_FUNC_CALL:
            /* Generic function call: c_op(arg0, arg1, ...) */
            buf_printf(&out, "%s(", spec->c_op);
            for (uint32_t i = 0; i < n; i++) {
                if (i > 0) buf_puts(&out, ", ");
                buf_puts(&out, arg_strs[i]);
            }
            buf_putc(&out, ')');
            break;
        default:
            /* unreachable for the shapes handled above */
            buf_puts(&out, "((void)0)");
            break;
    }
    buf_putc(&out, '\0');
    char *result = strdup(out.data);
    buf_free(&out);
    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
    free(arg_strs);
    return result;
}


/* ------------ entry points ------------ */

/* Phase H §1: Compute the C name of a typeclass instance's dictionary singleton.
 * Mirrors the type_suffix logic in EX_INSTANCE_DEF (emit_stmt) so that the name
 * is consistent wherever it needs to be referenced (EX_DICT emit_value, etc.).
 * Writes "dict_<TypeClass>_<typeargs>" into buf (size buflen). */
void emit_dict_name(char *buf, size_t buflen, const TypeClassInstance *inst) {
    const TypeClass *tc = inst->typeclass;
    char type_suffix[64] = "";
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_INT8:     component = "int8";     break;
            case TY_INT16:    component = "int16";    break;
            case TY_INT32:    component = "int32";    break;
            case TY_UINT8:    component = "uint8";    break;
            case TY_UINT16:   component = "uint16";   break;
            case TY_UINT32:   component = "uint32";   break;
            case TY_UINT64:   component = "uint64";   break;
            case TY_FLOAT:    component = "float";    break;
            case TY_FLOAT32:  component = "float32";  break;
            case TY_FLOAT64:  component = "float64";  break;
            case TY_STRUCT:
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.struct_.def &&
                         inst->type_args[i].as.struct_.def->name)
                    component = inst->type_args[i].as.struct_.def->name;
                break;
            case TY_APP: {
                const char *fn_part  = "T";
                const char *arg_part = "T";
                if (inst->type_args[i].as.app.fn) {
                    Type *fn = inst->type_args[i].as.app.fn;
                    if (fn->kind == TY_REC && fn->as.rec.name)
                        fn_part = fn->as.rec.name;
                    else if (fn->kind == TY_STRUCT && fn->as.struct_.def &&
                             fn->as.struct_.def->name)
                        fn_part = fn->as.struct_.def->name;
                }
                if (inst->type_args[i].as.app.arg) {
                    const char *n = type_name(*inst->type_args[i].as.app.arg);
                    if (n) arg_part = n;
                }
                char app_comp[48];
                snprintf(app_comp, sizeof(app_comp), "%s_%s", fn_part, arg_part);
                for (char *p = app_comp; *p; p++) {
                    if (!isalnum((unsigned char)*p)) *p = '_';
                }
                strncat(type_suffix, app_comp,
                        sizeof(type_suffix) - strlen(type_suffix) - 1);
                continue;
            }
            default: break;
        }
        char comp_buf[32];
        strncpy(comp_buf, component, sizeof(comp_buf) - 1);
        comp_buf[sizeof(comp_buf) - 1] = '\0';
        for (char *p = comp_buf; *p; p++) {
            if (!isalnum((unsigned char)*p)) *p = '_';
        }
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(buf, buflen, "dict_%s%s", tc->name->name, type_suffix);
}

/* Phase 19D: Collect bindings introduced within an expression (to identify outer captures).
 * defs/ndefs/cdefs is a growable array of Binding*. */
void collect_defined(const Expr *e, Binding ***defs, uint32_t *ndefs, uint32_t *cdefs) {
    if (!e) return;
    switch (e->kind) {
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                Binding *b = e->as.let_.bindings[i].binding;
                if (b) {
                    if (*ndefs >= *cdefs) {
                        *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                        *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                    }
                    (*defs)[(*ndefs)++] = b;
                }
                collect_defined(e->as.let_.bindings[i].init, defs, ndefs, cdefs);
            }
            collect_defined(e->as.let_.body, defs, ndefs, cdefs);
            break;
        }
        case EX_FN_DEF: {
            if (e->as.fn_def_.fn && e->as.fn_def_.fn->binding) {
                if (*ndefs >= *cdefs) {
                    *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                    *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                }
                (*defs)[(*ndefs)++] = e->as.fn_def_.fn->binding;
            }
            break;
        }
        case EX_FN: {
            if (e->as.fn_.fn && e->as.fn_.fn->binding) {
                if (*ndefs >= *cdefs) {
                    *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                    *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                }
                (*defs)[(*ndefs)++] = e->as.fn_.fn->binding;
            }
            break;
        }
        case EX_DO: {
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                collect_defined(e->as.do_.items[i], defs, ndefs, cdefs);
            break;
        }
        case EX_IF: {
            collect_defined(e->as.if_.cond, defs, ndefs, cdefs);
            collect_defined(e->as.if_.then_, defs, ndefs, cdefs);
            collect_defined(e->as.if_.else_or_null, defs, ndefs, cdefs);
            break;
        }
        case EX_WHILE: {
            collect_defined(e->as.while_.cond, defs, ndefs, cdefs);
            collect_defined(e->as.while_.body, defs, ndefs, cdefs);
            break;
        }
        case EX_HANDLE: {
            /* Walk the handle body to find bindings defined within it */
            if (e->as.handle_.handle)
                collect_defined(e->as.handle_.handle->body, defs, ndefs, cdefs);
            break;
        }
        default:
            break;
    }
}

/* Phase 19D: Collect free variable bindings in an expression (bindings not defined within it).
 * Returns a malloc'd array of outer bindings; caller frees. *n_out is set to count. */
Binding **collect_handle_captures(const Expr *body, uint32_t *n_out) {
    /* Step 1: collect all bindings defined within body */
    Binding **defs = NULL;
    uint32_t ndefs = 0, cdefs = 0;
    collect_defined(body, &defs, &ndefs, &cdefs);

    /* Step 2: walk body for EX_VAR refs not in defined set and not already captured */
    Binding **caps = NULL;
    uint32_t ncaps = 0, ccaps = 0;

    /* Use a simple recursive walk via a stack */
    /* We'll use a small helper approach: collect all EX_VAR bindings from body */
    /* then filter out those in defs */
    /* Simple DFS via recursion - collect all variable references */
    /* For now, implement a conservative approach: collect all EX_VAR that are not
     * defined within body and not globals */
    typedef struct ExprStack { const Expr *e; struct ExprStack *next; } ExprStack;
    ExprStack *stack = NULL;
    ExprStack initial = { body, NULL };
    stack = &initial;

    /* We need heap-allocated stack nodes for recursive traversal */
    /* Use a simple iterative approach with malloc'd stack */
    ExprStack **heap_nodes = NULL;
    uint32_t heap_count = 0, heap_cap = 0;

#define PUSH_EXPR(ex) do { \
    if (ex) { \
        if (heap_count >= heap_cap) { \
            heap_cap = (heap_cap == 0) ? 16 : heap_cap * 2; \
            heap_nodes = (ExprStack **)realloc(heap_nodes, heap_cap * sizeof(ExprStack *)); \
        } \
        ExprStack *_node = (ExprStack *)malloc(sizeof(ExprStack)); \
        _node->e = (ex); _node->next = stack; stack = _node; \
        heap_nodes[heap_count++] = _node; \
    } \
} while(0)

    /* Reset stack - just use heap nodes from the start */
    stack = NULL;
    PUSH_EXPR(body);

    while (stack) {
        ExprStack *top = stack;
        stack = top->next;
        const Expr *cur = top->e;
        if (!cur) continue;

        switch (cur->kind) {
            case EX_VAR: {
                Binding *b = cur->as.var.binding;
                if (!b || b->is_global || b->type.kind == TY_FN) break;
                /* Check if defined within body */
                bool in_defs = false;
                for (uint32_t i = 0; i < ndefs; i++) {
                    if (defs[i] == b) { in_defs = true; break; }
                }
                if (in_defs) break;
                /* Check if already captured */
                bool already = false;
                for (uint32_t i = 0; i < ncaps; i++) {
                    if (caps[i] == b) { already = true; break; }
                }
                if (!already) {
                    if (ncaps >= ccaps) {
                        ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                        caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                    }
                    caps[ncaps++] = b;
                }
                break;
            }
            case EX_LET: {
                for (uint32_t i = 0; i < cur->as.let_.n; i++)
                    PUSH_EXPR(cur->as.let_.bindings[i].init);
                PUSH_EXPR(cur->as.let_.body);
                break;
            }
            case EX_DO: {
                for (uint32_t i = 0; i < cur->as.do_.n; i++)
                    PUSH_EXPR(cur->as.do_.items[i]);
                break;
            }
            case EX_IF: {
                PUSH_EXPR(cur->as.if_.cond);
                PUSH_EXPR(cur->as.if_.then_);
                PUSH_EXPR(cur->as.if_.else_or_null);
                break;
            }
            case EX_WHILE: {
                PUSH_EXPR(cur->as.while_.cond);
                PUSH_EXPR(cur->as.while_.body);
                break;
            }
            case EX_SET: {
                /* The target binding is also a reference (it's being mutated) */
                if (cur->as.set_.target) {
                    Binding *b = cur->as.set_.target;
                    if (!b->is_global && b->type.kind != TY_FN) {
                        /* Check if defined within body */
                        bool in_defs = false;
                        for (uint32_t i = 0; i < ndefs; i++) {
                            if (defs[i] == b) { in_defs = true; break; }
                        }
                        if (!in_defs) {
                            bool already = false;
                            for (uint32_t i = 0; i < ncaps; i++) {
                                if (caps[i] == b) { already = true; break; }
                            }
                            if (!already) {
                                if (ncaps >= ccaps) {
                                    ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                                    caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                                }
                                caps[ncaps++] = b;
                            }
                        }
                    }
                }
                PUSH_EXPR(cur->as.set_.value);
                break;
            }
            case EX_CALL: {
                for (uint32_t i = 0; i < cur->as.call_.n_args; i++)
                    PUSH_EXPR(cur->as.call_.args[i]);
                PUSH_EXPR(cur->as.call_.fn_expr);
                break;
            }
            case EX_BUILTIN: {
                for (uint32_t i = 0; i < cur->as.builtin.n; i++)
                    PUSH_EXPR(cur->as.builtin.args[i]);
                break;
            }
            case EX_RETURN: {
                PUSH_EXPR(cur->as.return_.value);
                break;
            }
            case EX_PERFORM: {
                if (cur->as.perform_.perform) {
                    for (uint32_t i = 0; i < cur->as.perform_.perform->n_args; i++)
                        PUSH_EXPR(cur->as.perform_.perform->args[i]);
                }
                break;
            }
            case EX_HANDLE: {
                if (cur->as.handle_.handle) {
                    /* Only walk the handle body (not case bodies), since case bodies
                     * have their own local scope (effect params + k are local there). */
                    PUSH_EXPR(cur->as.handle_.handle->body);
                }
                break;
            }
            case EX_RESUME: {
                if (cur->as.resume_.resume) {
                    PUSH_EXPR(cur->as.resume_.resume->k);
                    PUSH_EXPR(cur->as.resume_.resume->value);
                }
                break;
            }
            case EX_DISCONTINUE: {
                if (cur->as.discontinue_.discontinue) {
                    PUSH_EXPR(cur->as.discontinue_.discontinue->k);
                    PUSH_EXPR(cur->as.discontinue_.discontinue->exception);
                }
                break;
            }
            case EX_CONT_PRED: {
                PUSH_EXPR(cur->as.cont_pred_.expr);
                break;
            }
            case EX_FN_TO_FAT: {
                /* A#1 auto-shim wrapper: descend to the inner fn so its closure
                 * captures are collected (KB-IDIOM-1). */
                PUSH_EXPR(cur->as.fn_to_fat_.inner);
                break;
            }
            case EX_REF: { PUSH_EXPR(cur->as.ref_.expr); break; }
            case EX_DEREF: { PUSH_EXPR(cur->as.deref_.expr); break; }
            case EX_GET_FIELD: {
                PUSH_EXPR(cur->as.get_field_.struct_expr);
                break;
            }
            case EX_SET_FIELD: {
                PUSH_EXPR(cur->as.set_field_.value);
                PUSH_EXPR(cur->as.set_field_.receiver);
                break;
            }
            case EX_BORROW_IMMUT: {
                PUSH_EXPR(cur->as.borrow_immut_.expr);
                break;
            }
            case EX_BORROW_MUT: {
                PUSH_EXPR(cur->as.borrow_mut_.expr);
                break;
            }
            case EX_DEFER: {
                /* Defer body's captures are pre-computed; add them directly.
                 * Walking the defer body would require separate collect_defined
                 * tracking for bindings defined inside the defer. */
                for (uint8_t j = 0; j < cur->as.defer_.n_captures; j++) {
                    Binding *db = cur->as.defer_.captures[j];
                    if (!db || db->is_global || db->type.kind == TY_FN) continue;
                    bool in_defs = false;
                    for (uint32_t k = 0; k < ndefs; k++) {
                        if (defs[k] == db) { in_defs = true; break; }
                    }
                    if (in_defs) continue;
                    bool already = false;
                    for (uint32_t k = 0; k < ncaps; k++) {
                        if (caps[k] == db) { already = true; break; }
                    }
                    if (!already) {
                        if (ncaps >= ccaps) {
                            ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                            caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                        }
                        caps[ncaps++] = db;
                    }
                }
                break;
            }
            case EX_CLOSURE: {
                /* KB-IDIOM-1: a closure constructed inside the handle body
                 * captures free variables from the enclosing scope. Its env-init
                 * (__t->field = <name>) references those names directly, so they
                 * must be threaded into the handle body's env (body_env) just like
                 * any other free variable. Do NOT walk the fn body (it has its own
                 * param/local scope); instead add the closure's pre-computed
                 * captures, which are already transitive (an outer closure captures
                 * everything its inner closures need from further out). */
                struct Closure *cl = cur->as.closure_.closure;
                if (cl) {
                    for (uint8_t j = 0; j < cl->n_captures; j++) {
                        Binding *cb = cl->captures[j];
                        if (!cb || cb->is_global || cb->type.kind == TY_FN) continue;
                        bool in_defs = false;
                        for (uint32_t k = 0; k < ndefs; k++) {
                            if (defs[k] == cb) { in_defs = true; break; }
                        }
                        if (in_defs) continue;
                        bool already = false;
                        for (uint32_t k = 0; k < ncaps; k++) {
                            if (caps[k] == cb) { already = true; break; }
                        }
                        if (!already) {
                            if (ncaps >= ccaps) {
                                ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                                caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                            }
                            caps[ncaps++] = cb;
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }
#undef PUSH_EXPR

    /* Free heap nodes */
    for (uint32_t i = 0; i < heap_count; i++) free(heap_nodes[i]);
    free(heap_nodes);
    free(defs);

    *n_out = ncaps;
    return caps;
}

/* ------------ Phase ACB: emit_carrier_bridge ------------ */

/* Returns true when concrete_ty fits entirely in an int64_t-wide slot and
 * the carrier stores the value inline (bitwise reinterpret) rather than as
 * a heap pointer.  Scalars with type_size_bytes == 8 qualify; pointer-sized
 * types (TY_CSTR, TY_PTR_VOID) are already int64_t-compatible so no
 * reinterpret is needed and this returns false for them.
 * Struct/ADT/composite types have size 0 in type_size_bytes and are always
 * pointer carriers. */
static bool carrier_is_inline(TypeKind k) {
    switch (k) {
        case TY_FLOAT:
        case TY_FLOAT64:
        case TY_FLOAT32:
        case TY_INT32:
        case TY_UINT32:
        case TY_INT16:
        case TY_UINT16:
        case TY_INT8:
        case TY_UINT8:
        case TY_BOOL:
            return true;
        default:
            return false;
    }
}

char *emit_carrier_bridge(EmitCtx *ctx, Buf *body,
                          char *src_str,
                          CarrierKind src_ck, CarrierKind sink_ck,
                          Type concrete_ty) {
    /* No crossing needed. */
    if (src_ck == sink_ck) return src_str;

    const char *cname = emit_type_c_name(ctx, concrete_ty);
    Buf out;
    buf_init(&out);

    if (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE) {
        if (carrier_is_inline(concrete_ty.kind)) {
            /* Inline scalar: union bitwise reinterpret int64_t -> concrete. */
            buf_printf(&out, "((union { int64_t s; %s d; }){.s = (%s)}).d",
                       cname, src_str);
        } else {
            /* Pointer carrier: dereference the heap pointer. */
            buf_printf(&out, "(*(%s *)(intptr_t)(%s))", cname, src_str);
        }
    } else {
        /* CK_CONCRETE -> CK_CARRIER */
        if (carrier_is_inline(concrete_ty.kind)) {
            /* Inline scalar: union bitwise reinterpret concrete -> int64_t. */
            buf_printf(&out, "((union { %s s; int64_t d; }){.s = (%s)}).d",
                       cname, src_str);
        } else {
            /* Aggregate: spill to a local, return its address as int64_t.
             * The spill local uses a fresh tmp so it stays live through the
             * expression that consumes the carrier value. */
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = %s;\n", cname, tmp, src_str);
            buf_printf(&out, "(int64_t)(intptr_t)(&%s)", tmp);
            free(tmp);
        }
    }

    free(src_str);
    char *result = strdup(out.data);
    buf_free(&out);
    return result;
}

/* ============================================================================
 * SYM1 (runtime-symbols-plan): per-TU interned-symbol codegen registry.
 *
 * Every distinct `:foo` referenced in a translation unit lowers to a single
 * static `struct __tur_sym` record in .rodata.  Two references to the same
 * keyword fold to the same record (and therefore the same pointer), so `:Sym`
 * equality is pointer equality and hashing is a single field load.
 *
 * The registry is keyed by the interned compile-time Symbol* identity (the
 * reader/symtab already shares one Symbol* across all `:foo` occurrences in a
 * unit), with a name-string fallback so independently-interned symbols with the
 * same text still fold.  Mangling percent-encodes non-identifier bytes so the
 * emitted C identifier is unique and ASCII-only even for punctuated keywords.
 * ============================================================================
 */
#include "hamt.h"   /* tur_hamt_hash_str -- precomputed xxHash64 per keyword */

typedef struct SymRecord {
    char         *name;    /* malloc'd owned copy of the name (see note) */
    uint32_t      len;     /* byte length, excluding NUL */
    uint64_t      hash;    /* precomputed xxHash64 of name */
    char         *cid;     /* malloc'd mangled C identifier, e.g. "__tur_sym_foo" */
} SymRecord;

static SymRecord *g_sym_records   = NULL;
static uint32_t   g_n_sym_records = 0;
static uint32_t   g_cap_sym_records = 0;
/* SYM5: set when this TU defines str->sym (i.e. sym-dynamic.tur was loaded),
 * which is the only thing that links the runtime intern table.  The seeding
 * constructor (which references tur_sym_register) is emitted only then, so a
 * program that uses literal :Sym values without str->sym never links the
 * table and never runs a startup constructor. */
static bool       g_sym_intern_used = false;

void sym_codegen_note_intern_used(void) { g_sym_intern_used = true; }

/* The name is strdup'd rather than borrowed from the source Symbol: in the
 * multi-file build path each TU is elaborated in its own arena which is freed
 * before the next TU, so a borrowed Symbol->name would dangle across the
 * registry's lifetime.  sym_codegen_reset() must still be called per TU to
 * keep dedup TU-local, but owning the string makes a missed reset a leak
 * rather than a use-after-free. */
void sym_codegen_reset(void) {
    for (uint32_t i = 0; i < g_n_sym_records; i++) {
        free(g_sym_records[i].cid);
        free(g_sym_records[i].name);
    }
    free(g_sym_records);
    g_sym_records     = NULL;
    g_n_sym_records   = 0;
    g_cap_sym_records = 0;
    g_sym_intern_used = false;
}

uint32_t sym_codegen_count(void) { return g_n_sym_records; }

/* Append the mangled form of `name` to `out`: each byte that is not [A-Za-z0-9_]
 * is emitted as `_HH` (uppercase hex of the byte), so the result is a valid,
 * collision-free C identifier suffix. */
static void sym_mangle_append(Buf *out, const char *name, uint32_t len) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            buf_putc(out, (char)c);
        } else {
            buf_putc(out, '_');
            buf_putc(out, hex[(c >> 4) & 0xF]);
            buf_putc(out, hex[c & 0xF]);
        }
    }
}

const char *sym_codegen_register(const Symbol *sym) {
    if (!sym) return NULL;
    /* Dedup by name text (the source Symbol* identity is not stable across the
     * per-TU arenas of a multi-file build). */
    for (uint32_t i = 0; i < g_n_sym_records; i++) {
        if (g_sym_records[i].len == sym->len &&
            strcmp(g_sym_records[i].name, sym->name) == 0)
            return g_sym_records[i].cid;
    }
    if (g_n_sym_records == g_cap_sym_records) {
        uint32_t nc = g_cap_sym_records ? g_cap_sym_records * 2 : 8;
        SymRecord *nr = (SymRecord *)realloc(g_sym_records, nc * sizeof(SymRecord));
        if (!nr) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_sym_records = nr;
        g_cap_sym_records = nc;
    }
    Buf cid; buf_init(&cid);
    buf_puts(&cid, "__tur_sym_");
    sym_mangle_append(&cid, sym->name, sym->len);
    buf_putc(&cid, '\0');
    SymRecord *rec = &g_sym_records[g_n_sym_records++];
    rec->name = strdup(sym->name);
    rec->len  = sym->len;
    rec->hash = tur_hamt_hash_str(sym->name);
    rec->cid  = strdup(cid.data);
    buf_free(&cid);
    return rec->cid;
}

/* Emit the runtime symbol struct + one record per distinct keyword.
 * `struct __tur_sym` is emitted unconditionally when -Xsymbols is on (so that
 * inline-C in stdlib sym helpers always sees the layout), and also whenever any
 * record was registered.
 *
 * SYM2 (cross-TU interning): when `external_weak` is true (the multi-file /
 * separate-compilation build path), each record is emitted with external weak
 * linkage under its stable mangled name.  Two TUs that both reference `:foo`
 * emit identical `__tur_sym_foo` definitions; the linker folds the weak
 * duplicates to a single object, so `:foo` is one pointer across the whole
 * program.  In single-file / emit-c mode (`external_weak` false) the records
 * stay `static`, keeping the output self-contained. */
void sym_codegen_emit(Buf *out, bool external_weak) {
    extern bool g_symbols_enabled;
    if (g_n_sym_records == 0 && !g_symbols_enabled) return;
    buf_puts(out,
        "/* SYM1 (runtime-symbols-plan): interned runtime symbol records. */\n"
        "#ifndef TUR_SYM_DEFINED\n"
        "#define TUR_SYM_DEFINED 1\n"
        "struct __tur_sym {\n"
        "    uint64_t hash;   /* precomputed xxHash64 of name */\n"
        "    uint32_t len;    /* byte length, excluding NUL */\n"
        "    uint32_t _pad;\n"
        "    char     name[]; /* NUL-terminated UTF-8 */\n"
        "};\n"
        "#endif\n");
    const char *storage = external_weak
        ? "__attribute__((weak)) const"   /* SYM2: linker folds same-named dups */
        : "static const";
    for (uint32_t i = 0; i < g_n_sym_records; i++) {
        SymRecord *r = &g_sym_records[i];
        /* The record has a flexible array member, so use a sized anonymous
         * struct for the definition and cast to const struct __tur_sym * at use. */
        buf_printf(out,
            "%s struct { uint64_t hash; uint32_t len; uint32_t _pad; char name[%u]; } %s = { ",
            storage, r->len + 1, r->cid);
        buf_printf(out, "%lluULL, %uu, 0u, ",
                   (unsigned long long)r->hash, r->len);
        /* Emit the name as a C string literal (escape conservatively). */
        buf_putc(out, '"');
        for (uint32_t j = 0; j < r->len; j++) {
            unsigned char c = (unsigned char)r->name[j];
            if (c == '"' || c == '\\') { buf_putc(out, '\\'); buf_putc(out, (char)c); }
            else if (c == '\n') buf_puts(out, "\\n");
            else if (c == '\t') buf_puts(out, "\\t");
            else if (c < 0x20)  buf_printf(out, "\\%03o", c);
            else                buf_putc(out, (char)c);
        }
        buf_puts(out, "\" };\n");
    }
    /* SYM5: seed the runtime intern table with this TU's static records so that
     * str->sym("foo") returns the same pointer as the literal :foo.  The
     * registrar lives in src/runtime/symbols.c, auto-linked into -Xsymbols
     * programs via the marker in stdlib/sym.tur's str->sym.  First registration
     * of a name wins, so weak-folded cross-TU records register idempotently.
     * Gated on str->sym being defined in this TU (g_sym_intern_used): only then
     * is the table (and tur_sym_register) linked, and only then can anything
     * query the table -- so a literal-only program emits no constructor. */
    if (g_n_sym_records > 0 && g_sym_intern_used) {
        buf_puts(out, "extern void tur_sym_register(const struct __tur_sym *);\n");
        buf_puts(out, "__attribute__((constructor)) static void __tur_sym_seed(void) {\n");
        for (uint32_t i = 0; i < g_n_sym_records; i++) {
            buf_printf(out, "    tur_sym_register((const struct __tur_sym *)&%s);\n",
                       g_sym_records[i].cid);
        }
        buf_puts(out, "}\n");
    }
    if (g_n_sym_records > 0) buf_putc(out, '\n');
}
